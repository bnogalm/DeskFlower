#include "node.h"

#include "buildinfo.h"
#include "clock.h"

#include <QCborArray>
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
// Simultaneous initiation: ours wins only while it is this fresh (unanswered longer, it may be
// going nowhere, and the peer's authenticated one is taken instead).
constexpr int kHandshakeTieMs = 1500;
// Boot ids of a peer whose initiation counters are remembered (replay guard, see Peer::Epoch).
constexpr int kMaxHandshakeEpochs = 8;
// Limits on unauthenticated traffic and on what it can make us keep.
constexpr int kMaxUntrustedPeers = 64;
constexpr int kQueryAnswerMs = 500; // one answer to beacon queries per sender and interval
constexpr int kMaxPendingStreams = 16;
// Accepted connections from one address that have sent nothing yet: a paired device opens a few
// streams in the same instant (clipboard, a drag, an update), so the cap is well above that.
constexpr int kMaxSilentStreamsPerAddress = 8;
constexpr int kMaxStreamsPerPeer = 32;
// Endpoints kept per paired device (Node::Endpoint), bounded per source: what beacons claim is
// forgotten when the beacons stop; the others are stored (the best kMaxStoredEndpoints).
constexpr int kBeaconEndpointExpiryMs = 30000;
constexpr int kMaxStoredEndpoints = 8;
constexpr int kMaxSelfAddresses = 8;  // addresses a hello reports (and that are taken from one)
constexpr int kMaxHintEndpoints = 6;  // endpoints a group introduction carries per member
constexpr int kInterfaceCheckMs = 5000; // this device's addresses are checked this often
constexpr int kManualQueryMs = 30000;   // a typed host that never answered is asked again
constexpr int kMaxManualHosts = 16;
// Connected through a router while one of the peer's own addresses is on a local subnet (two
// devices of one LAN that met through the router's public address): a handshake to that address
// is tried this often, a few times, and the session moves there when it answers.
constexpr int kProbeIntervalMs = 5000;
constexpr int kMaxProbeFails = 4;
// Streams (Node::openStream): one more TCP endpoint every kHappyEyeballsMs; the peer is asked to
// dial back after kDialAfterMs (at once when it is behind a NAT); a tunnel through the session
// after kTunnelAfterMs; given up after kStreamDeadlineMs.
constexpr int kHappyEyeballsMs = 250;
constexpr int kDialAfterMs = 300;
constexpr int kTunnelAfterMs = 5000;
constexpr int kDialWaitMs = 10000;
constexpr int kStreamDeadlineMs = 20000;
constexpr int kMaxStreamTargets = 8;
constexpr int kMaxDialsPerPeer = 8;
constexpr int kUnreachableReportMs = 60000;
// The hello's "st": this version dials back and tunnels streams (absent in earlier versions).
constexpr int kStreamPathsVersion = 1;
// First frame of a dial-back stream (the dialing side sends it as soon as the stream is up).
const QByteArray kDialMagic = QByteArrayLiteral("\xff" "DFDIAL1");
// Tunnel messages, on the tunnel channel: [op u8][sender opened it u8][tunnel id u32][payload].
constexpr quint8 kTunnelOpen = 1;
constexpr quint8 kTunnelData = 2;
constexpr quint8 kTunnelClose = 3;
constexpr int kTunnelChunk = 16 * 1024;
constexpr quint64 kOpenedHere = quint64(1) << 32; // tunnel key bit: this device opened it
// Members a group.members message may introduce: well above any real group, so that one message
// cannot enroll an unbounded number of devices to handshake with. The records one member may
// have introduced at a time, and the records introduced in all, are bounded too (a member that
// keeps listing fresh keys enrolls nothing past that), and a record a member introduced that
// never connected goes when that member is forgotten or demoted (Node::dropIntroducedBy).
constexpr int kMaxIntroductions = 64;
constexpr int kMaxIntroducedPerMember = 128;
constexpr int kMaxIntroduced = 256;
// A stored device that answers no initiation (off, or a stale address hint) is tried every 5 s
// for a while, then once a minute, so that it does not make this device a packet source for
// ever; a beacon from it or a network change makes the retries quick again. A device with an
// endpoint the user typed, an address it reported on one of this device's subnets, or an
// endpoint that worked within kSteadyEndpointMs stays at every 5 s (hasSteadyEndpoint): across
// routers no beacon ever comes to speed things up again. Addresses it reported on other
// networks (private ones, met on another LAN) do not count: from elsewhere they are strangers'.
constexpr int kSlowRetryAfter = 60;
constexpr int kSlowRetryMs = 60000;
constexpr qint64 kSteadyEndpointMs = 24LL * 3600 * 1000;

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

// An address one device can be reached at: no wildcard, broadcast or multicast address.
bool isUnicastAddress(const QHostAddress &addr)
{
    if (addr.isNull() || addr.isMulticast() || addr.isBroadcast())
        return false;
    if (addr == QHostAddress::Any || addr == QHostAddress::AnyIPv4 || addr == QHostAddress::AnyIPv6)
        return false;
    return addr.protocol() == QAbstractSocket::IPv4Protocol || addr.protocol() == QAbstractSocket::IPv6Protocol;
}

QString endpointText(const QHostAddress &addr, quint16 port) { return QStringLiteral("%1:%2").arg(addr.toString()).arg(port); }

// The order endpoints are preferred and kept in (EndpointSource), and how many of each are kept.
int sourceRank(EndpointSource source) { return int(source); }
int sourceCap(EndpointSource source)
{
    switch (source) {
    case EndpointSource::Configured:
        return 3;
    case EndpointSource::SelfReported:
        return kMaxSelfAddresses;
    case EndpointSource::Observed:
        return 2;
    case EndpointSource::GroupHint:
        return 4;
    case EndpointSource::Beacon:
        break;
    }
    return 4;
}

// Where the sockets listen: every interface, unless DESKFLOWER_BIND_ADDRESS says otherwise (the
// tests use 127.0.0.1, so that Windows does not ask for a firewall rule for every test binary).
QHostAddress defaultBindAddress()
{
    static const QHostAddress address = [] {
        QHostAddress a;
        const QByteArray v = qgetenv("DESKFLOWER_BIND_ADDRESS");
        return (!v.isEmpty() && a.setAddress(QString::fromLatin1(v))) ? a : QHostAddress(QHostAddress::AnyIPv4);
    }();
    return address;
}
} // namespace

// A stream's bytes through the UDP session (Node::openStream's last resort): carried by the
// peer's tunnel channel, a reliable channel of its own beside the control one, so that a bulk
// transfer neither waits behind nor delays input and clipboard messages. Inside the session, the
// stream still runs its own Noise handshake, so both ends authenticate it exactly as over TCP.
class TunnelTransport : public StreamTransport
{
public:
    TunnelTransport(Node *node, QByteArray peerId, quint64 key) : m_node(node), m_peer(std::move(peerId)), m_key(key) {}
    ~TunnelTransport() override { abort(); }

    void write(const QByteArray &bytes) override
    {
        if (m_closed)
            return;
        m_out += bytes;
        // Frames are written as several small pieces: gathered until the event loop runs.
        if (!m_flushQueued) {
            m_flushQueued = true;
            QMetaObject::invokeMethod(this, [this] { flush(); }, Qt::QueuedConnection);
        }
    }
    QByteArray readAll() override { return std::exchange(m_in, {}); }
    // Unbounded (the tunnel channel has no flow control per stream): a paused SecureStream fails
    // past SecureStream::kMaxPausedBacklog.
    qint64 bytesAvailable() const override { return m_in.size(); }
    qint64 bytesToWrite() const override { return m_out.size() + (m_node ? m_node->tunnelBacklog(m_peer) : 0); }
    void disconnectFromHost() override
    {
        if (m_closed)
            return;
        flush();
        close(true); // the close travels after the data on the same ordered channel
        QMetaObject::invokeMethod(this, [this] { emit disconnected(); }, Qt::QueuedConnection);
    }
    void abort() override
    {
        if (m_closed)
            return;
        m_out.clear();
        close(true);
    }
    bool isUnconnected() const override { return m_closed; }
    QString errorString() const override { return m_error; }

    // From Node.
    void deliver(const QByteArray &data)
    {
        if (m_closed)
            return;
        m_in += data;
        emit readyRead();
    }
    void remoteClosed()
    {
        if (m_closed)
            return;
        close(false);
        emit disconnected();
    }
    void lose(const QString &reason)
    {
        if (m_closed)
            return;
        m_closed = true;
        m_error = reason;
        emit errorOccurred();
    }
    void drained()
    {
        if (!m_closed)
            emit bytesWritten();
    }
    void detach() { m_node = nullptr; } // the node is going away

private:
    void flush()
    {
        m_flushQueued = false;
        if (m_closed || !m_node)
            return;
        for (qsizetype offset = 0; offset < m_out.size(); offset += kTunnelChunk)
            m_node->sendTunnel(m_peer, kTunnelData, m_key, m_out.mid(offset, kTunnelChunk));
        m_out.clear();
    }
    void close(bool tell)
    {
        m_closed = true;
        if (m_node) {
            if (tell)
                m_node->sendTunnel(m_peer, kTunnelClose, m_key, {});
            m_node->dropTunnel(m_peer, m_key);
        }
    }

    Node *m_node;
    QByteArray m_peer;
    quint64 m_key;
    QByteArray m_out;
    QByteArray m_in;
    QString m_error;
    bool m_closed = false;
    bool m_flushQueued = false;
};

// One way to reach a paired device. A device may be reachable at several (its own addresses on
// each network, the router that translates them, an address the user typed): all are kept,
// handshakes go to them in turn while it is not connected, streams race them, and packets go to
// the one the last authenticated packet came from (Peer::addr).
struct Node::Endpoint {
    QHostAddress addr;
    quint16 port = 0;    // UDP
    quint16 tcpPort = 0; // 0: the device's own (its hello)
    EndpointSource source = EndpointSource::Observed;
    qint64 lastOk = 0;  // wall clock of the last authenticated contact through it (persisted)
    qint64 seen = 0;    // when it was last claimed or used (monotonic)
    qint64 lastTry = 0; // last handshake sent to it
    int fails = 0;      // handshakes to it that went unanswered since it last worked
};

// The paths openStream races for one stream (or the TCP endpoints a dial-back tries).
struct Node::OutgoingStream {
    QByteArray peerId;
    std::vector<StreamTarget> targets;
    size_t next = 0;
    std::vector<QPointer<SecureStream>> attempts;
    int running = 0;
    qint64 started = 0;
    bool dialSent = false;
    bool tunnelStarted = false;
    QByteArray dialNonce; // asked of the peer: the dial-back that completes this stream
    QByteArray dialFrame; // dialing back for the peer: what the stream presents first
    QString lastError;
    std::vector<StreamTarget> unreachable; // the peer's own address it could not reach over TCP
    QTimer timer;
};

struct Node::Peer {
    Peer(QByteArray peerId, const ReliableChannel::Sender &sender)
        : id(std::move(peerId)),
          reliable(sender),
          tunnel(sender, quint8(wire::Inner::TunnelData), quint8(wire::Inner::TunnelAck))
    {
    }

    QByteArray id;
    QString name;
    QString os;
    QString version;
    QString platform;
    QString updateVersion;
    QString updatePlatform;
    QStringList capabilities;
    bool capabilitiesKnown = false;
    int protocol = 0; // PeerView::protocol
    QByteArray publicKey;
    // For a paired device: where it was last authenticated (a handshake or a decrypted packet),
    // paired, stored or hinted by a group member: where packets go. Beacons never set it (see
    // endpoints).
    QHostAddress addr;
    quint16 port = 0;
    bool exploring = false;
    bool connected = false;
    qint64 lastSeen = -1;
    qint64 addrSeen = 0;
    quint16 tcpPort = 0; // 0: same as the UDP port
    // Every endpoint known for a paired device (see Endpoint), kept after it connects. Beacons
    // are unauthenticated: one forged by a stranger must neither move `addr` (every handshake
    // would go to the forger) nor keep the real device's claim out, so what they claim is one
    // more endpoint, tried in turn with the others, and the endpoint a handshake completes at
    // becomes `addr`.
    std::vector<Endpoint> endpoints;
    int nextTarget = 0;  // round-robin position over addr + endpoints
    int targetCount = 1; // targets at the last initiation
    // From its authenticated hello: the addresses of its own interfaces (an address it is seen
    // at that is none of these is a router's: NAT), and whether it dials back and tunnels streams.
    QList<QHostAddress> selfAddrs;
    bool streamPaths = false;
    qint64 lastProbeAt = 0; // a handshake to a direct address while connected through a router
    qint64 unreachableReportedAt = -kUnreachableReportMs;
    int activeDials = 0; // dial-backs under way for it
    // The level the peer gives us, from its authenticated hello: unknown until the first one of
    // the pairing (treated as Device by the group logic), reset when the pairing ends.
    std::optional<TrustLevel> remoteTrust;

