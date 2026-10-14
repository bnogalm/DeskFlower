#include "node.h"

#include "buildinfo.h"
#include "clock.h"

#include <QCborValue>
#include <QDateTime>
#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSysInfo>
#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcNode, "deskflower.node")

namespace df {

namespace {
const QByteArray kPrologue("deskflower/1");
constexpr int kTickMs = 20;
constexpr int kKeepaliveMs = 1000;
constexpr int kSessionTimeoutMs = 6000;
constexpr int kRekeyMs = 120000;
constexpr int kPreviousSessionGraceMs = 15000;
constexpr int kUnconfirmedSessionMs = 10000;
constexpr int kUntrustedExpiryMs = 15000;
constexpr int kAddressStaleMs = 5000;
constexpr int kBeaconExploringMs = 1000;
constexpr int kBeaconIdleMs = 3000;

const QString kT = QStringLiteral("t");
const QString kB = QStringLiteral("b");
const QString kI = QStringLiteral("i");
const QString kN = QStringLiteral("n");
const QString kK = QStringLiteral("k");
const QString kO = QStringLiteral("o");
const QString kP = QStringLiteral("p");
const QString kX = QStringLiteral("x");
const QString kQ = QStringLiteral("q");

QHostAddress normalized(const QHostAddress &addr)
{
    bool ok = false;
    const quint32 v4 = addr.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : addr;
}
} // namespace

struct Node::Peer {
    Peer(QByteArray peerId, ReliableChannel::Sender sender) : id(std::move(peerId)), reliable(std::move(sender)) {}

    QByteArray id;
    QString name;
    QString os;
    QString version;
    QString platform;
    QString updateVersion;
    QString updatePlatform;
    QByteArray publicKey;
    QHostAddress addr;
    quint16 port = 0;
    bool exploring = false;
    bool connected = false;
    qint64 lastSeen = -1;
    qint64 addrSeen = 0;
    quint16 tcpPort = 0; // 0: same as the UDP port

    std::unique_ptr<SecureSession> current;
    std::unique_ptr<SecureSession> previous;
    std::unique_ptr<SecureSession> next; // responder side, waiting for the first data packet
    qint64 previousSince = 0;

    std::unique_ptr<noise::IKInitiator> handshake;
    quint32 handshakeIndex = 0;
    qint64 handshakeSentAt = 0;
    int handshakeAttempts = 0;
    quint64 lastHandshakeTimestamp = 0;

    ReliableChannel reliable;
    QByteArray remoteBoot;
};

Node::Node(Options options, QObject *parent) : QObject(parent), m_options(std::move(options)) {}

Node::~Node() = default;

bool Node::start(QString *error)
{
    if (!crypto::init()) {
        *error = QStringLiteral("libsodium init failed");
        return false;
    }
    if (!m_identity.loadOrCreate(m_options.configDir, error))
        return false;
    m_store.load(m_options.configDir);
    m_bootId = crypto::randomBytes(8);

    for (const TrustedPeer &t : m_store.all()) {
        Peer &p = peerFor(t.id);
        p.publicKey = t.publicKey;
        p.name = t.name;
    }

    if (!m_socket.bind(QHostAddress::AnyIPv4, m_options.dataPort) && !m_socket.bind(QHostAddress::AnyIPv4, 0)) {
        *error = QStringLiteral("Cannot bind UDP socket: %1").arg(m_socket.errorString());
        return false;
    }
    // Large transfers (clipboard images) arrive in bursts: the default UDP buffers (64 KB on
    // Windows) would drop most of them.
    m_socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
    m_socket.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 4 * 1024 * 1024);
    m_socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    m_socket.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    connect(&m_socket, &QUdpSocket::readyRead, this, &Node::onDataReadyRead);