    std::unique_ptr<SecureSession> current;
    std::unique_ptr<SecureSession> previous;
    std::unique_ptr<SecureSession> next; // responder side, waiting for the first data packet
    qint64 previousSince = 0;

    std::unique_ptr<noise::IKInitiator> handshake;
    quint32 handshakeIndex = 0;
    qint64 handshakeSentAt = 0;
    int handshakeAttempts = 0;
    qint64 lastQueryAt = 0;     // last unicast beacon query sent to it (sendBeacons)
    QHostAddress handshakeAddr; // where the pending initiation went
    quint16 handshakePort = 0;
    // Replay guard for the peer's initiations: its boot id is the epoch (a restart opens a new
    // one, whatever its clock does) and within an epoch the counter must grow. The last
    // kMaxHandshakeEpochs epochs are kept, so a recording from before the peer's last restarts is
    // refused as well; nobody can add epochs but the peer itself (the payload is encrypted to our
    // static key with the peer's), so the history only moves on when it really restarts.
    struct Epoch {
        QByteArray bootId;
        quint64 counter = 0;
        qint64 seen = 0;
    };
    std::vector<Epoch> handshakeEpochs;

    ReliableChannel reliable;
    // Streams through the session (TunnelTransport), by key: tunnel id, plus kOpenedHere when this
    // device opened it.
    ReliableChannel tunnel;
    std::map<quint64, QPointer<TunnelTransport>> tunnels;
    quint32 nextTunnel = 1;
    qint64 backlogSince = -1;   // oldest moment with unacknowledged reliable data (diagnostics)
    qint64 backlogLoggedAt = 0;
    QByteArray remoteBoot;
    QByteArray nextBoot; // announced by the initiation that created `next`; adopted when confirmed
};

bool Node::RateLimit::take(qint64 nowMs)
{
    tokens = std::min(burst, tokens + double(nowMs - last) * perSecond / 1000.0);
    last = nowMs;
    if (tokens < 1)
        return false;
    tokens -= 1;
    return true;
}

// A device sends a beacon per interface every second while exploring; a handshake per peer every
// couple of minutes (or every second while reconnecting); pairing, a few packets per attempt.
Node::Node(Options options, QObject *parent)
    : QObject(parent),
      m_options(std::move(options)),
      m_beaconLimit(100, 200),
      m_handshakeLimit(20, 40),
      m_responseLimit(20, 40),
      m_pairingLimit(20, 40)
{
}

Node::~Node()
{
    // Streams may outlive the node (their owners delete them): their tunnels stop calling it.
    for (auto &[id, peer] : m_peers) {
        for (auto &[key, tunnel] : peer->tunnels) {
            if (tunnel)
                tunnel->detach();
        }
    }
    m_outgoing.clear();
}

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
    // Started from the clock so that responders of earlier versions, which compare it with the
    // timestamps they used to get, keep accepting us after a restart.
    m_handshakeCounter = quint64(QDateTime::currentMSecsSinceEpoch());

    for (const TrustedPeer &t : m_store.all()) {
        Peer &p = peerFor(t.id);
        p.publicKey = t.publicKey;
        p.name = t.name;
        // Where it was last connected: tried straight away (discovery may not reach it), and
        // replaced by whatever discovery finds (addrSeen 0: stale at once). The other stored
        // endpoints are tried in turn.
        const QHostAddress last(t.lastAddress);
        if (!last.isNull() && t.lastPort) {
            p.addr = last;
            p.port = t.lastPort;
        }
        loadEndpoints(p, t);
    }
    // From here on, m_options.bindAddress is the address actually bound to: the hello reports it
    // alone when it is not a wildcard.
    if (m_options.bindAddress.isNull())
        m_options.bindAddress = defaultBindAddress();
    refreshLocalAddresses(true);

    const QHostAddress bindAddress = m_options.bindAddress;
    if (!m_socket.bind(bindAddress, m_options.dataPort) && !m_socket.bind(bindAddress, 0)) {
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
    if (m_tcp.listen(bindAddress, m_socket.localPort()) || m_tcp.listen(bindAddress, 0)) {
        connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_tcp.nextPendingConnection()) {
                // Connections that have not authenticated yet are few per address and short-lived
                // (SecureStream's handshake timeouts, 2 s for one that sends nothing), so that
                // strangers cannot hold the slots: at the per-address cap of connections that
                // have sent nothing, the oldest such one from that address makes room (a paired
                // device's own streams, or several devices behind one address, speak at once and
                // are not refused for a stranger's sake: the cap is well above what they open
                // together); at the global cap the oldest pending one does.
                const QHostAddress address = normalized(socket->peerAddress());
                const auto silentFrom = [&address](const PendingStream &p) {
                    return p.address == address && p.stream->isSilent();
                };
                if (std::count_if(m_pendingStreams.cbegin(), m_pendingStreams.cend(), silentFrom)
                    >= kMaxSilentStreamsPerAddress) {
                    SecureStream *oldest =
                        std::find_if(m_pendingStreams.begin(), m_pendingStreams.end(), silentFrom)->stream;
                    oldest->abort(QStringLiteral("Evicted")); // settles it below
                }
                if (int(m_pendingStreams.size()) >= kMaxPendingStreams)
                    m_pendingStreams.front().stream->abort(QStringLiteral("Evicted")); // settles it below
                auto trust = [this](const QByteArray &remoteStatic) -> std::optional<QByteArray> {
                    const QByteArray id = crypto::deviceIdFromPublicKey(remoteStatic);
                    const TrustedPeer *peer = m_store.find(id);
                    if (!peer || peer->publicKey != remoteStatic || !isUsable(id))
                        return std::nullopt;
                    if (m_acceptedStreams[id] >= kMaxStreamsPerPeer)
                        return std::nullopt;
                    return id;
                };
                watchAccepted(SecureStream::accept(socket, m_identity.keys(), trust, this), address, true);
            }
        });
    } else {
        qCWarning(lcNode) << "Cannot listen on TCP:" << m_tcp.errorString() << "(file transfers disabled)";
    }

    if (m_options.enableDiscovery) {
        if (m_discovery.bind(bindAddress, m_options.discoveryPort,
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
            writeDatagram(packet, addr, port);
        });
    m_pairing->setPinTimeout(m_options.pinTimeoutMs);
    connect(m_pairing.get(), &PairingManager::pinGenerated, this,
            [this](const QByteArray &id, const QString &name, const QString &pin, std::optional<TrustLevel> suggested) {
                // What the PIN dialog offers is what the pairing applies unless the user picks
                // another level: the requester's suggestion, but a guest stays a guest, a device
                // paired again never gets less than it had, and Group is never taken up without
                // the user picking it (setPairTrust).
                if (suggested && !m_pairTrust.contains(id)) {
                    const TrustedPeer *existing = isUsable(id) ? m_store.find(id) : nullptr;
                    TrustLevel level = std::min(*suggested, TrustLevel::Device);
                    if (existing)
                        level = existing->isGuest() ? TrustLevel::Guest : std::max(existing->trust, level);
                    m_pairTrust.insert(id, level);
                    m_pairSuggested.insert(id, *suggested);
                }
                emit pairingPinGenerated(id, name, pin);
            });
    connect(m_pairing.get(), &PairingManager::pinRequired, this, &Node::pairingPinRequired);
    connect(m_pairing.get(), &PairingManager::succeeded, this, &Node::onPairingSucceeded);
    connect(m_pairing.get(), &PairingManager::failed, this,
            [this](const QByteArray &id, const QString &reason) {
                // Unless another pairing with that device is under way (a repeated request
                // cancels the first one, then sets its level).
                if (!m_pairing->isPairingWith(id)) {
                    m_pairTrust.remove(id);
                    m_pairSuggested.remove(id);
                }
                emit pairingFinished(id, false, reason);
            });

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
            sendMessage(id, helloMessage(id));
    }
}

void Node::setCapability(const QString &name, bool on)
{
    if (m_capabilities.contains(name) == on)
        return;
    if (on)
        m_capabilities.append(name);
    else
        m_capabilities.removeAll(name);
    for (auto &[id, peer] : m_peers) {
        if (peer->connected)
            sendMessage(id, helloMessage(id));
    }
}

bool Node::setName(const QString &name)
{
    if (!m_identity.setName(name))
        return false;
    for (auto &[id, peer] : m_peers) {
        if (peer->connected)
            sendMessage(id, helloMessage(id));
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
        v.viaNat = trusted && isViaNat(*p);
        v.trusted = trusted;
        v.allowView = isViewGranted(id);
        v.trust = trustLevel(id);
        v.effectiveTrust = p->remoteTrust ? std::min(v.trust, *p->remoteTrust) : v.trust;
        v.declaredTrust = trusted ? p->remoteTrust : std::nullopt;
        v.otherUser = trusted && !isUsable(id);
        v.connected = p->connected;
        v.exploring = p->exploring && p->lastSeen >= 0 && now - p->lastSeen < kUntrustedExpiryMs;
        if (trusted) {
            v.version = p->version;
            v.platform = p->platform;
            v.updateVersion = p->updateVersion;
            v.updatePlatform = p->updatePlatform;
            v.capabilities = p->capabilities;
            v.capabilitiesKnown = p->capabilitiesKnown;
            v.protocol = p->protocol;
        }
        out.append(v);
    }
    return out;
}

int Node::peerProtocol(const QByteArray &peerId) const
{
    if (peerId == id())
        return wire::kProtocolVersion;
    auto it = m_peers.find(peerId);
    return it != m_peers.end() ? it->second->protocol : 0;
}

bool Node::transfersCompatible(const QByteArray &peerId) const
{
    const int protocol = peerProtocol(peerId);
    return protocol == 0 || protocol == wire::kProtocolVersion;
}

bool Node::isConnected(const QByteArray &peerId) const
{
    auto it = m_peers.find(peerId);
    return it != m_peers.end() && it->second->connected;
}

bool Node::isViewGranted(const QByteArray &peerId) const
{
    const TrustedPeer *trusted = m_store.find(peerId);
    return trusted && trusted->allowView && !trusted->isGuest();
}

TrustLevel Node::trustLevel(const QByteArray &peerId) const
{
    const TrustedPeer *trusted = m_store.find(peerId);
    return trusted ? trusted->trust : TrustLevel::Device;
}

TrustLevel Node::effectiveTrust(const QByteArray &peerId) const
{
    auto it = m_peers.find(peerId);
    const TrustLevel remote = it == m_peers.end() ? TrustLevel::Device
                                                  : it->second->remoteTrust.value_or(TrustLevel::Device);
    return std::min(trustLevel(peerId), remote);
}

std::optional<TrustLevel> Node::declaredTrust(const QByteArray &peerId) const
{
    auto it = m_peers.find(peerId);
    return it == m_peers.end() ? std::nullopt : it->second->remoteTrust;
}

bool Node::isGuest(const QByteArray &peerId) const
{
    const TrustedPeer *trusted = m_store.find(peerId);
    return trusted && trusted->isGuest();
}

void Node::setTrust(const QByteArray &peerId, TrustLevel level)
{
    if (!m_store.contains(peerId) || !isUsable(peerId) || trustLevel(peerId) == level)
        return;
    qCInfo(lcNode) << "Paired device" << peerId.toHex() << "is now" << trustName(level);
    const bool demoted = trustLevel(peerId) == TrustLevel::Group && level != TrustLevel::Group;
    m_store.setTrust(peerId, level);
    if (demoted)
        dropIntroducedBy(peerId); // its word no longer counts for devices never heard from
    emit peersChanged();
    // The peer learns the new level at once, and a new group member meets the others.
    auto it = m_peers.find(peerId);
    if (it != m_peers.end() && it->second->connected) {
        sendMessage(peerId, helloMessage(peerId));
        if (effectiveTrust(peerId) == TrustLevel::Group)
            sendGroupState(*it->second);
    }
}

bool Node::setPairTrust(const QByteArray &peerId, TrustLevel level)
{
    if (!m_pairing || !m_pairing->isPairingWith(peerId))
        return false;
    m_pairTrust.insert(peerId, level);
    return true;
}

std::optional<TrustLevel> Node::pairingSuggestion(const QByteArray &peerId) const
{
    auto it = m_pairSuggested.constFind(peerId);
    if (it == m_pairSuggested.constEnd() || !m_pairing || !m_pairing->isPairingWith(peerId)
        || *it <= pairingTrust(peerId))
        return std::nullopt;
    return *it;
}

TrustLevel Node::pairingTrust(const QByteArray &peerId) const
{
    // Not chosen: a device paired again keeps the level it had (never a silent upgrade), a new
    // one is Device. Another user's record is not this user's choice.
    auto it = m_pairTrust.constFind(peerId);
    if (it != m_pairTrust.constEnd())
        return *it;
    const TrustedPeer *existing = m_store.find(peerId);
    return existing && isUsable(peerId) ? existing->trust : TrustLevel::Device;
}

bool Node::isViewAllowed(const QByteArray &peerId) const { return m_remoteViewEnabled && isViewGranted(peerId); }

void Node::setViewAllowed(const QByteArray &peerId, bool on)
{
    if (!m_store.contains(peerId) || !isUsable(peerId) || isViewGranted(peerId) == on || (on && isGuest(peerId)))
        return;
    m_store.setAllowView(peerId, on);
    emit peersChanged();
}

void Node::setRemoteViewEnabled(bool on)
{
    if (m_remoteViewEnabled == on)
        return;
    m_remoteViewEnabled = on;
    emit peersChanged(); // what isViewAllowed answers changed
}

void Node::addManualPeer(const QHostAddress &addr, quint16 port)
{
    const QHostAddress host = normalized(addr);
    if (!isUnicastAddress(host))
        return;
    port = port ? port : wire::kDefaultDataPort;
    auto it = std::find_if(m_manualHosts.begin(), m_manualHosts.end(),
                           [&](const ManualHost &h) { return h.addr == host && h.port == port; });
    if (it == m_manualHosts.end()) {
        if (int(m_manualHosts.size()) >= kMaxManualHosts)
            m_manualHosts.erase(m_manualHosts.begin()); // the oldest typed
        m_manualHosts.push_back({host, port, 0, {}});
        it = m_manualHosts.end() - 1;
    }
    it->lastQuery = nowMs();
    writeDatagram(beaconPacket(true), host, port);
    // A paired device already known there gets its endpoint now, and its retries speed up.
    for (auto &[id, peer] : m_peers) {
        if (peer->addr == host && m_store.contains(id))
            matchManualHost(id, peer->addr, peer->port);
    }
}

void Node::matchManualHost(const QByteArray &id, const QHostAddress &from, quint16 port)
{
    // Matched on the address only: the user may have typed no port (the default) for a device on
    // another one. Called once the device is authenticated there, so that a beacon claiming the
    // address cannot bind the typed host to another device.
    auto peerIt = m_peers.find(id);
    if (peerIt == m_peers.end() || !m_store.contains(id))
        return;
    for (ManualHost &h : m_manualHosts) {
        if (h.addr != from)
            continue;
        h.peerId = id;
        Endpoint *e = findEndpoint(*peerIt->second, from, port);
        if (!e || e->source != EndpointSource::Configured) {
            addEndpoint(*peerIt->second, from, port, 0, EndpointSource::Configured);
            qCInfo(lcNode) << "Keeping" << endpointText(from, port) << "(typed by the user) for" << peerIt->second->name;
            saveEndpoints(*peerIt->second);
        }
    }
}

bool Node::requestPairing(const QByteArray &peerId, TrustLevel level)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || m_store.contains(peerId) || it->second->addr.isNull())
        return false;
    // Started first: it fails a pairing still under way, which drops the level chosen for it.
    if (!m_pairing->start(peerId, it->second->addr, it->second->port, level))
        return false;
    m_pairTrust.insert(peerId, level);
    return true;
}

bool Node::submitPin(const QByteArray &peerId, const QString &pin) { return m_pairing->submitPin(peerId, pin); }

void Node::cancelPairing(const QByteArray &peerId) { m_pairing->cancel(peerId); }

void Node::unpair(const QByteArray &peerId)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || !m_store.contains(peerId) || !isUsable(peerId))
        return; // another user's pairing is theirs to remove
    Peer &p = *it->second;
    if (p.connected) {
        QCborMap m;
        m.insert(kT, QStringLiteral("unpair"));
        p.reliable.send(QCborValue(m).toCbor(), nowMs()); // best effort, sent immediately
    }
    m_store.forget(peerId, p.name); // group members must not bring it back
    m_store.remove(peerId);
    m_levelCheck.remove(peerId);
    dropSessions(p);
    p.reliable.reset();
    p.tunnel.reset();
    p.remoteBoot.clear();
    p.remoteTrust.reset(); // what it declared went with the pairing
    p.endpoints.clear();
    p.selfAddrs.clear();
    dropIntroducedBy(peerId);
    emit peersChanged();
}

void Node::unforget(const QByteArray &peerId)
{
    if (!m_store.isForgotten(peerId))
        return;
    qCInfo(lcNode) << "Forgotten device" << peerId.toHex() << "allowed again";
    m_store.unforget(peerId);
    m_skippedNoticed.remove(peerId);
    emit peersChanged();
    // A member answers every hello with its member list: one that knows the device introduces
    // it now rather than at the next reconnection.
    for (const auto &[id, p] : m_peers) {
        if (p->connected && trustLevel(id) == TrustLevel::Group && effectiveTrust(id) == TrustLevel::Group)
            sendMessage(id, helloMessage(id));
    }
}

void Node::dropIntroducedBy(const QByteArray &introducer)
{
    // What a member introduced and this device never talked to goes with the member: it was only
    // ever that member's word. A member that connected once stands on its own record.
    for (const QByteArray &id : m_store.introducedBy(introducer, true)) {
        qCInfo(lcNode) << "Dropping" << id.toHex() << "introduced by" << introducer.toHex() << "(never connected)";
        m_store.remove(id);
        auto it = m_peers.find(id);
        if (it != m_peers.end()) {
            dropSessions(*it->second);
            it->second->reliable.reset();
            it->second->tunnel.reset();
            it->second->addr.clear();
            it->second->endpoints.clear();
        }
    }
}

bool Node::sendMessage(const QByteArray &peerId, const QCborMap &message)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || !m_store.contains(peerId) || !isUsable(peerId))
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
    if (it == m_peers.end() || !m_store.contains(peerId) || !isUsable(peerId))
        return nullptr;
    Peer &p = *it->second;
    if (!(p.connected && p.streamPaths) && streamTargets(p).empty())
        return nullptr;
    return startOutgoing(p, {});
}

void Node::injectDatagram(const QByteArray &datagram, const QHostAddress &from, quint16 port)
{
    handlePacket(datagram, normalized(from), port, true);
}

void Node::writeDatagram(const QByteArray &datagram, const QHostAddress &addr, quint16 port)
{
    if (m_options.datagramHook && m_options.datagramHook(datagram, addr, port))
        return;
    if (m_socket.writeDatagram(datagram, addr, port) < 0)
        ++m_failedSends; // a full socket buffer: the reliable channel resends
}

QCborMap Node::helloMessage(const QByteArray &peerId) const
{
    QCborMap m;
    m.insert(kT, QStringLiteral("hello"));
    m.insert(kN, m_identity.name());
    m.insert(kO, QSysInfo::productType());
    m.insert(QStringLiteral("tp"), m_tcp.serverPort());
    m.insert(QStringLiteral("v"), build::version());
    m.insert(QStringLiteral("pl"), build::platform());
    m.insert(QStringLiteral("tl"), trustName(trustLevel(peerId))); // the level we give the peer
    if (!m_updateVersion.isEmpty()) {
        m.insert(QStringLiteral("uv"), m_updateVersion);
        m.insert(QStringLiteral("upl"), m_updatePlatform);
    }
    m.insert(QStringLiteral("cap"), QCborArray::fromStringList(m_capabilities)); // always: empty says "none"
    // The protocol of the features that change between versions (wire::kProtocolVersion); older
    // versions ignore keys they do not know.
    m.insert(QStringLiteral("pv"), wire::kProtocolVersion);
    // This device's own addresses and ports, so that a peer that sees it through a router (NAT)
    // knows, and can still reach it directly when they share a network; and the stream paths it
    // knows besides TCP (dial-back and tunnels).
    QCborArray addresses;
    for (const QHostAddress &a : m_localAddresses)
        addresses.append(a.toString());
    m.insert(QStringLiteral("la"), addresses);
    m.insert(QStringLiteral("up"), m_socket.localPort());
    m.insert(QStringLiteral("st"), kStreamPathsVersion);
    if (m_helloOverride)
        m_helloOverride(m);
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
        if (!m_beaconLimit.take(nowMs()))
            break;
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
        if (viaDataSocket && m_pairingLimit.take(nowMs())) {
            if (auto msg = wire::parseCborPacket(packet))
                m_pairing->handlePacket(*type, *msg, from, port);
        }
        break;
    case wire::Type::HandshakeInit:
        // Costs two key exchanges before the sender is known.
        if (viaDataSocket && m_handshakeLimit.take(nowMs()))
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

    const qint64 now = nowMs();
    if (viaDataSocket && msg.value(kQ).toBool()) {
        // Answered at most once per interval and sender: a forged source address must not turn
        // us into a traffic reflector.
        if (m_queryAnswered.size() > 256) {
            for (auto it = m_queryAnswered.begin(); it != m_queryAnswered.end();)
                it = now - it.value() > kQueryAnswerMs ? m_queryAnswered.erase(it) : std::next(it);
        }
        const auto sender = std::make_pair(from, port);
        auto it = m_queryAnswered.find(sender);
        if (it == m_queryAnswered.end() || now - it.value() > kQueryAnswerMs) {
            m_queryAnswered.insert(sender, now);
            writeDatagram(beaconPacket(false), from, port);
        }
    }

    const bool trusted = m_store.contains(id);
    if (!trusted && !m_peers.count(id)) {
        // Unknown devices cost memory and screen space until they expire: keep a bounded number.
        int untrusted = 0;
        for (const auto &[peerId, peer] : m_peers)
            untrusted += m_store.contains(peerId) ? 0 : 1;
        if (untrusted >= kMaxUntrustedPeers)
            return;
    }
    Peer &p = peerFor(id);
    const bool wasVisible = p.lastSeen >= 0 && nowMs() - p.lastSeen < kUntrustedExpiryMs;
    const QString name = msg.value(kN).toString().left(64);
    const QString os = msg.value(kO).toString().left(32);
    const bool exploring = msg.value(kX).toBool();
    // The beacon's source port is the sender's data socket.
    const quint16 dataPort = port;

    if (!wasVisible)
        qCInfo(lcNode) << "Discovered" << name << from.toString() << port << (trusted ? "(trusted)" : "");
    // Beacons are unauthenticated: a paired device keeps the name from its authenticated hello
    // (anyone can send a beacon with its id and another name).
    bool changed = !wasVisible || p.os != os || p.exploring != exploring || (!trusted && p.name != name);
    p.lastSeen = now;
    if (!trusted) {
        p.name = name;
        p.publicKey = pub;
    }
    p.os = os;
    p.exploring = exploring;
    const quint16 tcpPort = quint16(msg.value(QStringLiteral("tp")).toInteger());

    if (trusted) {
        // Beacons are unauthenticated: they never move a paired device's address (every
        // handshake would follow a forged one to the forger, and the real device's own
        // initiations would be refused as simultaneous) and never touch a live session. What
        // they claim for a device that is not connected is one more endpoint to try (endpoints);
        // the authenticated hello is the source of truth for the TCP port once connected.
        if (!p.connected) {
            if (p.addr == from && p.port == dataPort)
                p.tcpPort = tcpPort;
            if (Endpoint *e = addEndpoint(p, from, dataPort, tcpPort, EndpointSource::Beacon))
                e->fails = 0;
            // It is there: back to the usual retries if they had slowed down (kSlowRetryAfter).
            p.handshakeAttempts = std::min(p.handshakeAttempts, 5 * std::max(1, p.targetCount));
        } else if (isSelfAddress(p, from) && (p.addr != from || p.port != dataPort)) {
            // Connected through a router while it is right here, at one of its own addresses:
            // a direct handshake is worth trying again (probeDirectPath).
            if (Endpoint *e = findEndpoint(p, from, dataPort))
                e->fails = 0;
            p.lastProbeAt = 0;
        }
    } else {
        p.tcpPort = tcpPort;
        // A host with several interfaces sends a beacon from each one: stick to the current
        // address while it keeps answering.
        if (p.addr == from && p.port == dataPort) {
            p.addrSeen = now;
        } else if (p.addr.isNull() || now - p.addrSeen > kAddressStaleMs) {
            p.addr = from;
            p.port = dataPort;
            p.addrSeen = now;
            changed = true;
        }
    }
    if (changed)
        emit peersChanged();
}