    // Bulk transfers over TCP: preferably on the same port number, otherwise on any free port
    // (Windows reserves TCP ranges for Hyper-V/WSL). The port is announced in beacons and hello.
    if (m_tcp.listen(QHostAddress::AnyIPv4, m_socket.localPort()) || m_tcp.listen(QHostAddress::AnyIPv4, 0)) {
        connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_tcp.nextPendingConnection()) {
                auto trust = [this](const QByteArray &remoteStatic) -> std::optional<QByteArray> {
                    const QByteArray id = crypto::deviceIdFromPublicKey(remoteStatic);
                    const TrustedPeer *peer = m_store.find(id);
                    if (!peer || peer->publicKey != remoteStatic)
                        return std::nullopt;
                    return id;
                };
                SecureStream *stream = SecureStream::accept(socket, m_identity.keys(), trust, this);
                connect(stream, &SecureStream::established, this, [this, stream] { emit streamAccepted(stream); });
                // Accepted streams live until they close.
                connect(stream, &SecureStream::closed, stream, &QObject::deleteLater);
            }
        });
    } else {
        qCWarning(lcNode) << "Cannot listen on TCP:" << m_tcp.errorString() << "(file transfers disabled)";
    }

    if (m_options.enableDiscovery) {
        if (m_discovery.bind(QHostAddress::AnyIPv4, m_options.discoveryPort,
                             QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            bool joined = false;
            for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
                if (iface.flags().testFlags(QNetworkInterface::IsUp | QNetworkInterface::CanMulticast))
                    joined |= m_discovery.joinMulticastGroup(wire::multicastGroup(), iface);
            }
            if (!joined)
                m_discovery.joinMulticastGroup(wire::multicastGroup());
            connect(&m_discovery, &QUdpSocket::readyRead, this, &Node::onDiscoveryReadyRead);
        } else {
            qCWarning(lcNode) << "Discovery disabled, cannot bind port" << m_options.discoveryPort
                              << m_discovery.errorString();
        }
    }

    m_pairing = std::make_unique<PairingManager>(
        m_identity, [this](const QByteArray &packet, const QHostAddress &addr, quint16 port) {
            m_socket.writeDatagram(packet, addr, port);
        });
    connect(m_pairing.get(), &PairingManager::pinGenerated, this, &Node::pairingPinGenerated);
    connect(m_pairing.get(), &PairingManager::pinRequired, this, &Node::pairingPinRequired);
    connect(m_pairing.get(), &PairingManager::succeeded, this, &Node::onPairingSucceeded);
    connect(m_pairing.get(), &PairingManager::failed, this,
            [this](const QByteArray &id, const QString &reason) { emit pairingFinished(id, false, reason); });

    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &Node::onTick);
    m_tick.start();

    qCInfo(lcNode) << "Node" << m_identity.name() << m_identity.fingerprint() << "listening on UDP"
                   << m_socket.localPort();
    return true;
}

void Node::setUpdateOffer(const QString &version, const QString &platform)
{
    if (m_updateVersion == version && m_updatePlatform == platform)
        return;
    m_updateVersion = version;
    m_updatePlatform = platform;
    for (auto &[id, peer] : m_peers) {
        if (peer->connected)
            sendMessage(id, helloMessage());
    }
}

bool Node::setName(const QString &name)
{
    if (!m_identity.setName(name))
        return false;
    for (auto &[id, peer] : m_peers) {
        if (peer->connected)
            sendMessage(id, helloMessage());
    }
    m_lastBeacon = -1000000;
    return true;
}

void Node::setExploring(bool on)
{
    m_exploring = on;
    m_explorationStarted = nowMs();
    if (m_pairing)
        m_pairing->setAcceptingRequests(on);
    m_lastBeacon = -1000000;
    emit explorationChanged(on);
    emit peersChanged();
}

Node::Peer &Node::peerFor(const QByteArray &id)
{
    auto it = m_peers.find(id);
    if (it == m_peers.end()) {
        auto sender = [this, id](const QByteArray &inner) {
            auto found = m_peers.find(id);
            if (found != m_peers.end())
                sendInner(*found->second, inner);
        };
        it = m_peers.emplace(id, std::make_unique<Peer>(id, sender)).first;
    }
    return *it->second;
}

QList<PeerView> Node::peers() const
{
    QList<PeerView> out;
    const qint64 now = nowMs();
    for (const auto &[id, p] : m_peers) {
        const bool trusted = m_store.contains(id);
        if (!trusted && (p->lastSeen < 0 || now - p->lastSeen > kUntrustedExpiryMs))
            continue;
        PeerView v;
        v.id = id;
        v.name = p->name;
        v.os = p->os;
        v.fingerprint = p->publicKey.isEmpty() ? QString() : crypto::fingerprint(p->publicKey);
        v.address = p->addr;
        v.port = p->port;
        v.trusted = trusted;
        v.allowView = isViewAllowed(id);
        v.connected = p->connected;
        v.exploring = p->exploring && p->lastSeen >= 0 && now - p->lastSeen < kUntrustedExpiryMs;
        if (trusted) {
            v.version = p->version;
            v.platform = p->platform;
            v.updateVersion = p->updateVersion;
            v.updatePlatform = p->updatePlatform;
        }
        out.append(v);
    }
    return out;
}