// --- Endpoints ---------------------------------------------------------------------------------

Node::Endpoint *Node::findEndpoint(Peer &peer, const QHostAddress &addr, quint16 port)
{
    for (Endpoint &e : peer.endpoints) {
        if (e.addr == addr && e.port == port)
            return &e;
    }
    return nullptr;
}

Node::Endpoint *Node::addEndpoint(Peer &peer, const QHostAddress &addr, quint16 port, quint16 tcpPort,
                                  EndpointSource source)
{
    if (!isUnicastAddress(addr) || !port)
        return nullptr;
    const qint64 now = nowMs();
    if (Endpoint *e = findEndpoint(peer, addr, port)) {
        // A better source takes over (a beacon's claim that a hello confirms, the router's
        // mapping that turns out to be an address the user typed); never the other way round,
        // so that an observed NAT endpoint never replaces what the user or the device said.
        if (sourceRank(source) < sourceRank(e->source))
            e->source = source;
        // What a beacon says about the TCP port counts only for what beacons claimed.
        if (tcpPort && (source != EndpointSource::Beacon || e->source == EndpointSource::Beacon))
            e->tcpPort = tcpPort;
        e->seen = now;
        return e;
    }
    // Bounded per source: a flood of claims of one kind displaces only that kind, the least
    // recently useful first, so the endpoints that worked and those the user typed stay.
    const auto sameSource = [source](const Endpoint &e) { return e.source == source; };
    if (std::count_if(peer.endpoints.cbegin(), peer.endpoints.cend(), sameSource) >= sourceCap(source)) {
        auto victim = peer.endpoints.end();
        for (auto it = peer.endpoints.begin(); it != peer.endpoints.end(); ++it) {
            if (it->source != source)
                continue;
            if (victim == peer.endpoints.end() || std::pair(it->lastOk, it->seen) < std::pair(victim->lastOk, victim->seen))
                victim = it;
        }
        peer.endpoints.erase(victim);
    }
    Endpoint e;
    e.addr = addr;
    e.port = port;
    e.tcpPort = tcpPort;
    e.source = source;
    e.seen = now;
    peer.endpoints.push_back(e);
    return &peer.endpoints.back();
}

void Node::saveEndpoints(const Peer &peer)
{
    if (!m_store.contains(peer.id))
        return;
    std::vector<const Endpoint *> kept;
    for (const Endpoint &e : peer.endpoints) {
        if (e.source != EndpointSource::Beacon)
            kept.push_back(&e);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Endpoint *a, const Endpoint *b) {
        return std::pair(sourceRank(a->source), -a->lastOk) < std::pair(sourceRank(b->source), -b->lastOk);
    });
    QList<StoredEndpoint> stored;
    for (const Endpoint *e : kept) {
        if (stored.size() >= kMaxStoredEndpoints)
            break;
        stored.append({e->addr.toString(), e->port, e->tcpPort, e->source, e->lastOk});
    }
    m_store.setEndpoints(peer.id, stored);
}

void Node::loadEndpoints(Peer &peer, const TrustedPeer &stored)
{
    peer.endpoints.clear();
    for (const StoredEndpoint &s : stored.endpoints) {
        if (Endpoint *e = addEndpoint(peer, QHostAddress(s.address), s.port, s.tcpPort, s.source))
            e->lastOk = std::max(e->lastOk, s.lastOk);
    }
    // Written by a version that kept one address, or the last one connected to.
    if (!stored.lastAddress.isEmpty() && stored.lastPort)
        addEndpoint(peer, QHostAddress(stored.lastAddress), stored.lastPort, 0, EndpointSource::Observed);
}

bool Node::hasSteadyEndpoint(const Peer &peer) const
{
    const qint64 wallNow = QDateTime::currentMSecsSinceEpoch();
    return std::any_of(peer.endpoints.cbegin(), peer.endpoints.cend(), [this, wallNow](const Endpoint &e) {
        return e.source == EndpointSource::Configured
               || (e.source == EndpointSource::SelfReported && isOnLocalSubnet(e.addr))
               || (e.lastOk > 0 && wallNow - e.lastOk < kSteadyEndpointMs);
    });
}

bool Node::isSelfAddress(const Peer &peer, const QHostAddress &addr) const { return peer.selfAddrs.contains(addr); }

bool Node::isViaNat(const Peer &peer) const
{
    if (!peer.connected || peer.addr.isNull() || peer.selfAddrs.isEmpty() || isSelfAddress(peer, peer.addr))
        return false;
    // Hellos leave loopback out: a peer seen at a loopback address runs on this same device
    // (unless it reports loopback addresses itself, as nodes bound to one do in tests).
    if (peer.addr.isLoopback())
        return std::any_of(peer.selfAddrs.cbegin(), peer.selfAddrs.cend(),
                           [](const QHostAddress &a) { return a.isLoopback(); });
    return true;
}

bool Node::isThisDevice(const QHostAddress &addr, quint16 port, bool tcp) const
{
    // Two devices often share a virtual network (a VM host's, a container bridge), so a peer may
    // report an address this device has too: at this device's own port, that is this device.
    // At another port it may be a second instance on this computer, which is reachable there.
    return (addr.isLoopback() || m_localAddresses.contains(addr))
           && port == (tcp ? m_tcp.serverPort() : m_socket.localPort());
}

bool Node::isOnLocalSubnet(const QHostAddress &addr) const
{
    for (const auto &[ip, prefix] : m_localSubnets) {
        if (addr.isInSubnet(ip, prefix))
            return true;
    }
    return false;
}

void Node::refreshLocalAddresses(bool initial)
{
    m_lastInterfaceCheck = nowMs();
    // Interfaces that are running first (a disconnected adapter's address reaches nobody), in the
    // system's order otherwise.
    QList<QHostAddress> running, idle;
    QList<std::pair<QHostAddress, int>> subnets;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp))
            continue;
        QList<QHostAddress> &list = iface.flags().testFlag(QNetworkInterface::IsRunning) ? running : idle;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = normalized(entry.ip());
            if (ip.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (entry.prefixLength() >= 0)
                subnets.append({ip, entry.prefixLength()});
            if (!ip.isLoopback() && isUnicastAddress(ip) && !running.contains(ip) && !idle.contains(ip))
                list.append(ip);
        }
    }
    QList<QHostAddress> addresses = running + idle;
    const QHostAddress bound = normalized(m_options.bindAddress);
    if (!bound.isNull() && bound != QHostAddress(QHostAddress::AnyIPv4) && bound != QHostAddress(QHostAddress::Any))
        addresses = {bound};
    if (addresses.size() > kMaxSelfAddresses)
        addresses.resize(kMaxSelfAddresses);
    m_localSubnets = subnets;
    const auto sorted = [](QList<QHostAddress> list) {
        std::sort(list.begin(), list.end(),
                  [](const QHostAddress &a, const QHostAddress &b) { return a.toIPv4Address() < b.toIPv4Address(); });
        return list;
    };
    if (sorted(addresses) == sorted(m_localAddresses))
        return;
    m_localAddresses = addresses;
    if (!initial)
        onNetworkChanged();
}

void Node::onNetworkChanged()
{
    QStringList list;
    for (const QHostAddress &a : std::as_const(m_localAddresses))
        list.append(a.toString());
    qCInfo(lcNode).noquote() << "Network changed; this device's addresses:" << list.join(QStringLiteral(", "));
    // Everything that had stopped answering may answer now: quick retries again, and paired
    // devices learn the new addresses.
    for (auto &[id, peer] : m_peers) {
        if (!m_store.contains(id))
            continue;
        peer->handshakeAttempts = 0;
        peer->lastQueryAt = 0;
        peer->lastProbeAt = 0;
        for (Endpoint &e : peer->endpoints)
            e.fails = 0;
        if (peer->connected)
            sendMessage(id, helloMessage(id));
    }
    for (ManualHost &h : m_manualHosts)
        h.lastQuery = 0;
    m_lastBeacon = -1000000;
}