bool Node::isConnected(const QByteArray &peerId) const
{
    auto it = m_peers.find(peerId);
    return it != m_peers.end() && it->second->connected;
}

bool Node::isViewAllowed(const QByteArray &peerId) const
{
    const TrustedPeer *trusted = m_store.find(peerId);
    return trusted && trusted->allowView;
}

void Node::setViewAllowed(const QByteArray &peerId, bool on)
{
    if (!m_store.contains(peerId) || isViewAllowed(peerId) == on)
        return;
    m_store.setAllowView(peerId, on);
    emit peersChanged();
}

void Node::addManualPeer(const QHostAddress &addr, quint16 port)
{
    m_socket.writeDatagram(beaconPacket(true), addr, port ? port : wire::kDefaultDataPort);
}

bool Node::requestPairing(const QByteArray &peerId)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || m_store.contains(peerId) || it->second->addr.isNull())
        return false;
    return m_pairing->start(peerId, it->second->addr, it->second->port);
}

bool Node::submitPin(const QByteArray &peerId, const QString &pin) { return m_pairing->submitPin(peerId, pin); }

void Node::cancelPairing(const QByteArray &peerId) { m_pairing->cancel(peerId); }

void Node::unpair(const QByteArray &peerId)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || !m_store.contains(peerId))
        return;
    Peer &p = *it->second;
    if (p.connected) {
        QCborMap m;
        m.insert(kT, QStringLiteral("unpair"));
        p.reliable.send(QCborValue(m).toCbor(), nowMs()); // best effort, sent immediately
    }
    m_store.remove(peerId);
    dropSessions(p);
    p.reliable.reset();
    p.remoteBoot.clear();
    emit peersChanged();
}

bool Node::sendMessage(const QByteArray &peerId, const QCborMap &message)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || !m_store.contains(peerId))
        return false;
    it->second->reliable.send(QCborValue(message).toCbor(), nowMs());
    return true;
}

bool Node::sendDatagram(const QByteArray &peerId, const QByteArray &data)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || !it->second->connected)
        return false;
    sendInner(*it->second, char(wire::Inner::Datagram) + data);
    return true;
}

SecureStream *Node::openStream(const QByteArray &peerId)
{
    auto it = m_peers.find(peerId);
    const TrustedPeer *trusted = m_store.find(peerId);
    if (it == m_peers.end() || !trusted || it->second->addr.isNull())
        return nullptr;
    const Peer &p = *it->second;
    return SecureStream::connectTo(m_identity.keys(), trusted->publicKey, peerId, p.addr,
                                   p.tcpPort ? p.tcpPort : p.port);
}

QCborMap Node::helloMessage() const
{
    QCborMap m;
    m.insert(kT, QStringLiteral("hello"));
    m.insert(kN, m_identity.name());
    m.insert(kO, QSysInfo::productType());
    m.insert(QStringLiteral("tp"), m_tcp.serverPort());
    m.insert(QStringLiteral("v"), build::version());
    m.insert(QStringLiteral("pl"), build::platform());
    if (!m_updateVersion.isEmpty()) {
        m.insert(QStringLiteral("uv"), m_updateVersion);
        m.insert(QStringLiteral("upl"), m_updatePlatform);
    }
    return m;
}

// --- Receiving ---------------------------------------------------------------------------------

void Node::onDataReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket.receiveDatagram(wire::kMaxPacket * 2);
        handlePacket(dg.data(), normalized(dg.senderAddress()), quint16(dg.senderPort()), true);
    }
}

void Node::onDiscoveryReadyRead()
{
    while (m_discovery.hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_discovery.receiveDatagram(wire::kMaxPacket * 2);
        handlePacket(dg.data(), normalized(dg.senderAddress()), quint16(dg.senderPort()), false);
    }
}

void Node::handlePacket(const QByteArray &packet, const QHostAddress &from, quint16 port, bool viaDataSocket)
{
    const auto type = wire::parseType(packet);
    if (!type)
        return;
    switch (*type) {
    case wire::Type::Beacon:
        if (auto msg = wire::parseCborPacket(packet))
            handleBeacon(*msg, from, port, viaDataSocket);
        break;
    case wire::Type::PairRequest:
    case wire::Type::PairChallenge:
    case wire::Type::PairMsg1:
    case wire::Type::PairMsg2:
    case wire::Type::PairMsg3:
    case wire::Type::PairDone:
    case wire::Type::PairAbort:
        if (viaDataSocket) {
            if (auto msg = wire::parseCborPacket(packet))
                m_pairing->handlePacket(*type, *msg, from, port);
        }
        break;
    case wire::Type::HandshakeInit:
        if (viaDataSocket)
            handleHandshakeInit(packet, from, port);
        break;
    case wire::Type::HandshakeResp:
        if (viaDataSocket)
            handleHandshakeResp(packet, from, port);
        break;
    case wire::Type::Data:
        if (viaDataSocket)
            handleData(packet, from, port);
        break;
    }
}

void Node::handleBeacon(const QCborMap &msg, const QHostAddress &from, quint16 port, bool viaDataSocket)
{
    const QByteArray id = msg.value(kI).toByteArray();
    const QByteArray pub = msg.value(kK).toByteArray();
    if (id == m_identity.id() || pub.size() != crypto::kKeySize || crypto::deviceIdFromPublicKey(pub) != id)
        return;

    if (viaDataSocket && msg.value(kQ).toBool())
        m_socket.writeDatagram(beaconPacket(false), from, port);

    const bool trusted = m_store.contains(id);
    Peer &p = peerFor(id);
    const bool wasVisible = p.lastSeen >= 0 && nowMs() - p.lastSeen < kUntrustedExpiryMs;
    const QString name = msg.value(kN).toString().left(64);
    const QString os = msg.value(kO).toString().left(32);
    const bool exploring = msg.value(kX).toBool();
    // The beacon's source port is the sender's data socket.
    const quint16 dataPort = port;

    if (!wasVisible)
        qCInfo(lcNode) << "Discovered" << name << from.toString() << port << (trusted ? "(trusted)" : "");
    bool changed = !wasVisible || p.name != name || p.os != os || p.exploring != exploring;
    p.lastSeen = nowMs();
    p.name = name;
    p.os = os;
    p.exploring = exploring;
    if (!p.connected) // while connected, the authenticated hello is the source of truth
        p.tcpPort = quint16(msg.value(QStringLiteral("tp")).toInteger());
    if (!trusted)
        p.publicKey = pub;
    else
        m_store.rename(id, name);

    // A host with several interfaces sends a beacon from each one: stick to the current address
    // while it keeps answering. Beacons are unauthenticated, so they never move a live session.
    const qint64 now = nowMs();
    if (p.addr == from && p.port == dataPort) {
        p.addrSeen = now;
    } else if (!p.connected && (p.addr.isNull() || now - p.addrSeen > kAddressStaleMs)) {
        p.addr = from;
        p.port = dataPort;
        p.addrSeen = now;
        changed = true;
    }
    if (changed)
        emit peersChanged();
}

void Node::handleHandshakeInit(const QByteArray &packet, const QHostAddress &from, quint16 port)
{
    if (packet.size() < wire::kHeaderSize + 4 + noise::kMessage1Overhead)
        return;
    const quint32 senderIndex = wire::readU32(packet, wire::kHeaderSize);

    noise::IKResponder responder(m_identity.keys(), kPrologue);
    QByteArray payload;
    const auto remoteStatic = responder.readMessage1(packet.mid(wire::kHeaderSize + 4), &payload);
    if (!remoteStatic)
        return;
    const QByteArray id = crypto::deviceIdFromPublicKey(*remoteStatic);
    const TrustedPeer *trusted = m_store.find(id);
    if (!trusted || trusted->publicKey != *remoteStatic)
        return;

    const QCborMap info = QCborValue::fromCbor(payload).toMap();
    const quint64 timestamp = quint64(info.value(kT).toInteger());
    Peer &p = peerFor(id);
    if (timestamp <= p.lastHandshakeTimestamp)
        return; // replayed handshake
    if (p.handshake && m_identity.id() < id)
        return; // simultaneous initiation: ours wins, the peer will answer it

    p.lastHandshakeTimestamp = timestamp;
    if (p.handshake) {
        m_indexToPeer.erase(p.handshakeIndex);
        p.handshake.reset();
    }

    QCborMap reply;
    reply.insert(kB, m_bootId);
    QByteArray message2;
    const auto keys = responder.writeMessage2(QCborValue(reply).toCbor(), &message2);
    if (!keys)
        return;

    const QByteArray remoteBoot = info.value(kB).toByteArray();
    if (!p.remoteBoot.isEmpty() && p.remoteBoot != remoteBoot)
        p.reliable.reset(); // the peer restarted
    p.remoteBoot = remoteBoot;

    releaseIndex(p.next);
    const quint32 localIndex = allocateIndex(id);
    p.next = std::make_unique<SecureSession>(localIndex, senderIndex, *keys, false, nowMs());
    setPeerAddress(p, from, port);

    QByteArray out = wire::header(wire::Type::HandshakeResp);
    wire::appendU32(out, localIndex);
    wire::appendU32(out, senderIndex);
    out += message2;
    m_socket.writeDatagram(out, from, port);
}