void Node::probeDirectPath(Peer &peer, qint64 now)
{
    // Connected through a router (NAT) to a device that has an address on a local subnet: two
    // devices of one network that met through the router's public address (a group hint, an
    // address typed by hand, hairpin NAT). Streams to the router's address go nowhere, so a
    // handshake to the direct address is tried now and then; the session moves there when it
    // answers (handleHandshakeResp), and the other side follows the packets (followPeer).
    if (!isViaNat(peer) || now - peer.lastProbeAt < kProbeIntervalMs)
        return;
    if (peer.handshake) {
        if (now - peer.handshakeSentAt < kProbeIntervalMs)
            return; // an answer may still come
        abandonHandshake(peer); // the last probe went unanswered: one fail on its endpoint
    }
    for (Endpoint &e : peer.endpoints) {
        if (e.source != EndpointSource::SelfReported || e.fails >= kMaxProbeFails || !isOnLocalSubnet(e.addr)
            || isThisDevice(e.addr, e.port, false))
            continue;
        peer.lastProbeAt = now;
        qCInfo(lcNode) << "Trying a direct path to" << peer.name << "at" << endpointText(e.addr, e.port)
                       << "(connected through a router at" << endpointText(peer.addr, peer.port) << ")";
        sendHandshakeTo(peer, e.addr, e.port);
        return;
    }
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
    if (!trusted || trusted->publicKey != *remoteStatic || !isUsable(id))
        return;

    const QCborMap info = QCborValue::fromCbor(payload).toMap();
    const quint64 counter = quint64(info.value(kT).toInteger());
    const QByteArray remoteBoot = info.value(kB).toByteArray();
    Peer &p = peerFor(id);
    const qint64 now = nowMs();
    // Replayed (or reordered) initiation: an epoch already seen, with a counter not above the
    // last one taken in it. One from an unknown epoch may be a replay too (a recording from
    // before the last kMaxHandshakeEpochs restarts of the peer): it costs a key exchange and an
    // unconfirmed session, nothing else, so it is answered.
    auto epoch = std::find_if(p.handshakeEpochs.begin(), p.handshakeEpochs.end(),
                              [&remoteBoot](const Peer::Epoch &e) { return e.bootId == remoteBoot; });
    if (epoch != p.handshakeEpochs.end() && counter <= epoch->counter)
        return;
    // Simultaneous initiation: ours wins when our id is the smaller one and ours went to this
    // same endpoint a moment ago (the peer answers it). One that went elsewhere or has gone
    // unanswered for longer may never be answered: the peer's authenticated one is taken as well.
    if (p.handshake && m_identity.id() < id && p.handshakeAddr == from && p.handshakePort == port
        && now - p.handshakeSentAt < kHandshakeTieMs)
        return;

    if (epoch != p.handshakeEpochs.end()) {
        epoch->counter = counter;
        epoch->seen = now;
    } else {
        if (int(p.handshakeEpochs.size()) >= kMaxHandshakeEpochs) {
            // The confirmed epoch (the peer's current boot) is never pushed out by recordings of
            // unknown ones; of the others the earliest taken goes (a replay does not make an old
            // epoch recent).
            const auto victim = std::find_if(p.handshakeEpochs.begin(), p.handshakeEpochs.end(),
                                             [&p](const Peer::Epoch &e) { return e.bootId != p.remoteBoot; });
            if (victim != p.handshakeEpochs.end())
                p.handshakeEpochs.erase(victim);
        }
        p.handshakeEpochs.push_back({remoteBoot, counter, now});
    }
    // Our own pending initiation stays: this one proves nothing until its first packet decrypts
    // (handleData drops ours then, the peer having shown it holds a session with us), and until
    // then a recording must not be able to cancel ours. Both may complete: current and previous
    // sessions handle that as they do a rekey.

    QCborMap reply;
    reply.insert(kB, m_bootId);
    QByteArray message2;
    const auto keys = responder.writeMessage2(QCborValue(reply).toCbor(), &message2);
    if (!keys)
        return;

    // Nothing else changes until the session is confirmed by its first decrypted packet
    // (handleData): the address, the boot id and the reliable channel follow the peer, not a
    // packet anyone may have recorded.
    releaseIndex(p.next);
    const quint32 localIndex = allocateIndex(id);
    p.next = std::make_unique<SecureSession>(localIndex, senderIndex, *keys, false, nowMs());
    p.nextBoot = remoteBoot;

    QByteArray out = wire::header(wire::Type::HandshakeResp);
    wire::appendU32(out, localIndex);
    wire::appendU32(out, senderIndex);
    out += message2;
    writeDatagram(out, from, port);
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
    // Costs two key exchanges, and the index it must carry travels in clear with our initiation.
    if (!m_responseLimit.take(nowMs()))
        return;

    QByteArray payload;
    const auto keys = p.handshake->readMessage2(packet.mid(wire::kHeaderSize + 8), &payload);
    if (!keys)
        return; // the pending initiation stays: a forged response cannot spoil the genuine one
    p.handshake.reset();
    p.handshakeAttempts = 0;

    const bool restarted = adoptRemoteBoot(p, QCborValue::fromCbor(payload).toMap().value(kB).toByteArray());

    const qint64 now = nowMs();
    releaseIndex(p.previous);
    p.previous = std::move(p.current);
    p.previousSince = now;
    p.current = std::make_unique<SecureSession>(receiverIndex, senderIndex, *keys, true, now);
    if (Endpoint *e = findEndpoint(p, from, port)) {
        e->fails = 0;
        e->lastOk = QDateTime::currentMSecsSinceEpoch();
    }
    setPeerAddress(p, from, port);
    sendInner(p, QByteArray(1, char(wire::Inner::Keepalive))); // confirms the session to the responder
    if (restarted)
        setConnected(p, false); // see handleData
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
    bool restarted = false;
    if (p.current && p.current->localIndex() == index) {
        inner = p.current->open(packet, now);
    } else if (p.next && p.next->localIndex() == index) {
        inner = p.next->open(packet, now);
        if (inner) {
            // The peer proved it holds the session: what its initiation announced applies now,
            // and our own pending initiation, if any, has nothing left to do.
            releaseIndex(p.previous);
            p.previous = std::move(p.current);
            p.previousSince = now;
            p.current = std::move(p.next);
            restarted = adoptRemoteBoot(p, p.nextBoot);
            if (p.handshake) {
                m_indexToPeer.erase(p.handshakeIndex);
                p.handshake.reset();
                p.handshakeAttempts = 0;
            }
        }
    } else if (p.previous && p.previous->localIndex() == index) {
        inner = p.previous->open(packet, now);
    }
    if (!inner || inner->isEmpty()) {
        ++m_rejectedPackets;
        return;
    }

    followPeer(p, from, port);
    // A peer that restarted before its old session timed out here knows nothing of this device any
    // more (no hello, workspace or clipboard state): a new connection on both sides, so that this
    // side sends its hello again and the higher layers start over with it.
    if (restarted)
        setConnected(p, false);
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
            if (type == QLatin1String("hello") || type == QLatin1String("unpair") || type.startsWith(QLatin1String("group."))
                || type.startsWith(QLatin1String("stream.")))
                handleControlMessage(peer, msg); // never seen by the higher layers
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
    case wire::Inner::TunnelData:
    case wire::Inner::TunnelAck: {
        const qint64 backlog = peer.tunnel.unackedBytes();
        for (const QByteArray &message : peer.tunnel.receive(inner, nowMs()))
            handleTunnelMessage(peer, message);
        if (peer.tunnel.unackedBytes() < backlog) {
            // Room again for the streams that wait on it (bytesWritten, as a socket would say).
            std::vector<QPointer<TunnelTransport>> waiting;
            for (const auto &[key, tunnel] : peer.tunnels)
                waiting.push_back(tunnel);
            for (const QPointer<TunnelTransport> &tunnel : waiting) {
                if (tunnel)
                    tunnel->drained();
            }
        }
        break;
    }
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
        peer.capabilitiesKnown = msg.value(QStringLiteral("cap")).isArray();
        peer.capabilities.clear();
        for (const QCborValue &c : msg.value(QStringLiteral("cap")).toArray()) {
            if (peer.capabilities.size() < 32)
                peer.capabilities.append(c.toString().left(32));
        }
        // Advertised from protocol 3 on; implied by the version before that.
        const int protocol = msg.contains(QStringLiteral("pv"))
                                 ? int(std::clamp<qint64>(msg.value(QStringLiteral("pv")).toInteger(), 1, 1000))
                                 : wire::impliedProtocol(peer.version);
        const bool protocolChanged = protocol != peer.protocol;
        peer.protocol = protocol;
        if (protocolChanged && protocol != wire::kProtocolVersion)
            qCInfo(lcNode).noquote() << peer.name << "uses protocol" << protocol << "(this device:"
                                     << wire::kProtocolVersion << "): no file transfers with it";
        // Absent in versions without trust levels: Device, never Group.
        peer.remoteTrust = trustFromName(msg.value(QStringLiteral("tl")).toString(), TrustLevel::Device);
        // Its own addresses (absent before 0.1.22): its SelfReported endpoints follow them.
        peer.selfAddrs.clear();
        const QCborArray addresses = msg.value(QStringLiteral("la")).toArray();
        for (qsizetype i = 0; i < std::min<qsizetype>(addresses.size(), kMaxSelfAddresses); ++i) {
            const QHostAddress a = normalized(QHostAddress(addresses.at(i).toString()));
            if (isUnicastAddress(a) && a.protocol() == QAbstractSocket::IPv4Protocol && !peer.selfAddrs.contains(a))
                peer.selfAddrs.append(a);
        }
        peer.streamPaths = msg.value(QStringLiteral("st")).toInteger() >= kStreamPathsVersion;
        std::erase_if(peer.endpoints, [&peer](const Endpoint &e) {
            return e.source == EndpointSource::SelfReported && !peer.selfAddrs.contains(e.addr);
        });
        const quint16 udpPort = quint16(msg.value(QStringLiteral("up")).toInteger());
        if (udpPort) {
            for (const QHostAddress &a : std::as_const(peer.selfAddrs))
                addEndpoint(peer, a, udpPort, peer.tcpPort, EndpointSource::SelfReported);
        }
        if (isViaNat(peer)) {
            QStringList own;
            for (const QHostAddress &a : std::as_const(peer.selfAddrs))
                own.append(a.toString());
            qCInfo(lcNode).noquote() << peer.name << "is behind a router (NAT): seen at"
                                     << endpointText(peer.addr, peer.port) << "while its own addresses are"
                                     << own.join(QStringLiteral(", "));
        }
        saveEndpoints(peer);
        m_store.rename(peer.id, peer.name);
        // The first hello after a pairing tells whether the other side chose as much as this one.
        if (auto it = m_levelCheck.find(peer.id); it != m_levelCheck.end()) {
            const TrustLevel chosen = *it;
            m_levelCheck.erase(it);
            if (*peer.remoteTrust < chosen)
                emit pairingLevelDiffers(peer.id, chosen, *peer.remoteTrust);
        }
        if (protocolChanged)
            emit peerProtocolChanged(peer.id);
        emit peersChanged();
        // Every hello (connection, rename, level change) refreshes the group: idempotent.
        if (effectiveTrust(peer.id) == TrustLevel::Group)
            sendGroupState(peer);
    } else if (type == QLatin1String("stream.dial")) {
        handleDialRequest(peer, msg);
    } else if (type == QLatin1String("stream.unreachable")) {
        // The peer could not open a TCP connection to the address it sees this device at, which
        // is this device's own (no router translates it): something here refuses the connection.
        // Worth telling the user when the peer is on another network (a firewall that only lets
        // the local subnet in, as Windows does on public networks).
        const QHostAddress addr = normalized(QHostAddress(msg.value(QStringLiteral("a")).toString()));
        const quint16 port = quint16(msg.value(kP).toInteger());
        if (m_localAddresses.contains(addr) && !isOnLocalSubnet(peer.addr)) {
            qCInfo(lcNode) << peer.name << "at" << endpointText(peer.addr, peer.port)
                           << "could not open a TCP connection to" << endpointText(addr, port)
                           << "(a firewall here may refuse other networks)";
            emit incomingBlocked(peer.id, addr, port);
        }
    } else if (type == QLatin1String("unpair")) {
        m_store.forget(peer.id, peer.name); // it ended the pairing: members must not bring it back
        m_store.remove(peer.id);
        loseTunnels(peer, QStringLiteral("Unpaired"));
        m_levelCheck.remove(peer.id);
        dropSessions(peer);
        peer.remoteBoot.clear();
        peer.remoteTrust.reset();
        dropIntroducedBy(peer.id);
        emit peersChanged();
    } else if (type == QLatin1String("group.members") || type == QLatin1String("group.introduce")) {
        // Only a group member we share Group with may introduce devices, and only while a user
        // owns the pairings here (sessions do not run otherwise, but belt and braces).
        const TrustedPeer *sender = m_store.find(peer.id);
        if (!sender || sender->trust != TrustLevel::Group || effectiveTrust(peer.id) != TrustLevel::Group)
            return;
        if (m_usersSeparate && m_localUser.isEmpty())
            return;
        QList<QCborMap> entries;
        if (type == QLatin1String("group.introduce")) {
            entries.append(msg);
        } else {
            const QCborArray members = msg.value(QStringLiteral("m")).toArray();
            if (members.size() > kMaxIntroductions)
                qCWarning(lcNode) << "Group member" << peer.name << "lists" << members.size()
                                  << "members; only" << kMaxIntroductions << "are taken";
            for (qsizetype i = 0; i < std::min<qsizetype>(members.size(), kMaxIntroductions); ++i)
                entries.append(members.at(i).toMap());
        }
        handleGroupEntries(peer, entries);
    }
}

// --- Group introductions -----------------------------------------------------------------------

QCborMap Node::groupEntry(const QByteArray &id) const
{
    // {k public key, n name, a/p where to find it, e every endpoint known for it}: addresses
    // are hints for devices that discovery does not reach; beacons replace them. `e` lists
    // [address, UDP port, TCP port, source] (EndpointSource names), so that a member behind a
    // router is found at its own address by those on its network and at the router's by the
    // others; versions before 0.1.22 read `a`/`p` only.
    const TrustedPeer *t = m_store.find(id);
    QCborMap e;
    if (!t)
        return e;
    e.insert(kK, t->publicKey);
    e.insert(kN, t->name);
    auto it = m_peers.find(id);
    if (it != m_peers.end() && !it->second->addr.isNull()) {
        e.insert(QStringLiteral("a"), it->second->addr.toString());
        e.insert(kP, it->second->port);
    } else if (!t->lastAddress.isEmpty()) {
        e.insert(QStringLiteral("a"), t->lastAddress);
        e.insert(kP, t->lastPort);
    }
    if (it != m_peers.end()) {
        std::vector<const Endpoint *> known;
        for (const Endpoint &ep : it->second->endpoints) {
            if (ep.source != EndpointSource::Beacon)
                known.push_back(&ep);
        }
        std::stable_sort(known.begin(), known.end(), [](const Endpoint *a, const Endpoint *b) {
            return sourceRank(a->source) < sourceRank(b->source);
        });
        QCborArray endpoints;
        for (const Endpoint *ep : known) {
            if (endpoints.size() >= kMaxHintEndpoints)
                break;
            endpoints.append(QCborArray{ep->addr.toString(), ep->port, ep->tcpPort, endpointSourceName(ep->source)});
        }
        if (!endpoints.isEmpty())
            e.insert(QStringLiteral("e"), endpoints);
    }
    return e;
}

void Node::sendGroupState(Peer &peer)
{
    // The members the recipient should know: those we share Group with right now (a stored
    // Group peer that has not declared Group back would get a record of a device it refuses).
    // The recipient gets the list; the members get the recipient. Nothing is forwarded further.
    QCborArray members;
    QList<QByteArray> connectedMembers;
    for (const TrustedPeer &t : m_store.all()) {
        if (t.id == peer.id || t.trust != TrustLevel::Group || !isUsable(t.id)
            || effectiveTrust(t.id) != TrustLevel::Group)
            continue;
        members.append(groupEntry(t.id));
        auto it = m_peers.find(t.id);
        if (it != m_peers.end() && it->second->connected)
            connectedMembers.append(t.id);
    }
    if (members.isEmpty())
        return;
    QCborMap list;
    list.insert(kT, QStringLiteral("group.members"));
    list.insert(QStringLiteral("m"), members);
    sendMessage(peer.id, list);

    QCborMap intro = groupEntry(peer.id);
    intro.insert(kT, QStringLiteral("group.introduce"));
    for (const QByteArray &member : connectedMembers)
        sendMessage(member, intro);
}

void Node::handleGroupEntries(const Peer &from, const QList<QCborMap> &entries)
{
    QHash<QByteArray, TrustedPeer> accepted; // by id: a key listed twice is one member
    int byThisMember = int(m_store.introducedBy(from.id).size());
    int introduced = m_store.introducedCount();
    for (const QCborMap &entry : entries) {
        const QByteArray key = entry.value(kK).toByteArray();
        if (key.size() != crypto::kKeySize)
            continue;
        const QByteArray id = crypto::deviceIdFromPublicKey(key);
        // Never touch what exists: a PIN pairing keeps its level (a guest stays a guest), and a
        // device the user forgot stays forgotten until paired with a PIN again or allowed again;
        // the user hears of it once, since only they can change that.
        if (m_store.isForgotten(id) && !m_store.contains(id) && id != m_identity.id()) {
            if (!m_skippedNoticed.contains(id)) {
                m_skippedNoticed.insert(id);
                const QString known = m_store.forgotten().value(id);
                emit forgottenIntroductionSkipped(from.id, id,
                                                  known.isEmpty() ? entry.value(kN).toString().left(64) : known);
            }
            continue;
        }
        if (id == m_identity.id() || id == from.id || m_store.contains(id) || accepted.contains(id))
            continue;
        if (byThisMember >= kMaxIntroducedPerMember || introduced >= kMaxIntroduced) {
            qCWarning(lcNode) << "Group member" << from.name << "introduces more devices than this device keeps:"
                              << "ignoring the rest";
            break;
        }
        ++byThisMember;
        ++introduced;

        TrustedPeer t;
        t.id = id;
        t.publicKey = key;
        t.name = entry.value(kN).toString().left(64);
        t.pairedAt = QDateTime::currentDateTimeUtc();
        t.trust = TrustLevel::Group;
        t.introducedBy = from.id;
        if (m_usersSeparate)
            t.owner = m_localUser;
        // The address hint is where the introducer last saw the member: taken only when it can
        // be one device's address, so that a member cannot make us send handshakes anywhere else.
        const QHostAddress hint(entry.value(QStringLiteral("a")).toString());
        const quint16 hintPort = quint16(entry.value(kP).toInteger());
        if (isUnicastAddress(hint) && hintPort) {
            t.lastAddress = hint.toString();
            t.lastPort = hintPort;
        }
        // Every endpoint the introducer knows, under the same rule, kept as hints (the best
        // sources first, as many as hints are kept for).
        const QCborArray endpoints = entry.value(QStringLiteral("e")).toArray();
        for (qsizetype i = 0; i < std::min<qsizetype>(endpoints.size(), kMaxHintEndpoints); ++i) {
            const QCborArray ep = endpoints.at(i).toArray();
            const QHostAddress addr = normalized(QHostAddress(ep.at(0).toString()));
            const quint16 port = quint16(ep.at(1).toInteger());
            if (!isUnicastAddress(addr) || !port || t.endpoints.size() >= sourceCap(EndpointSource::GroupHint))
                continue;
            t.endpoints.append({addr.toString(), port, quint16(ep.at(2).toInteger()), EndpointSource::GroupHint, 0});
        }
        accepted.insert(id, t);
    }
    if (accepted.isEmpty())
        return;
    m_store.upsertAll(accepted.values()); // one write of peers.json for the whole list

    for (const TrustedPeer &t : std::as_const(accepted)) {
        qCInfo(lcNode) << "Group member" << t.name << "introduced by" << from.name;
        Peer &p = peerFor(t.id);
        p.publicKey = t.publicKey;
        p.name = t.name;
        p.reliable.reset();
        p.tunnel.reset();
        p.remoteBoot.clear();
        p.remoteTrust.reset();
        if (p.addr.isNull() && !t.lastAddress.isEmpty()) {
            p.addr = QHostAddress(t.lastAddress); // tried by the next tick (addrSeen 0: replaced by
            p.port = t.lastPort;                  // whatever discovery finds)
        }
        loadEndpoints(p, t);
    }
    emit peersChanged();
}

void Node::onPairingSucceeded(const TrustedPeer &paired, const QHostAddress &addr, quint16 port)
{
    TrustedPeer trusted = paired;
    // The choice is for this pairing only, however it ends.
    trusted.trust = pairingTrust(trusted.id);
    m_pairTrust.remove(trusted.id);
    m_pairSuggested.remove(trusted.id);
    if (m_usersSeparate) {
        if (m_localUser.isEmpty()) {
            emit pairingFinished(trusted.id, false, tr("Nobody is signed in on this device"));
            return;
        }
        trusted.owner = m_localUser;
    }
    m_store.unforget(trusted.id); // a PIN pairing is the user's word
    m_skippedNoticed.remove(trusted.id);
    m_store.upsert(trusted);
    m_levelCheck.insert(trusted.id, trusted.trust); // compared with what its first hello declares
    Peer &p = peerFor(trusted.id);
    p.publicKey = trusted.publicKey;
    p.name = trusted.name;
    p.reliable.reset();
    p.tunnel.reset();
    p.remoteBoot.clear();
    p.remoteTrust.reset(); // a new pairing: its level comes with its hello
    p.handshakeEpochs.clear();
    p.endpoints.clear();
    setPeerAddress(p, addr, port);
    emit pairingFinished(trusted.id, true, QString());
    emit peersChanged();
    initiateHandshake(p);
}

// --- Sending / housekeeping --------------------------------------------------------------------

bool Node::isUsable(const QByteArray &peerId) const
{
    if (!m_usersSeparate)
        return true;
    const TrustedPeer *trusted = m_store.find(peerId);
    return trusted && !m_localUser.isEmpty() && trusted->owner == m_localUser;
}

void Node::adoptUnownedPeers()
{
    // Pairings made before they were tied to users belong to the first user seen.
    if (m_localUser.isEmpty())
        return;
    for (const TrustedPeer &t : m_store.all()) {
        if (t.owner.isEmpty())
            m_store.setOwner(t.id, m_localUser);
    }
}

void Node::setLocalUser(const QString &user)
{
    if (m_usersSeparate && m_localUser == user)
        return;
    m_usersSeparate = true;
    m_localUser = user;
    adoptUnownedPeers();
    for (auto &[id, peer] : m_peers) {
        if (m_store.contains(id) && !isUsable(id)) {
            if (peer->connected)
                qCInfo(lcNode) << "Disconnecting from" << peer->name << "(paired by another user)";
            dropSessions(*peer);
        }
    }
    emit peersChanged();
}

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
                writeDatagram(packet, wire::multicastGroup(), m_options.discoveryPort);
            }
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (!entry.broadcast().isNull())
                    writeDatagram(packet, entry.broadcast(), m_options.discoveryPort);
            }
        }
    }
    // Unicast queries to trusted peers we know an address for but are not connected to (a device
    // that answers nothing is queried as seldom as it is handshaked with).
    const QByteArray query = beaconPacket(true);
    const qint64 now = nowMs();
    for (const auto &[id, p] : m_peers) {
        if (p->connected || !m_store.contains(id))
            continue;
        if (p->handshakeAttempts >= kSlowRetryAfter && !hasSteadyEndpoint(*p) && now - p->lastQueryAt < kSlowRetryMs)
            continue;
        p->lastQueryAt = now;
        if (!p->addr.isNull())
            writeDatagram(query, p->addr, p->port);
        for (const Endpoint &e : p->endpoints) {
            if (e.addr != p->addr || e.port != p->port)
                writeDatagram(query, e.addr, e.port);
        }
    }
    // Hosts the user typed that nobody answered from yet (an address to a device that was off).
    for (ManualHost &h : m_manualHosts) {
        if (h.peerId.isEmpty() && now - h.lastQuery >= kManualQueryMs) {
            h.lastQuery = now;
            writeDatagram(query, h.addr, h.port);
        }
    }
}

void Node::initiateHandshake(Peer &peer)
{
    if (peer.publicKey.isEmpty())
        return;
    // The known address and every other endpoint, best sources first, in turn (see
    // Peer::endpoints).
    std::vector<std::pair<QHostAddress, quint16>> targets;
    if (!peer.addr.isNull())
        targets.emplace_back(peer.addr, peer.port);
    std::vector<const Endpoint *> others;
    for (const Endpoint &e : peer.endpoints)
        others.push_back(&e);
    std::stable_sort(others.begin(), others.end(), [](const Endpoint *a, const Endpoint *b) {
        return sourceRank(a->source) < sourceRank(b->source);
    });
    for (const Endpoint *e : others) {
        if ((e->addr != peer.addr || e->port != peer.port) && !isThisDevice(e->addr, e->port, false))
            targets.emplace_back(e->addr, e->port);
    }
    if (targets.empty())
        return;
    const auto [addr, port] = targets[size_t(peer.nextTarget) % targets.size()];
    peer.nextTarget = (peer.nextTarget + 1) % int(targets.size());
    peer.targetCount = int(targets.size());
    sendHandshakeTo(peer, addr, port);
}

void Node::abandonHandshake(Peer &peer)
{
    if (!peer.handshake)
        return;
    m_indexToPeer.erase(peer.handshakeIndex);
    peer.handshake.reset();
    // It went unanswered (an answer clears the endpoint's fails).
    if (Endpoint *e = findEndpoint(peer, peer.handshakeAddr, peer.handshakePort))
        ++e->fails;
}

void Node::sendHandshakeTo(Peer &peer, const QHostAddress &addr, quint16 port)
{
    abandonHandshake(peer); // the previous one, if any
    if (Endpoint *e = findEndpoint(peer, addr, port))
        e->lastTry = nowMs();

    QCborMap payload;
    payload.insert(kT, qint64(++m_handshakeCounter)); // the peer's replay guard (handleHandshakeInit)
    payload.insert(kB, m_bootId);

    peer.handshake = std::make_unique<noise::IKInitiator>(m_identity.keys(), peer.publicKey, kPrologue);
    const QByteArray message1 = peer.handshake->writeMessage1(QCborValue(payload).toCbor());
    if (message1.isEmpty()) {
        peer.handshake.reset();
        return;
    }
    peer.handshakeIndex = allocateIndex(peer.id);
    peer.handshakeSentAt = nowMs();
    peer.handshakeAddr = addr;
    peer.handshakePort = port;
    ++peer.handshakeAttempts;

    QByteArray packet = wire::header(wire::Type::HandshakeInit);
    wire::appendU32(packet, peer.handshakeIndex);
    packet += message1;
    writeDatagram(packet, addr, port);
}