void Node::handleHandshakeResp(const QByteArray &packet, const QHostAddress &from, quint16 port)
{
    if (packet.size() < wire::kHeaderSize + 8 + noise::kMessage2Overhead)
        return;
    const quint32 senderIndex = wire::readU32(packet, wire::kHeaderSize);
    const quint32 receiverIndex = wire::readU32(packet, wire::kHeaderSize + 4);
    auto idIt = m_indexToPeer.find(receiverIndex);
    if (idIt == m_indexToPeer.end())
        return;
    Peer &p = peerFor(idIt->second);
    if (!p.handshake || p.handshakeIndex != receiverIndex)
        return;

    QByteArray payload;
    const auto keys = p.handshake->readMessage2(packet.mid(wire::kHeaderSize + 8), &payload);
    if (!keys)
        return;
    p.handshake.reset();
    p.handshakeAttempts = 0;

    const QByteArray remoteBoot = QCborValue::fromCbor(payload).toMap().value(kB).toByteArray();
    if (!p.remoteBoot.isEmpty() && p.remoteBoot != remoteBoot)
        p.reliable.reset();
    p.remoteBoot = remoteBoot;

    const qint64 now = nowMs();
    releaseIndex(p.previous);
    p.previous = std::move(p.current);
    p.previousSince = now;
    p.current = std::make_unique<SecureSession>(receiverIndex, senderIndex, *keys, true, now);
    setPeerAddress(p, from, port);
    sendInner(p, QByteArray(1, char(wire::Inner::Keepalive))); // confirms the session to the responder
    setConnected(p, true);
}

void Node::handleData(const QByteArray &packet, const QHostAddress &from, quint16 port)
{
    if (packet.size() < wire::kDataHeaderSize + crypto::kTagSize)
        return;
    const quint32 index = wire::readU32(packet, wire::kHeaderSize);
    auto idIt = m_indexToPeer.find(index);
    if (idIt == m_indexToPeer.end())
        return;
    Peer &p = peerFor(idIt->second);
    const qint64 now = nowMs();

    std::optional<QByteArray> inner;
    if (p.current && p.current->localIndex() == index) {
        inner = p.current->open(packet, now);
    } else if (p.next && p.next->localIndex() == index) {
        inner = p.next->open(packet, now);
        if (inner) {
            releaseIndex(p.previous);
            p.previous = std::move(p.current);
            p.previousSince = now;
            p.current = std::move(p.next);
        }
    } else if (p.previous && p.previous->localIndex() == index) {
        inner = p.previous->open(packet, now);
    }
    if (!inner || inner->isEmpty())
        return;

    setPeerAddress(p, from, port);
    setConnected(p, true);
    handleInner(p, *inner);
}

void Node::handleInner(Peer &peer, const QByteArray &inner)
{
    switch (wire::Inner(quint8(inner[0]))) {
    case wire::Inner::Keepalive:
        break;
    case wire::Inner::Reliable:
    case wire::Inner::Ack: {
        const QByteArray id = peer.id;
        const QList<QByteArray> messages = peer.reliable.receive(inner, nowMs());
        for (const QByteArray &raw : messages) {
            const QCborMap msg = QCborValue::fromCbor(raw).toMap();
            const QString type = msg.value(kT).toString();
            if (type == QLatin1String("hello") || type == QLatin1String("unpair"))
                handleControlMessage(peer, msg);
            else
                emit messageReceived(id, msg);
            if (m_peers.find(id) == m_peers.end())
                return;
        }
        break;
    }
    case wire::Inner::Datagram:
        emit datagramReceived(peer.id, inner.mid(1));
        break;
    }
}