void Node::sendInner(Peer &peer, const QByteArray &inner)
{
    if (!peer.current || !peer.current->isConfirmed() || peer.addr.isNull())
        return;
    writeDatagram(peer.current->seal(inner, nowMs()), peer.addr, peer.port);
}

void Node::setConnected(Peer &peer, bool connected)
{
    if (peer.connected == connected)
        return;
    peer.connected = connected;
    if (connected) {
        qCInfo(lcNode) << "Connected to" << peer.name << peer.addr.toString() << peer.port;
        // The other endpoints stay: a device behind a router has several, and the next time
        // the one that worked now may not.
        peer.nextTarget = 0;
        peer.handshakeAttempts = 0;
        if (Endpoint *e = addEndpoint(peer, peer.addr, peer.port, 0, EndpointSource::Observed)) {
            e->fails = 0;
            e->lastOk = QDateTime::currentMSecsSinceEpoch();
        }
        matchManualHost(peer.id, peer.addr, peer.port);
        saveEndpoints(peer);
        m_store.setLastAddress(peer.id, peer.addr.toString(), peer.port);
        m_store.setConnectedOnce(peer.id);
        sendMessage(peer.id, helloMessage(peer.id));
        emit peerConnected(peer.id);
    } else {
        qCInfo(lcNode) << "Disconnected from" << peer.name;
        loseTunnels(peer, QStringLiteral("Connection to the device lost"));
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
        // An endpoint that came true brings the TCP port it announced (the hello refreshes it as
        // soon as the session is up).
        if (const Endpoint *e = findEndpoint(peer, addr, port); e && e->tcpPort)
            peer.tcpPort = e->tcpPort;
        peer.addr = addr;
        peer.port = port;
        if (peer.connected) {
            qCInfo(lcNode) << peer.name << "is now at" << endpointText(addr, port)
                           << (isViaNat(peer) ? "(through a router)" : "");
            if (Endpoint *e = addEndpoint(peer, addr, port, 0, EndpointSource::Observed))
                e->lastOk = QDateTime::currentMSecsSinceEpoch();
            matchManualHost(peer.id, addr, port);
            saveEndpoints(peer);
            m_store.setLastAddress(peer.id, addr.toString(), port);
        }
        emit peersChanged();
    }
}

void Node::followPeer(Peer &peer, const QHostAddress &addr, quint16 port)
{
    // Authenticated packets move the address (roaming, a NAT that changed its mapping), except
    // from one of the peer's own addresses to a detour through a router while the direct one
    // still brings packets: the router's copy of a packet (hairpin NAT) arriving late must not
    // take the session off the direct path.
    if ((peer.addr != addr || peer.port != port) && !peer.addr.isNull() && isSelfAddress(peer, peer.addr)
        && !isSelfAddress(peer, addr) && nowMs() - peer.addrSeen < kAddressStaleMs)
        return;
    setPeerAddress(peer, addr, port);
}

bool Node::adoptRemoteBoot(Peer &peer, const QByteArray &bootId)
{
    const bool restarted = !peer.remoteBoot.isEmpty() && peer.remoteBoot != bootId;
    if (restarted) {
        peer.reliable.reset(); // the peer restarted
        peer.tunnel.reset();
        loseTunnels(peer, QStringLiteral("The device restarted"));
    }
    peer.remoteBoot = bootId;
    return restarted;
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

    if (now - m_lastInterfaceCheck >= kInterfaceCheckMs)
        refreshLocalAddresses(false);

    if (now - m_lastBeacon >= (m_exploring ? kBeaconExploringMs : kBeaconIdleMs)) {
        m_lastBeacon = now;
        sendBeacons();
    }

    for (auto it = m_dialWaits.begin(); it != m_dialWaits.end();)
        it = now > it->expires ? m_dialWaits.erase(it) : std::next(it);

    bool listChanged = false;
    for (auto it = m_peers.begin(); it != m_peers.end();) {
        Peer &p = *it->second;
        if (m_store.contains(p.id) && !isUsable(p.id)) { // another user's: kept, idle
            dropSessions(p);
            ++it;
            continue;
        }
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
        std::erase_if(p.endpoints, [now](const Endpoint &e) {
            return e.source == EndpointSource::Beacon && now - e.seen > kBeaconEndpointExpiryMs;
        });

        const bool needHandshake =
            !p.current || (p.current->isInitiator() && now - p.current->createdAt() > kRekeyMs) ||
            (!p.current->isInitiator() && now - p.current->createdAt() > kRekeyMs + 15000);
        if (needHandshake && (!p.addr.isNull() || !p.endpoints.empty())) {
            // Quick retries give every target a few tries before slowing down; a device that
            // never answers is tried once a minute (kSlowRetryAfter), unless it has an endpoint
            // the user typed or it reported itself.
            const int retryMs = p.handshakeAttempts < 5 * std::max(1, p.targetCount)            ? 1000
                                : p.handshakeAttempts < kSlowRetryAfter || hasSteadyEndpoint(p) ? 5000
                                                                                                : kSlowRetryMs;
            if (!p.handshake || now - p.handshakeSentAt > retryMs)
                initiateHandshake(p);
        } else if (p.connected) {
            probeDirectPath(p, now);
        }

        p.reliable.tick(now);
        p.tunnel.tick(now);
        // Reliable data that stays unacknowledged for long is worth a line in the debug log.
        if (p.reliable.unacked() == 0) {
            p.backlogSince = -1;
        } else if (p.backlogSince < 0) {
            p.backlogSince = now;
        } else if (now - p.backlogSince > 2000 && now - p.backlogLoggedAt > 2000) {
            p.backlogLoggedAt = now;
            qCDebug(lcNode).nospace() << "Reliable backlog to " << p.name << ": " << p.reliable.unacked()
                                      << " fragments for " << (now - p.backlogSince) << " ms, window "
                                      << p.reliable.congestionWindow() << ", timeout " << p.reliable.rtoMs()
                                      << " ms; failed sends " << m_failedSends << ", rejected packets "
                                      << m_rejectedPackets;
        }
        if (p.current && now - p.current->lastSent() > kKeepaliveMs)
            sendInner(p, QByteArray(1, char(wire::Inner::Keepalive)));
        ++it;
    }
    if (listChanged)
        emit peersChanged();
}

// --- Streams -----------------------------------------------------------------------------------