void Node::handleControlMessage(Peer &peer, const QCborMap &msg)
{
    const QString type = msg.value(kT).toString();
    if (type == QLatin1String("hello")) {
        peer.name = msg.value(kN).toString().left(64);
        peer.os = msg.value(kO).toString().left(32);
        peer.tcpPort = quint16(msg.value(QStringLiteral("tp")).toInteger());
        // Display and offer only: an update is checked against its signed manifest.
        peer.version = msg.value(QStringLiteral("v")).toString().left(32);
        peer.platform = msg.value(QStringLiteral("pl")).toString().left(32);
        peer.updateVersion = msg.value(QStringLiteral("uv")).toString().left(32);
        peer.updatePlatform = msg.value(QStringLiteral("upl")).toString().left(32);
        m_store.rename(peer.id, peer.name);
        emit peersChanged();
    } else if (type == QLatin1String("unpair")) {
        m_store.remove(peer.id);
        dropSessions(peer);
        peer.remoteBoot.clear();
        emit peersChanged();
    }
}

void Node::onPairingSucceeded(const TrustedPeer &trusted, const QHostAddress &addr, quint16 port)
{
    m_store.upsert(trusted);
    Peer &p = peerFor(trusted.id);
    p.publicKey = trusted.publicKey;
    p.name = trusted.name;
    p.reliable.reset();
    p.remoteBoot.clear();
    p.lastHandshakeTimestamp = 0;
    setPeerAddress(p, addr, port);
    emit pairingFinished(trusted.id, true, QString());
    emit peersChanged();
    initiateHandshake(p);
}

// --- Sending / housekeeping --------------------------------------------------------------------

QByteArray Node::beaconPacket(bool query) const
{
    QCborMap m;
    m.insert(kI, m_identity.id());
    m.insert(kN, m_identity.name());
    m.insert(kK, m_identity.keys().publicKey);
    m.insert(kO, QSysInfo::productType());
    m.insert(kP, m_socket.localPort());
    m.insert(QStringLiteral("tp"), m_tcp.serverPort());
    m.insert(kX, m_exploring);
    if (query)
        m.insert(kQ, true);
    return wire::cborPacket(wire::Type::Beacon, m);
}

void Node::sendBeacons()
{
    const QByteArray packet = beaconPacket(false);
    if (m_options.enableDiscovery) {
        for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
            const auto flags = iface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning))
                continue;
            if (flags.testFlag(QNetworkInterface::CanMulticast)) {
                m_socket.setMulticastInterface(iface);
                m_socket.writeDatagram(packet, wire::multicastGroup(), m_options.discoveryPort);
            }
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (!entry.broadcast().isNull())
                    m_socket.writeDatagram(packet, entry.broadcast(), m_options.discoveryPort);
            }
        }
    }
    // Unicast queries to trusted peers we know an address for but are not connected to.
    const QByteArray query = beaconPacket(true);
    for (const auto &[id, p] : m_peers) {
        if (!p->connected && !p->addr.isNull() && m_store.contains(id))
            m_socket.writeDatagram(query, p->addr, p->port);
    }
}

void Node::initiateHandshake(Peer &peer)
{
    if (peer.publicKey.isEmpty() || peer.addr.isNull())
        return;
    if (peer.handshake)
        m_indexToPeer.erase(peer.handshakeIndex);

    static quint64 lastTimestamp = 0;
    lastTimestamp = std::max<quint64>(lastTimestamp + 1, quint64(QDateTime::currentMSecsSinceEpoch()));
    QCborMap payload;
    payload.insert(kT, qint64(lastTimestamp));
    payload.insert(kB, m_bootId);

    peer.handshake = std::make_unique<noise::IKInitiator>(m_identity.keys(), peer.publicKey, kPrologue);
    const QByteArray message1 = peer.handshake->writeMessage1(QCborValue(payload).toCbor());
    if (message1.isEmpty()) {
        peer.handshake.reset();
        return;
    }
    peer.handshakeIndex = allocateIndex(peer.id);
    peer.handshakeSentAt = nowMs();
    ++peer.handshakeAttempts;

    QByteArray packet = wire::header(wire::Type::HandshakeInit);
    wire::appendU32(packet, peer.handshakeIndex);
    packet += message1;
    m_socket.writeDatagram(packet, peer.addr, peer.port);
}

void Node::sendInner(Peer &peer, const QByteArray &inner)
{
    if (!peer.current || !peer.current->isConfirmed() || peer.addr.isNull())
        return;
    m_socket.writeDatagram(peer.current->seal(inner, nowMs()), peer.addr, peer.port);
}

void Node::setConnected(Peer &peer, bool connected)
{
    if (peer.connected == connected)
        return;
    peer.connected = connected;
    if (connected) {
        qCInfo(lcNode) << "Connected to" << peer.name << peer.addr.toString() << peer.port;
        sendMessage(peer.id, helloMessage());
        emit peerConnected(peer.id);
    } else {
        qCInfo(lcNode) << "Disconnected from" << peer.name;
        emit peerDisconnected(peer.id);
    }
    emit peersChanged();
}

void Node::dropSessions(Peer &peer)
{
    releaseIndex(peer.current);
    releaseIndex(peer.previous);
    releaseIndex(peer.next);
    peer.current.reset();
    peer.previous.reset();
    peer.next.reset();
    if (peer.handshake) {
        m_indexToPeer.erase(peer.handshakeIndex);
        peer.handshake.reset();
    }
    setConnected(peer, false);
}

void Node::setPeerAddress(Peer &peer, const QHostAddress &addr, quint16 port)
{
    peer.addrSeen = nowMs();
    if (peer.addr != addr || peer.port != port) {
        peer.addr = addr;
        peer.port = port;
        emit peersChanged();
    }
}

quint32 Node::allocateIndex(const QByteArray &peerId)
{
    quint32 index = 0;
    do {
        const QByteArray r = crypto::randomBytes(4);
        index = wire::readU32(r, 0);
    } while (index == 0 || m_indexToPeer.count(index));
    m_indexToPeer[index] = peerId;
    return index;
}

void Node::releaseIndex(const std::unique_ptr<SecureSession> &session)
{
    if (session)
        m_indexToPeer.erase(session->localIndex());
}

void Node::onTick()
{
    const qint64 now = nowMs();

    if (m_exploring && now - m_explorationStarted > m_options.explorationTimeoutMs)
        setExploring(false);

    if (now - m_lastBeacon >= (m_exploring ? kBeaconExploringMs : kBeaconIdleMs)) {
        m_lastBeacon = now;
        sendBeacons();
    }

    bool listChanged = false;
    for (auto it = m_peers.begin(); it != m_peers.end();) {
        Peer &p = *it->second;
        if (!m_store.contains(p.id)) {
            dropSessions(p);
            if (p.lastSeen < 0 || now - p.lastSeen > kUntrustedExpiryMs * 4) {
                it = m_peers.erase(it);
                listChanged = true;
                continue;
            }
            ++it;
            continue;
        }

        if (p.current && now - p.current->lastReceived() > kSessionTimeoutMs) {
            qCInfo(lcNode) << "Session timeout with" << p.name;
            dropSessions(p);
        }
        if (p.previous && now - p.previousSince > kPreviousSessionGraceMs) {
            releaseIndex(p.previous);
            p.previous.reset();
        }
        if (p.next && now - p.next->createdAt() > kUnconfirmedSessionMs) {
            releaseIndex(p.next);
            p.next.reset();
        }

        const bool needHandshake =
            !p.current || (p.current->isInitiator() && now - p.current->createdAt() > kRekeyMs) ||
            (!p.current->isInitiator() && now - p.current->createdAt() > kRekeyMs + 15000);
        if (needHandshake && !p.addr.isNull()) {
            const int retryMs = p.handshakeAttempts < 5 ? 1000 : 5000;
            if (!p.handshake || now - p.handshakeSentAt > retryMs)
                initiateHandshake(p);
        }

        p.reliable.tick(now);
        if (p.current && now - p.current->lastSent() > kKeepaliveMs)
            sendInner(p, QByteArray(1, char(wire::Inner::Keepalive)));
        ++it;
    }
    if (listChanged)
        emit peersChanged();
}

} // namespace df