std::vector<Node::StreamTarget> Node::streamTargets(const Peer &peer, bool dialBack) const
{
    // TCP endpoints, most likely first: where its packets come from when no router translates
    // it; its own addresses on a network this device is on; addresses the user typed; its other
    // own addresses; the router's address (reached only when the port is forwarded); the rest.
    // A dial-back, which the peer asks for, goes only where this device has reason to look for
    // it: where its packets come from, endpoints that worked or that the user typed, and its own
    // addresses on this device's subnets. Not to addresses it merely claims elsewhere, nor to a
    // group member's hints, so that a peer cannot turn this device into a port scanner.
    const bool viaNat = isViaNat(peer);
    const auto tcpOf = [&peer](const Endpoint &e) {
        return e.tcpPort ? e.tcpPort : peer.tcpPort ? peer.tcpPort : e.port;
    };
    std::vector<std::pair<int, StreamTarget>> ranked;
    if (!peer.addr.isNull()) {
        const bool direct = isSelfAddress(peer, peer.addr);
        ranked.push_back({viaNat ? 4 : 0, {peer.addr, peer.tcpPort ? peer.tcpPort : peer.port, direct}});
    }
    for (const Endpoint &e : peer.endpoints) {
        if (dialBack && e.source != EndpointSource::Configured && e.lastOk <= 0
            && !(e.source == EndpointSource::SelfReported && isOnLocalSubnet(e.addr)))
            continue;
        int rank = 5;
        if (e.source == EndpointSource::SelfReported)
            rank = isOnLocalSubnet(e.addr) ? 1 : 3;
        else if (e.source == EndpointSource::Configured)
            rank = 2;
        ranked.push_back({rank, {e.addr, tcpOf(e), false}});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<StreamTarget> out;
    for (const auto &[rank, target] : ranked) {
        if (!target.port || int(out.size()) >= kMaxStreamTargets || isThisDevice(target.addr, target.port, true))
            continue;
        const bool seen = std::any_of(out.cbegin(), out.cend(), [&target](const StreamTarget &t) {
            return t.addr == target.addr && t.port == target.port;
        });
        if (!seen)
            out.push_back(target);
    }
    return out;
}

SecureStream *Node::startOutgoing(Peer &peer, const QByteArray &dialNonce)
{
    // A dial-back is this node's own (it lives until it closes); a stream for a caller is theirs.
    const bool dialing = !dialNonce.isEmpty();
    SecureStream *proxy = SecureStream::pending(m_identity.keys(), peer.id, 0, dialing ? this : nullptr);
    auto o = std::make_unique<OutgoingStream>();
    o->peerId = peer.id;
    o->targets = streamTargets(peer, dialing);
    o->started = nowMs();
    o->dialFrame = dialing ? kDialMagic + dialNonce : QByteArray();
    o->timer.setInterval(kHappyEyeballsMs);
    connect(&o->timer, &QTimer::timeout, this, [this, proxy] { stepOutgoing(proxy); });
    o->timer.start();
    m_outgoing[proxy] = std::move(o);
    // The owner may give up before any path won (closing or deleting it): the attempts go too.
    connect(proxy, &SecureStream::closed, this, [this, proxy] {
        auto it = m_outgoing.find(proxy);
        if (it == m_outgoing.end())
            return;
        std::unique_ptr<OutgoingStream> o = std::move(it->second);
        m_outgoing.erase(it);
        m_dialWaits.remove(o->dialNonce);
        for (const QPointer<SecureStream> &attempt : o->attempts) {
            if (attempt) {
                attempt->disconnect(this);
                attempt->abort(QStringLiteral("Cancelled"));
                attempt->deleteLater();
            }
        }
    });
    connect(proxy, &QObject::destroyed, this, [this, proxy] {
        auto it = m_outgoing.find(proxy);
        if (it != m_outgoing.end()) {
            m_dialWaits.remove(it->second->dialNonce);
            m_outgoing.erase(it); // the attempts were its children
        }
    });
    if (dialing)
        connect(proxy, &SecureStream::closed, proxy, &QObject::deleteLater);
    // The first step after the caller connected to the stream's signals: an attempt that fails at
    // once must not close the stream before anyone listens.
    QMetaObject::invokeMethod(this, [this, proxy] { stepOutgoing(proxy); }, Qt::QueuedConnection);
    return proxy;
}

void Node::stepOutgoing(SecureStream *proxy)
{
    auto it = m_outgoing.find(proxy);
    if (it == m_outgoing.end())
        return;
    OutgoingStream &o = *it->second;
    auto peerIt = m_peers.find(o.peerId);
    const TrustedPeer *trusted = m_store.find(o.peerId);
    if (peerIt == m_peers.end() || !trusted || !isUsable(o.peerId)) {
        o.lastError = QStringLiteral("The device is not paired");
        failOutgoing(proxy);
        return;
    }
    Peer &p = *peerIt->second;
    const qint64 elapsed = nowMs() - o.started;

    // One more TCP endpoint per step (happy eyeballs): a quick one wins, a slow or dead one does
    // not hold the others back.
    if (o.next < o.targets.size()) {
        const StreamTarget target = o.targets[o.next++];
        if (m_options.connectHook && !m_options.connectHook(target.addr, target.port)) {
            o.lastError = QStringLiteral("No answer from %1 (is its firewall letting TCP through?)")
                              .arg(endpointText(target.addr, target.port));
            qCInfo(lcNode) << "Stream to" << p.name << "at" << endpointText(target.addr, target.port)
                           << "failed:" << o.lastError;
            if (target.direct)
                o.unreachable.push_back(target);
        } else {
            startAttempt(proxy,
                         SecureStream::connectTo(m_identity.keys(), trusted->publicKey, p.id, target.addr,
                                                 target.port, proxy),
                         endpointText(target.addr, target.port), target);
        }
    }

    // Through the session, with a peer that knows how: a dial-back, then a tunnel.
    const bool viaSession = o.dialFrame.isEmpty() && p.connected && p.streamPaths;
    if (viaSession && !o.dialSent && (elapsed >= kDialAfterMs || isViaNat(p) || o.targets.empty())) {
        o.dialSent = true;
        o.dialNonce = crypto::randomBytes(16);
        m_dialWaits.insert(o.dialNonce, {p.id, proxy, nowMs() + kDialWaitMs});
        QCborMap dial;
        dial.insert(kT, QStringLiteral("stream.dial"));
        dial.insert(kN, o.dialNonce);
        sendMessage(p.id, dial);
    }
    if (viaSession && !o.tunnelStarted && elapsed >= kTunnelAfterMs) {
        o.tunnelStarted = true;
        const quint64 key = kOpenedHere | p.nextTunnel++;
        auto *transport = new TunnelTransport(this, p.id, key);
        p.tunnels[key] = transport;
        sendTunnel(p.id, kTunnelOpen, key, {});
        qCInfo(lcNode) << "No TCP connection to" << p.name << "got through: tunnelling the stream through the session";
        startAttempt(proxy,
                     SecureStream::connectOver(transport, m_identity.keys(), trusted->publicKey, p.id,
                                               QStringLiteral("the tunnel"), proxy),
                     QStringLiteral("tunnel"), {});
    }
    if (m_outgoing.find(proxy) == m_outgoing.end())
        return; // an attempt settled it synchronously

    bool waiting = o.running > 0 || o.next < o.targets.size();
    if (viaSession)
        waiting = waiting || !o.tunnelStarted || (o.dialSent && m_dialWaits.contains(o.dialNonce));
    if (!waiting || elapsed > kStreamDeadlineMs)
        failOutgoing(proxy);
}

void Node::startAttempt(SecureStream *proxy, SecureStream *attempt, const QString &path, const StreamTarget &target)
{
    OutgoingStream &o = *m_outgoing.at(proxy);
    o.attempts.push_back(attempt);
    ++o.running;
    connect(attempt, &SecureStream::established, this, [this, proxy, attempt, path] {
        if (m_outgoing.find(proxy) == m_outgoing.end() || !proxy->takeOver(attempt)) {
            attempt->abort(QStringLiteral("Not needed"));
            return;
        }
        attempt->deleteLater();
        finishOutgoing(proxy, attempt, path);
    });
    connect(attempt, &SecureStream::closed, this, [this, proxy, attempt, path, target](const QString &reason) {
        attempt->deleteLater();
        auto it = m_outgoing.find(proxy);
        if (it == m_outgoing.end())
            return;
        OutgoingStream &o = *it->second;
        --o.running;
        o.lastError = reason;
        auto peerIt = m_peers.find(o.peerId);
        qCInfo(lcNode) << "Stream to" << (peerIt != m_peers.end() ? peerIt->second->name : QString()) << "via"
                       << path << "failed:" << reason;
        if (target.direct)
            o.unreachable.push_back(target);
    });
}

void Node::finishOutgoing(SecureStream *proxy, SecureStream *winner, const QString &path)
{
    auto it = m_outgoing.find(proxy);
    if (it == m_outgoing.end())
        return;
    std::unique_ptr<OutgoingStream> o = std::move(it->second);
    m_outgoing.erase(it);
    o->timer.stop();
    m_dialWaits.remove(o->dialNonce);
    for (const QPointer<SecureStream> &attempt : o->attempts) {
        if (attempt && attempt != winner) {
            attempt->disconnect(this);
            attempt->abort(QStringLiteral("Not needed"));
            attempt->deleteLater();
        }
    }
    auto peerIt = m_peers.find(o->peerId);
    Peer *peer = peerIt != m_peers.end() ? peerIt->second.get() : nullptr;
    const QString name = peer ? peer->name : QString();
    const bool direct = path != QLatin1String("dial-back") && path != QLatin1String("tunnel");
    if (direct)
        qCDebug(lcNode) << "Stream to" << name << "established at" << path;
    else
        qCInfo(lcNode) << "Stream to" << name << "established via" << path;

    // Its own address refused TCP while another path worked: the peer gets to know (a firewall
    // there, incomingBlocked), now and then.
    const qint64 now = nowMs();
    if (!direct && peer && peer->connected && peer->streamPaths && !o->unreachable.empty()
        && now - peer->unreachableReportedAt >= kUnreachableReportMs) {
        peer->unreachableReportedAt = now;
        QCborMap report;
        report.insert(kT, QStringLiteral("stream.unreachable"));
        report.insert(QStringLiteral("a"), o->unreachable.front().addr.toString());
        report.insert(kP, o->unreachable.front().port);
        sendMessage(o->peerId, report);
    }

    if (!o->dialFrame.isEmpty()) {
        // Dialing back for the peer: the stream says which request it answers, then serves it as
        // one the peer opened.
        proxy->send(o->dialFrame);
        const QByteArray peerId = o->peerId;
        ++m_acceptedStreams[peerId];
        connect(proxy, &QObject::destroyed, this, [this, peerId] {
            auto it = m_acceptedStreams.find(peerId);
            if (it != m_acceptedStreams.end() && --it->second <= 0)
                m_acceptedStreams.erase(it);
        });
        emit streamAccepted(proxy);
    }
}

void Node::failOutgoing(SecureStream *proxy)
{
    auto it = m_outgoing.find(proxy);
    if (it == m_outgoing.end())
        return;
    std::unique_ptr<OutgoingStream> o = std::move(it->second);
    m_outgoing.erase(it);
    m_dialWaits.remove(o->dialNonce);
    for (const QPointer<SecureStream> &attempt : o->attempts) {
        if (attempt) {
            attempt->disconnect(this);
            attempt->abort(QStringLiteral("Cancelled"));
            attempt->deleteLater();
        }
    }
    auto peerIt = m_peers.find(o->peerId);
    const QString name = peerIt != m_peers.end() ? peerIt->second->name : QString();
    const QString reason = o->lastError.isEmpty() ? QStringLiteral("Could not reach %1").arg(name) : o->lastError;
    qCInfo(lcNode) << (o->dialFrame.isEmpty() ? "Stream to" : "Dial-back to") << name << "failed:" << reason;
    proxy->abort(reason);
}

void Node::handleDialRequest(Peer &peer, const QCborMap &msg)
{
    // The peer could not (or may not) reach this device over TCP: this device connects to it
    // instead, and the stream carries the nonce so that the peer knows which request it answers.
    const QByteArray nonce = msg.value(kN).toByteArray();
    if (nonce.size() != 16 || peer.activeDials >= kMaxDialsPerPeer || !isUsable(peer.id))
        return;
    if (streamTargets(peer, true).empty())
        return;
    ++peer.activeDials;
    SecureStream *proxy = startOutgoing(peer, nonce);
    const QByteArray id = peer.id;
    connect(proxy, &SecureStream::closed, this, [this, id] {
        auto it = m_peers.find(id);
        if (it != m_peers.end())
            it->second->activeDials = std::max(0, it->second->activeDials - 1);
    });
}

void Node::handleDialBack(SecureStream *stream, const QByteArray &nonce)
{
    // Only the device asked, with the nonce sent to it over the session, before it expires.
    auto it = m_dialWaits.find(nonce);
    SecureStream *proxy = nullptr;
    if (it != m_dialWaits.end()) {
        if (it->peerId == stream->peerId() && nowMs() <= it->expires)
            proxy = it->proxy.data();
        m_dialWaits.erase(it);
    }
    if (!proxy || !proxy->takeOver(stream)) {
        stream->abort(QStringLiteral("Unexpected dial-back")); // closed -> deleted
        return;
    }
    stream->deleteLater();
    finishOutgoing(proxy, nullptr, QStringLiteral("dial-back"));
}

void Node::watchAccepted(SecureStream *stream, const QHostAddress &address, bool tcp)
{
    if (tcp)
        m_pendingStreams.push_back({stream, address});
    auto settle = [this, stream] {
        auto it = std::find_if(m_pendingStreams.begin(), m_pendingStreams.end(),
                               [stream](const PendingStream &p) { return p.stream == stream; });
        if (it != m_pendingStreams.end())
            m_pendingStreams.erase(it);
    };
    connect(stream, &SecureStream::established, this, [this, stream, settle, tcp] {
        settle();
        const QByteArray first = stream->firstFrame();
        if (tcp && first.startsWith(kDialMagic)) {
            stream->discardFirstFrame();
            handleDialBack(stream, first.mid(kDialMagic.size()));
            return;
        }
        const QByteArray peer = stream->peerId();
        ++m_acceptedStreams[peer];
        connect(stream, &QObject::destroyed, this, [this, peer] {
            auto it = m_acceptedStreams.find(peer);
            if (it != m_acceptedStreams.end() && --it->second <= 0)
                m_acceptedStreams.erase(it);
        });
        emit streamAccepted(stream);
    });
    // Accepted streams live until they close.
    connect(stream, &SecureStream::closed, this, settle);
    connect(stream, &SecureStream::closed, stream, &QObject::deleteLater);
}

// --- Tunnels -----------------------------------------------------------------------------------

void Node::sendTunnel(const QByteArray &peerId, quint8 op, quint64 key, const QByteArray &payload)
{
    auto it = m_peers.find(peerId);
    if (it == m_peers.end())
        return;
    QByteArray message;
    message.reserve(payload.size() + 6);
    message.append(char(op));
    message.append(char((key & kOpenedHere) ? 1 : 0)); // the sender opened it
    wire::appendU32(message, quint32(key));
    message.append(payload);
    it->second->tunnel.send(message, nowMs());
}

qint64 Node::tunnelBacklog(const QByteArray &peerId) const
{
    auto it = m_peers.find(peerId);
    return it == m_peers.end() ? 0 : it->second->tunnel.unackedBytes();
}

void Node::dropTunnel(const QByteArray &peerId, quint64 key)
{
    auto it = m_peers.find(peerId);
    if (it != m_peers.end())
        it->second->tunnels.erase(key);
}

void Node::loseTunnels(Peer &peer, const QString &reason)
{
    // The tunnel channel itself goes on, like the control one: it is reset only together with
    // the peer's state (a restart, a pairing), never because this side alone lost the session
    // (the other side may still count where it was). What it still carries for these tunnels
    // arrives for ids nobody has any more and is ignored.
    std::map<quint64, QPointer<TunnelTransport>> tunnels;
    tunnels.swap(peer.tunnels);
    for (auto &[key, tunnel] : tunnels) {
        if (tunnel)
            tunnel->lose(reason);
    }
}

void Node::handleTunnelMessage(Peer &peer, const QByteArray &message)
{
    if (message.size() < 6)
        return;
    const quint8 op = quint8(message[0]);
    const bool senderOpened = message[1] != 0;
    const quint64 key = (senderOpened ? 0 : kOpenedHere) | wire::readU32(message, 2);
    auto it = peer.tunnels.find(key);
    TunnelTransport *tunnel = it != peer.tunnels.end() ? it->second.data() : nullptr;
    switch (op) {
    case kTunnelOpen: {
        // A stream the peer opens through the session: accepted like a TCP one (its own Noise
        // handshake, only from this very peer, within the same per-peer cap).
        if (!senderOpened || it != peer.tunnels.end() || !m_store.contains(peer.id) || !isUsable(peer.id)
            || int(peer.tunnels.size()) >= kMaxStreamsPerPeer) {
            sendTunnel(peer.id, kTunnelClose, key, {});
            return;
        }
        auto *transport = new TunnelTransport(this, peer.id, key);
        peer.tunnels[key] = transport;
        const QByteArray id = peer.id;
        auto trust = [this, id](const QByteArray &remoteStatic) -> std::optional<QByteArray> {
            const TrustedPeer *trusted = m_store.find(id);
            if (!trusted || trusted->publicKey != remoteStatic || !isUsable(id) || m_acceptedStreams[id] >= kMaxStreamsPerPeer)
                return std::nullopt;
            return id;
        };
        watchAccepted(SecureStream::acceptOver(transport, m_identity.keys(), trust, this), peer.addr, false);
        break;
    }
    case kTunnelData:
        if (tunnel)
            tunnel->deliver(message.mid(6));
        break;
    case kTunnelClose:
        if (tunnel)
            tunnel->remoteClosed(); // drops it from the map
        else if (it != peer.tunnels.end())
            peer.tunnels.erase(it);
        break;
    default:
        break;
    }
}

} // namespace df
