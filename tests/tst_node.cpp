// End-to-end test: two nodes on localhost pair with a PIN, open an encrypted session and exchange
// messages. Discovery is disabled; peers are found through a unicast beacon query.

#include "cpace.h"
#include "crypto.h"
#include "filetransfer.h"
#include "natproxy.h"
#include "node.h"
#include "noise.h"
#include "pairing.h"
#include "testutil.h"
#include "wire.h"

#include <QCborArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QPointer>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

using namespace df;

namespace {
std::unique_ptr<Node> makeNode(const QString &dir)
{
    Node::Options o;
    o.configDir = dir;
    o.dataPort = 0;
    o.enableDiscovery = false;
    auto node = std::make_unique<Node>(o);
    QString error;
    if (!node->start(&error))
        qWarning() << error;
    return node;
}

bool knows(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return true;
    }
    return false;
}

// The key pair of the identity stored in `dir` (as an attacker who took the device's files, or a
// test that speaks for it, would have it).
crypto::KeyPair keysIn(const QString &dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral("identity.json")));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    crypto::KeyPair keys;
    keys.secretKey = QByteArray::fromBase64(
        QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("secretKey")).toString().toLatin1());
    keys.publicKey = crypto::publicKeyFromSecret(keys.secretKey);
    return keys;
}

// The public key of the identity stored in `dir` (what the device broadcasts in its beacons).
QByteArray publicKeyIn(const QString &dir) { return keysIn(dir).publicKey; }

QByteArray beaconFor(const QByteArray &id, const QByteArray &key, const QString &name)
{
    QCborMap m;
    m.insert(QStringLiteral("i"), id);
    m.insert(QStringLiteral("n"), name);
    m.insert(QStringLiteral("k"), key);
    m.insert(QStringLiteral("o"), QStringLiteral("windows"));
    return wire::cborPacket(wire::Type::Beacon, m);
}

quint16 portOf(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return p.port;
    }
    return 0;
}

// A behind a router with NAT (natproxy.h), B outside: see TestNode's NAT tests.
struct NatWorld {
    explicit NatWorld(test::NatProxy::Filtering filtering = test::NatProxy::Filtering::PortRestricted)
        : nat(QHostAddress(QStringLiteral("127.0.0.20")), filtering)
    {
    }
    ~NatWorld() { *slotA = nullptr; }

    void startA(const std::function<void(Node::Options &)> &tweak = {}, quint16 port = 0)
    {
        a = test::makeNodeAt(dirA.path(), addrA.toString(), [&](Node::Options &o) {
            o.datagramHook = nat.insideHook(slotA, lanA);
            if (tweak)
                tweak(o);
        }, port);
        *slotA = a.get();
    }
    void startB(quint16 port = 0)
    {
        b = test::makeNodeAt(dirB.path(), addrB.toString(),
                             [&](Node::Options &o) { test::outsideHooks(o, lanA, nat.publicAddress()); }, port);
    }

    const QHostAddress addrA{QStringLiteral("127.0.0.11")};
    const QHostAddress addrB{QStringLiteral("127.0.0.12")};
    QList<QHostAddress> lanA{addrA}; // the network behind the router
    QTemporaryDir dirA, dirB;
    test::NatProxy nat;
    test::NatProxy::Slot slotA = std::make_shared<Node *>(nullptr);
    std::unique_ptr<Node> a, b;
};

// Whether peers.json in `dir` keeps an endpoint at `addr` from `source` for device `id`.
bool storedEndpoint(const QString &dir, const QByteArray &id, const QHostAddress &addr, const QString &source)
{
    QFile f(QDir(dir).filePath(QStringLiteral("peers.json")));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        const QJsonObject o = v.toObject();
        const QByteArray key = QByteArray::fromBase64(o.value(QStringLiteral("publicKey")).toString().toLatin1());
        if (crypto::deviceIdFromPublicKey(key) != id)
            continue;
        for (const QJsonValue &e : o.value(QStringLiteral("addresses")).toArray()) {
            if (e.toObject().value(QStringLiteral("a")).toString() == addr.toString()
                && e.toObject().value(QStringLiteral("src")).toString() == source)
                return true;
        }
    }
    return false;
}

QByteArray randomData(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = char(QRandomGenerator::global()->bounded(256));
    return data;
}

// `fetcher` downloads a file `publisher` offers, through whatever path openStream finds.
bool fetchWorks(Node &publisher, Node &fetcher, const QByteArray &content, int timeoutMs)
{
    QTemporaryDir source, target;
    QFile f(source.filePath(QStringLiteral("file.bin")));
    if (!f.open(QIODevice::WriteOnly) || f.write(content) != content.size())
        return false;
    f.close();
    FileTransfer from(&publisher), to(&fetcher);
    Offer offer;
    QString error;
    if (!Offer::addPaths(&offer, {f.fileName()}, 1 << 30, 10, &error))
        return false;
    const quint64 id = from.publish(offer);
    std::optional<FetchResult> result;
    to.fetch(publisher.id(), id, target.filePath(QStringLiteral("dl")), true, 1 << 30,
             [&](const FetchResult &r) { result = r; });
    if (!QTest::qWaitFor([&] { return result.has_value(); }, timeoutMs) || !result->ok) {
        qWarning() << "fetch failed:" << (result ? result->error : QStringLiteral("timed out"));
        return false;
    }
    QFile got(QDir(target.filePath(QStringLiteral("dl"))).filePath(QStringLiteral("file.bin")));
    return got.open(QIODevice::ReadOnly) && got.readAll() == content;
}

// Frame `index` of a numbered sequence: the index (u32 big-endian), then `size` - 4 bytes of it.
QByteArray numberedFrame(int index, int size)
{
    QByteArray frame(size, char(index));
    qToBigEndian<quint32>(quint32(index), frame.data());
    return frame;
}

// Whether `frames` are frames first..first+count-1 of numberedFrame(), in order.
bool numberedInOrder(const QList<QByteArray> &frames, int first, int count, int size)
{
    if (frames.size() < first + count)
        return false;
    for (int i = first; i < first + count; ++i) {
        if (frames.at(i) != numberedFrame(i, size))
            return false;
    }
    return true;
}

// Sends frames first..first+count-1 on `stream` as its transport drains (at most 1 MB queued, as
// FileTransfer's publisher does), then closes it when `closeAfter`. Returns the next frame to send.
std::shared_ptr<int> pumpFrames(SecureStream *stream, int first, int count, int size, bool closeAfter,
                                QObject *context)
{
    auto next = std::make_shared<int>(first);
    const int end = first + count;
    auto step = [stream, next, end, size, closeAfter] {
        while (*next < end && stream->pendingBytes() < 1024 * 1024)
            stream->send(numberedFrame((*next)++, size));
        if (*next == end && closeAfter) {
            ++*next;
            stream->close();
        }
    };
    QObject::connect(stream, &SecureStream::bytesWritten, context, step);
    step();
    return next;
}

// A stream `opener` opens to `acceptor`, which pauses it as soon as it is accepted: the frame that
// authenticated it ("hello") is still delivered, nothing after it until resumed.
struct PausedStream {
    bool open(Node &opener, Node &acceptor, int timeoutMs)
    {
        QObject::connect(&acceptor, &Node::streamAccepted, &guard, [this](SecureStream *stream) {
            if (served)
                return;
            served = stream;
            stream->setReadPaused(true);
            QObject::connect(stream, &SecureStream::received, &guard, [this](const QByteArray &frame) {
                if (frame != "hello")
                    got.append(frame);
            });
            QObject::connect(stream, &SecureStream::closed, &guard, [this](const QString &reason) {
                closedReason = reason;
                gotAtClose = int(got.size());
            });
        });
        client.reset(opener.openStream(acceptor.id()));
        if (!client)
            return false;
        QObject::connect(client.get(), &SecureStream::established, &guard, [this] { client->send("hello"); });
        QObject::connect(client.get(), &SecureStream::closed, &guard, [this] { clientClosed = true; });
        return QTest::qWaitFor([this] { return served && client->isEstablished(); }, timeoutMs);
    }

    std::unique_ptr<SecureStream> client;
    QPointer<SecureStream> served;
    QList<QByteArray> got; // what `served` received after "hello"
    std::optional<QString> closedReason;
    int gotAtClose = -1;
    bool clientClosed = false;
    QObject guard; // last: its connections go first
};

// Read pause from `opener` to `acceptor`: `held` frames sent while paused arrive in order on
// resume; then a few more frames sent while paused, followed by the sender's close, all arrive
// before `closed` (the close reaches the paused side first: over TCP, RemoteHostClosedError).
void checkReadPause(Node &opener, Node &acceptor, int held, bool tcp, int openTimeoutMs)
{
    constexpr int kSize = 60000;
    PausedStream s;
    QVERIFY(s.open(opener, acceptor, openTimeoutMs));

    const auto next = pumpFrames(s.client.get(), 0, held, kSize, false, &s.guard);
    QTest::qWait(1500);
    QCOMPARE(s.got.size(), 0);
    QVERIFY(!s.closedReason);
    if (tcp) { // far more than the socket reads ahead and the system buffers: the sender is held
        const int sent = *next;
        QTest::qWait(500);
        QVERIFY2(sent < held && *next == sent, "the sender was never held");
    }
    s.served->setReadPaused(false);
    QTRY_COMPARE_WITH_TIMEOUT(s.got.size(), held, 60000);
    QVERIFY(numberedInOrder(s.got, 0, held, kSize));

    constexpr int kTail = 8; // well under TcpTransport::kReadBufferBytes: the close is seen while paused
    s.served->setReadPaused(true);
    pumpFrames(s.client.get(), held, kTail, kSize, true, &s.guard);
    QTRY_VERIFY_WITH_TIMEOUT(s.clientClosed, 10000);
    QTest::qWait(1500);
    QVERIFY2(!s.closedReason, qPrintable(s.closedReason.value_or(QString())));
    QCOMPARE(s.got.size(), held);
    QVERIFY(s.served);
    s.served->setReadPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(s.closedReason.has_value(), 10000);
    QCOMPARE(*s.closedReason, QStringLiteral("Connection closed"));
    QCOMPARE(s.gotAtClose, held + kTail);
    QVERIFY(numberedInOrder(s.got, 0, held + kTail, kSize));
}

// One end of an in-memory connection whose deliveries the test drives: what one end writes waits
// in the other until the test emits its readyRead.
class PipeTransport : public StreamTransport
{
public:
    PipeTransport *peer = nullptr;
    QByteArray in;
    bool ended = false;

    void write(const QByteArray &bytes) override
    {
        if (!ended && peer)
            peer->in += bytes;
    }
    QByteArray readAll() override { return std::exchange(in, {}); }
    qint64 bytesToWrite() const override { return 0; }
    qint64 bytesAvailable() const override { return in.size(); }
    void disconnectFromHost() override { ended = true; }
    void abort() override { ended = true; }
    bool isUnconnected() const override { return ended; }
    QString errorString() const override { return QStringLiteral("pipe"); }
};
} // namespace

class TestNode : public QObject
{
    Q_OBJECT
private slots:
    void pairConnectAndTalk()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(true);

        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);

        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(pinShown.size(), 1);
        QTRY_COMPARE(pinAsked.size(), 1);
        const QString pin = pinShown.first().at(2).toString();
        QCOMPARE(pin.size(), 6);
        QVERIFY(a->submitPin(b->id(), pin));

        QTRY_COMPARE(finishedA.size(), 1);
        QTRY_COMPARE(finishedB.size(), 1);
        QVERIFY(finishedA.first().at(1).toBool());
        QVERIFY(finishedB.first().at(1).toBool());

        QTRY_VERIFY(a->isConnected(b->id()));
        QTRY_VERIFY(b->isConnected(a->id()));

        QSignalSpy gotB(b.get(), &Node::messageReceived);
        QCborMap msg;
        msg.insert(QStringLiteral("t"), QStringLiteral("test"));
        msg.insert(QStringLiteral("big"), QByteArray(20000, 'x'));
        QVERIFY(a->sendMessage(b->id(), msg));
        QTRY_COMPARE(gotB.size(), 1);
        QCOMPARE(gotB.first().at(1).value<QCborMap>(), msg);

        QSignalSpy datagrams(a.get(), &Node::datagramReceived);
        QVERIFY(b->sendDatagram(a->id(), "mouse"));
        QTRY_COMPARE(datagrams.size(), 1);
        QCOMPARE(datagrams.first().at(1).toByteArray(), QByteArray("mouse"));

        // Restarting B (same identity) must reconnect automatically.
        const QByteArray idB = b->id();
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);
        b = makeNode(dirB.path());
        QCOMPARE(b->id(), idB);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(idB) && b->isConnected(a->id()), 10000);

        QSignalSpy gotB2(b.get(), &Node::messageReceived);
        QVERIFY(a->sendMessage(idB, msg));
        QTRY_COMPARE(gotB2.size(), 1);

        // Unpairing on one side removes the trust on both.
        a->unpair(idB);
        QTRY_VERIFY(!b->isConnected(a->id()));
        bool stillTrusted = false;
        for (const PeerView &p : b->peers())
            stillTrusted |= p.id == a->id() && p.trusted;
        QVERIFY(!stillTrusted);
    }

    void reconnectsToTheLastAddressAfterARestart()
    {
        // A paired device that discovery cannot reach (another network, added by IP) is found
        // again after a restart: its last address is kept with the pairing.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        const QByteArray idA = a->id();
        a.reset(); // no discovery here: only the stored address can bring it back
        auto again = test::makeNode(dirA.path());
        QCOMPARE(again->id(), idA);
        QTRY_VERIFY_WITH_TIMEOUT(again->isConnected(b->id()) && b->isConnected(idA), 10000);
    }

    void peerRestartedBeforeTimingOutIsANewConnection()
    {
        // A restarts faster than B's session times out (a service restart, an update): B still
        // holds the old session when the new instance connects. B must treat it as a new
        // connection, so that A gets B's hello again (without it A knows no version, and the
        // clipboard and drags refuse B) and B's higher layers start over with A.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        const QByteArray idA = a->id();
        QSignalSpy disconnected(b.get(), &Node::peerDisconnected);
        QSignalSpy connected(b.get(), &Node::peerConnected);
        a.reset();
        QVERIFY(b->isConnected(idA)); // the old session has not timed out
        auto again = test::makeNode(dirA.path());
        QTRY_VERIFY_WITH_TIMEOUT(test::connectedWithHello(*again, b->id()), 10000);
        QVERIFY(b->isConnected(idA));
        QCOMPARE(disconnected.size(), 1);
        QCOMPARE(connected.size(), 1);
        QCOMPARE(connected.first().first().toByteArray(), idA);
    }

    void helloSaysWhichCapabilitiesItHas()
    {
        // The service announces optional features (hello "cap", e.g. kDragCapability while an
        // agent that runs native drags is connected); paired devices learn of a change at once.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        auto capabilitiesOf = [](const Node &node, const QByteArray &id) {
            for (const PeerView &p : node.peers()) {
                if (p.id == id)
                    return p.capabilitiesKnown ? p.capabilities : QStringList{QStringLiteral("unknown")};
            }
            return QStringList{QStringLiteral("absent")};
        };
        QTRY_COMPARE(capabilitiesOf(*b, a->id()), QStringList());
        a->setCapability(QLatin1String(kDragCapability), true);
        QTRY_COMPARE(capabilitiesOf(*b, a->id()), QStringList{QLatin1String(kDragCapability)});
        a->setCapability(QLatin1String(kDragCapability), false);
        QTRY_COMPARE(capabilitiesOf(*b, a->id()), QStringList());
    }

    void helloAdvertisesProtocol()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QCOMPARE(a->peerProtocol(a->id()), wire::kProtocolVersion); // itself
        QVERIFY(a->transfersCompatible(a->id()));
        QVERIFY(a->transfersCompatible(b->id())); // unknown: not refused up front
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(test::viewOf(*b, a->id())->protocol, wire::kProtocolVersion);
        QCOMPARE(b->peerProtocol(a->id()), wire::kProtocolVersion);
        QVERIFY(b->transfersCompatible(a->id()));
        QTRY_COMPARE(test::viewOf(*a, b->id())->protocol, wire::kProtocolVersion);
    }

    void helloWithoutProtocolIsImplied()
    {
        // A device from before protocol 3 advertises none: its version says which it has. It is
        // still discovered, paired and connected (the hello stays compatible); only transfers are
        // off with it.
        QCOMPARE(wire::impliedProtocol(QStringLiteral("0.1.30")), 2);
        QCOMPARE(wire::impliedProtocol(QStringLiteral("0.1.22")), 2);
        QCOMPARE(wire::impliedProtocol(QStringLiteral("0.1.20")), 2);
        QCOMPARE(wire::impliedProtocol(QStringLiteral("0.1.19")), 1);
        QCOMPARE(wire::impliedProtocol(QString()), 1);
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QString version = QStringLiteral("0.1.30");
        std::optional<int> pv; // none: an older device
        a->setHelloOverrideForTests([&](QCborMap &hello) {
            hello.insert(QStringLiteral("v"), version);
            hello.remove(QStringLiteral("pv"));
            if (pv)
                hello.insert(QStringLiteral("pv"), *pv);
        });
        QSignalSpy changed(b.get(), &Node::peerProtocolChanged);
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(b->peerProtocol(a->id()), 2);
        QVERIFY(!b->transfersCompatible(a->id()));
        QVERIFY(test::viewOf(*b, a->id())->connected);
        QVERIFY(changed.size() >= 1);
        QCOMPARE(changed.last().at(0).toByteArray(), a->id());

        // Every hello is read again (a capability change resends it).
        version = QStringLiteral("0.1.19");
        a->setCapability(QLatin1String(kDragCapability), true);
        QTRY_COMPARE(b->peerProtocol(a->id()), 1);
        pv = 4; // a newer device
        a->setCapability(QLatin1String(kDragCapability), false);
        QTRY_COMPARE(b->peerProtocol(a->id()), 4);
        QVERIFY(!b->transfersCompatible(a->id()));
        pv = 1000000; // bounded
        a->setCapability(QLatin1String(kDragCapability), true);
        QTRY_COMPARE(b->peerProtocol(a->id()), 1000);
        a->setHelloOverrideForTests({});
        a->setCapability(QLatin1String(kDragCapability), false);
        QTRY_COMPARE(b->peerProtocol(a->id()), wire::kProtocolVersion);
        QVERIFY(b->transfersCompatible(a->id()));
    }

    void pairingAsAGuestIsKeptAndNeverGrantsTheView()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b, std::nullopt, TrustLevel::Guest)); // B shows the PIN, makes A a guest
        QVERIFY(b->isGuest(a->id()));
        QVERIFY(!a->isGuest(b->id()));
        b->setViewAllowed(a->id(), true);
        QVERIFY(!b->isViewAllowed(a->id()));

        // A grant goes when a device becomes a guest; both survive a restart.
        a->setViewAllowed(b->id(), true);
        QVERIFY(a->isViewAllowed(b->id()));
        a->setTrust(b->id(), TrustLevel::Guest);
        QVERIFY(!a->isViewAllowed(b->id()));
        const QByteArray idB = b->id();
        b.reset();
        a.reset();
        auto again = test::makeNode(dirA.path());
        QVERIFY(again->isGuest(idB));
        QCOMPARE(again->trustLevel(idB), TrustLevel::Guest);
        again->setTrust(idB, TrustLevel::Device);
        QVERIFY(!again->isGuest(idB));
        QCOMPARE(again->trustLevel(idB), TrustLevel::Device);
        QVERIFY(!again->isViewAllowed(idB));
    }

    // --- Trust levels and groups ---------------------------------------------------------------

    void groupIntroducesNewMembers()
    {
        // A-B and A-D are paired as "all my devices": A introduces them to each other, so B and
        // D trust each other as group members, connect, and keep it after a restart. No PIN is
        // shown or asked for that pair, and the group messages never reach the higher layers.
        QTemporaryDir dirA, dirB, dirD;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QCOMPARE(a->trustLevel(b->id()), TrustLevel::Group);
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Group);
        QTRY_COMPARE(a->effectiveTrust(b->id()), TrustLevel::Group);
        QTRY_COMPARE(b->effectiveTrust(a->id()), TrustLevel::Group);

        QSignalSpy pinShownB(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAskedB(b.get(), &Node::pairingPinRequired);
        QSignalSpy pinAskedD(d.get(), &Node::pairingPinRequired);
        QSignalSpy messagesB(b.get(), &Node::messageReceived);
        QSignalSpy messagesD(d.get(), &Node::messageReceived);
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));

        QTRY_VERIFY(b->isTrusted(d->id()) && d->isTrusted(b->id()));
        QCOMPARE(b->trustLevel(d->id()), TrustLevel::Group);
        QCOMPARE(d->trustLevel(b->id()), TrustLevel::Group);
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(d->id()) && d->isConnected(b->id()), 10000);
        QTRY_COMPARE(b->effectiveTrust(d->id()), TrustLevel::Group);
        QCOMPARE(pinShownB.size(), 0);
        QCOMPARE(pinAskedB.size(), 0);
        QCOMPARE(pinAskedD.size(), 0);
        QCOMPARE(messagesB.size(), 0);
        QCOMPARE(messagesD.size(), 0);
        bool reported = false;
        for (const PeerView &p : b->peers())
            reported |= p.id == d->id() && p.trusted && p.trust == TrustLevel::Group && p.effectiveTrust == TrustLevel::Group;
        QVERIFY(reported);

        // B restarts: the introduction was stored, with D's address.
        const QByteArray idB = b->id();
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!d->isConnected(idB), 10000);
        b = test::makeNode(dirB.path());
        QVERIFY(b->isTrusted(d->id()));
        QCOMPARE(b->trustLevel(d->id()), TrustLevel::Group);
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(d->id()) && d->isConnected(idB), 10000);

        // D connected: it stands on its own now, so forgetting A (which introduced it) keeps it.
        b->unpair(a->id());
        QVERIFY(b->isTrusted(d->id()));
        QVERIFY(!b->isTrusted(a->id()));
    }

    void introductionsAreCappedAndJunkGoesWithTheIntroducer()
    {
        // A member may have at most 128 devices introduced at a time (a group is far smaller):
        // one that keeps listing fresh keys enrolls nothing past that. What it introduced and this
        // device never talked to goes when the member is forgotten or demoted: a compromised
        // member leaves no lasting list of hosts to send handshakes to.
        QTemporaryDir dirA, dirD;
        auto a = test::makeNode(dirA.path());
        auto d = test::makeNode(dirD.path());
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(a->effectiveTrust(d->id()), TrustLevel::Group);
        auto members = [](int count) {
            QCborArray m;
            for (int i = 0; i < count; ++i) {
                QCborMap e;
                e.insert(QStringLiteral("k"), crypto::generateKeyPair().publicKey);
                e.insert(QStringLiteral("n"), QStringLiteral("x"));
                m.append(e);
            }
            QCborMap msg;
            msg.insert(QStringLiteral("t"), QStringLiteral("group.members"));
            msg.insert(QStringLiteral("m"), m);
            return msg;
        };
        for (int i = 0; i < 4; ++i)
            QVERIFY(d->sendMessage(a->id(), members(64)));
        QTRY_COMPARE_WITH_TIMEOUT(a->peers().size(), 1 + 128, 10000);
        QTest::qWait(500);
        QCOMPARE(a->peers().size(), 1 + 128);

        // Forgetting D takes its never-heard-from introductions with it, in the store too (D itself
        // may still be listed as a nearby untrusted device for a moment).
        a->unpair(d->id());
        QVERIFY(a->peers().size() <= 1);
        for (const PeerView &p : a->peers())
            QVERIFY(!p.trusted);
        PeerStore store;
        QVERIFY(store.load(dirA.path()));
        QVERIFY(store.all().isEmpty());
    }

    void groupRequiresBothSides()
    {
        // Group needs both sides to choose it; otherwise the stricter level applies and nobody
        // is introduced. A-B share Group; D, E and G each miss it on one side.
        QTemporaryDir dirA, dirB, dirD, dirE, dirG;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        auto e = test::makeNode(dirE.path());
        auto g = test::makeNode(dirG.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Device)); // D: "this device only"
        QVERIFY(test::pairNodes(*a, *e, TrustLevel::Device, TrustLevel::Group)); // A: "this device only"
        QVERIFY(test::pairNodes(*a, *g, TrustLevel::Guest, TrustLevel::Group));  // G is a guest on A

        QTRY_COMPARE(a->effectiveTrust(d->id()), TrustLevel::Device);
        QCOMPARE(a->trustLevel(d->id()), TrustLevel::Group);
        QTRY_COMPARE(d->effectiveTrust(a->id()), TrustLevel::Device);
        QTRY_COMPARE(e->effectiveTrust(a->id()), TrustLevel::Device);
        QCOMPARE(e->trustLevel(a->id()), TrustLevel::Group);
        QTRY_COMPARE(g->effectiveTrust(a->id()), TrustLevel::Guest);
        QTRY_COMPARE(a->effectiveTrust(g->id()), TrustLevel::Guest);
        QTest::qWait(1000);
        for (const Node *x : {d.get(), e.get(), g.get()}) {
            QVERIFY(!b->isTrusted(x->id()));
            QVERIFY(!x->isTrusted(b->id()));
        }
        QVERIFY(!d->isTrusted(e->id()) && !e->isTrusted(d->id()));
    }

    void guestsAreNeverIntroduced()
    {
        QTemporaryDir dirA, dirB, dirD, dirG;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        auto g = test::makeNode(dirG.path());
        QVERIFY(test::pairNodes(*a, *g, TrustLevel::Guest, TrustLevel::Device)); // G is a guest on A
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_VERIFY(b->isTrusted(d->id()) && d->isTrusted(b->id()));
        QTest::qWait(500);
        QVERIFY(!b->isTrusted(g->id()) && !d->isTrusted(g->id()));
        QVERIFY(!g->isTrusted(b->id()) && !g->isTrusted(d->id()));

        // A guest (or any device without Group) cannot introduce anyone to A.
        QTRY_VERIFY(g->isConnected(a->id()));
        const crypto::KeyPair fake = crypto::generateKeyPair();
        QCborMap intro;
        intro.insert(QStringLiteral("t"), QStringLiteral("group.introduce"));
        intro.insert(QStringLiteral("k"), fake.publicKey);
        intro.insert(QStringLiteral("n"), QStringLiteral("x"));
        QSignalSpy messagesA(a.get(), &Node::messageReceived);
        QVERIFY(g->sendMessage(a->id(), intro));
        QTest::qWait(500);
        QVERIFY(!a->isTrusted(crypto::deviceIdFromPublicKey(fake.publicKey)));
        QCOMPARE(messagesA.size(), 0); // and the message never reached the higher layers
        QCOMPARE(a->peers().size(), 3);
    }

    void introductionsOnlyFromGroupPeers()
    {
        // A device paired as "this device only" cannot introduce anyone, and a group member's
        // introductions with a bad key or our own key are ignored.
        QTemporaryDir dirA, dirB, dirD;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Device, TrustLevel::Device));
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(d->effectiveTrust(a->id()), TrustLevel::Group);

        auto introduce = [](const QByteArray &key, const QString &type) {
            QCborMap e;
            e.insert(QStringLiteral("k"), key);
            e.insert(QStringLiteral("n"), QStringLiteral("x"));
            if (type == QLatin1String("group.members")) {
                QCborMap m;
                m.insert(QStringLiteral("t"), type);
                m.insert(QStringLiteral("m"), QCborArray{e});
                return m;
            }
            e.insert(QStringLiteral("t"), type);
            return e;
        };
        const crypto::KeyPair fake = crypto::generateKeyPair();
        const QByteArray fakeId = crypto::deviceIdFromPublicKey(fake.publicKey);
        QVERIFY(b->sendMessage(a->id(), introduce(fake.publicKey, QStringLiteral("group.introduce"))));
        QVERIFY(b->sendMessage(a->id(), introduce(fake.publicKey, QStringLiteral("group.members"))));
        QTest::qWait(500);
        QVERIFY(!a->isTrusted(fakeId));

        QByteArray aKey;
        {
            QFile f(QDir(dirA.path()).filePath(QStringLiteral("identity.json")));
            QVERIFY(f.open(QIODevice::ReadOnly));
            aKey = crypto::publicKeyFromSecret(QByteArray::fromBase64(
                QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("secretKey")).toString().toLatin1()));
        }
        QVERIFY(d->sendMessage(a->id(), introduce(QByteArray(31, 'k'), QStringLiteral("group.introduce"))));
        QVERIFY(d->sendMessage(a->id(), introduce(QByteArray(33, 'k'), QStringLiteral("group.members"))));
        QVERIFY(d->sendMessage(a->id(), introduce(aKey, QStringLiteral("group.introduce"))));
        QTest::qWait(500);
        QCOMPARE(a->peers().size(), 2);
        QVERIFY(!a->isTrusted(a->id()));
        QVERIFY(!a->isTrusted(crypto::deviceIdFromPublicKey(QByteArray(31, 'k'))));
    }

    void forgottenDeviceIsNotReintroduced()
    {
        QTemporaryDir dirA, dirB, dirD;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(d->id()) && d->isConnected(b->id()), 10000);

        // B forgets D: D drops B too, and A's next hello (a rename re-sends it) brings neither back.
        b->unpair(d->id());
        QVERIFY(!b->isTrusted(d->id()));
        QTRY_VERIFY(!d->isTrusted(b->id()));
        QVERIFY(a->setName(QStringLiteral("A2")));
        QTest::qWait(1000);
        QVERIFY(!b->isTrusted(d->id()));
        QVERIFY(!d->isTrusted(b->id()));
        QVERIFY(a->isTrusted(b->id()) && a->isTrusted(d->id()));

        // The tombstone survives a restart of B.
        const QByteArray idB = b->id();
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);
        b = test::makeNode(dirB.path());
        QVERIFY(!b->isTrusted(d->id()));
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(a->id()) && a->isConnected(idB), 10000);
        QTest::qWait(1000); // A introduced D again on B's hello
        QVERIFY(!b->isTrusted(d->id()));

        // A PIN pairing is the user's word: B and D are back, as group members, and what
        // another member does later (A forgets D) does not touch that.
        QVERIFY(test::pairNodes(*b, *d, TrustLevel::Group, TrustLevel::Group));
        QCOMPARE(b->trustLevel(d->id()), TrustLevel::Group);
        QCOMPARE(d->trustLevel(b->id()), TrustLevel::Group);
        a->unpair(d->id());
        QTRY_VERIFY(!d->isTrusted(a->id()));
        QTest::qWait(500);
        QVERIFY(b->isTrusted(d->id()) && d->isTrusted(b->id()));
        QVERIFY(b->isConnected(d->id()));
    }

    void introductionsNeverModifyExistingRecords()
    {
        // B and D paired each other directly (D is a guest on B, B "this device only" on D)
        // before A, a group member of both, introduces them: their records stay as they were.
        QTemporaryDir dirA, dirB, dirD;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        QVERIFY(test::pairNodes(*b, *d, TrustLevel::Guest, TrustLevel::Device));
        d->setViewAllowed(b->id(), true);
        QVERIFY(d->isViewAllowed(b->id()));
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(a->effectiveTrust(d->id()), TrustLevel::Group);
        QTest::qWait(1000);
        QCOMPARE(b->trustLevel(d->id()), TrustLevel::Guest);
        QCOMPARE(d->trustLevel(b->id()), TrustLevel::Device);
        QVERIFY(d->isViewAllowed(b->id()));
        QVERIFY(b->isConnected(d->id()));
    }

    void introducedPeersBelongToTheLocalUser()
    {
        QTemporaryDir dirA, dirB, dirD;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        a->setLocalUser(QStringLiteral("alice"));
        b->setLocalUser(QStringLiteral("alice"));
        d->setLocalUser(QStringLiteral("alice"));
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_VERIFY(b->isTrusted(d->id()) && d->isTrusted(b->id()));
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(d->id()), 10000);
        auto otherUser = [](const Node &n, const QByteArray &id) {
            for (const PeerView &p : n.peers()) {
                if (p.id == id)
                    return p.otherUser;
            }
            return false;
        };
        QVERIFY(!otherUser(*b, d->id()));

        // Another user signs in on B: the introduced device is out of reach, like a paired one.
        b->setLocalUser(QStringLiteral("bob"));
        QVERIFY(otherUser(*b, d->id()));
        QVERIFY(!b->isConnected(d->id()));
        QVERIFY(!b->sendMessage(d->id(), QCborMap{{QStringLiteral("t"), QStringLiteral("x")}}));
        b->setLocalUser(QStringLiteral("alice"));
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(d->id()), 10000);
    }

    void switchingToGroupLaterIntroduces()
    {
        // Devices paired as "this device only" become a group when both sides of each pair
        // switch; demoting one stops introducing it (what others stored remains).
        QTemporaryDir dirA, dirB, dirD, dirF;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto d = test::makeNode(dirD.path());
        auto f = test::makeNode(dirF.path());
        QVERIFY(test::pairNodes(*a, *b));
        QVERIFY(test::pairNodes(*a, *d));
        a->setTrust(b->id(), TrustLevel::Group);
        b->setTrust(a->id(), TrustLevel::Group);
        a->setTrust(d->id(), TrustLevel::Group);
        QTest::qWait(500);
        QVERIFY(!b->isTrusted(d->id()) && !d->isTrusted(b->id())); // D has not agreed yet
        QTRY_COMPARE(a->effectiveTrust(d->id()), TrustLevel::Device);
        d->setTrust(a->id(), TrustLevel::Group);
        QTRY_VERIFY(b->isTrusted(d->id()) && d->isTrusted(b->id()));
        QCOMPARE(b->trustLevel(d->id()), TrustLevel::Group);
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(d->id()) && d->isConnected(b->id()), 10000);

        // A demotes D (and B forgets it, so that B, a member storing D, does not introduce it
        // either): a new member F learns B from A, but not D.
        b->unpair(d->id());
        QTRY_VERIFY(!d->isTrusted(b->id()));
        a->setTrust(d->id(), TrustLevel::Device);
        QTRY_COMPARE(d->effectiveTrust(a->id()), TrustLevel::Device);
        QVERIFY(test::pairNodes(*a, *f, TrustLevel::Group, TrustLevel::Group));
        QTRY_VERIFY(f->isTrusted(b->id()) && b->isTrusted(f->id()));
        QTest::qWait(500);
        QVERIFY(!f->isTrusted(d->id()) && !d->isTrusted(f->id()));
        QVERIFY(a->isTrusted(d->id()) && d->isTrusted(a->id()));
    }

    void storeKeepsEndpoints()
    {
        // The endpoint list round-trips through peers.json; beacon claims and junk never load.
        QTemporaryDir dir;
        const crypto::KeyPair key = crypto::generateKeyPair();
        const QByteArray id = crypto::deviceIdFromPublicKey(key.publicKey);
        QJsonArray addresses;
        auto endpoint = [](const char *a, int port, const char *src) {
            QJsonObject e;
            e.insert(QStringLiteral("a"), QString::fromLatin1(a));
            e.insert(QStringLiteral("p"), port);
            e.insert(QStringLiteral("src"), QString::fromLatin1(src));
            return e;
        };
        QJsonObject typed = endpoint("10.1.2.3", 24801, "configured");
        typed.insert(QStringLiteral("tp"), 24900);
        typed.insert(QStringLiteral("ok"), 1700000000000.0);
        addresses.append(typed);
        addresses.append(endpoint("192.168.1.9", 24801, "self"));
        addresses.append(endpoint("10.9.9.9", 1, "beacon"));
        addresses.append(endpoint("10.9.9.8", 1, "nonsense"));
        addresses.append(endpoint("", 1, "self"));
        QJsonObject o;
        o.insert(QStringLiteral("publicKey"), QString::fromLatin1(key.publicKey.toBase64()));
        o.insert(QStringLiteral("name"), QStringLiteral("x"));
        o.insert(QStringLiteral("addresses"), addresses);
        QFile f(QDir(dir.path()).filePath(QStringLiteral("peers.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(QJsonArray{o}).toJson());
        f.close();

        PeerStore store;
        store.load(dir.path());
        QVERIFY(store.find(id));
        const QList<StoredEndpoint> loaded = store.find(id)->endpoints;
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded[0].address, QStringLiteral("10.1.2.3"));
        QCOMPARE(loaded[0].tcpPort, quint16(24900));
        QCOMPARE(loaded[0].source, EndpointSource::Configured);
        QCOMPARE(loaded[0].lastOk, qint64(1700000000000));
        QCOMPARE(loaded[1].source, EndpointSource::SelfReported);

        store.setEndpoints(id, {loaded[1]});
        PeerStore again;
        again.load(dir.path());
        QCOMPARE(again.find(id)->endpoints, QList<StoredEndpoint>{loaded[1]});
    }

    void storeKeepsTrustLevelsAndForgottenDevices()
    {
        QTemporaryDir dir;
        const crypto::KeyPair guest = crypto::generateKeyPair();
        const crypto::KeyPair device = crypto::generateKeyPair();
        const crypto::KeyPair group = crypto::generateKeyPair();
        auto entry = [](const crypto::KeyPair &k, const char *flag) {
            QJsonObject o;
            o.insert(QStringLiteral("publicKey"), QString::fromLatin1(k.publicKey.toBase64()));
            o.insert(QStringLiteral("name"), QStringLiteral("x"));
            if (flag)
                o.insert(QString::fromLatin1(flag), true);
            return o;
        };
        {
            QFile f(QDir(dir.path()).filePath(QStringLiteral("peers.json")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(QJsonArray{entry(guest, "guest"), entry(device, nullptr), entry(group, "group")}).toJson());
        }
        const QByteArray guestId = crypto::deviceIdFromPublicKey(guest.publicKey);
        const QByteArray deviceId = crypto::deviceIdFromPublicKey(device.publicKey);
        const QByteArray groupId = crypto::deviceIdFromPublicKey(group.publicKey);

        PeerStore store;
        QVERIFY(store.load(dir.path()));
        QCOMPARE(store.find(guestId)->trust, TrustLevel::Guest);
        QVERIFY(store.find(guestId)->isGuest());
        QCOMPARE(store.find(deviceId)->trust, TrustLevel::Device);
        QCOMPARE(store.find(groupId)->trust, TrustLevel::Group);
        QVERIFY(!store.isForgotten(guestId));

        // Flags are written only for Guest and Group; the tombstones live in forgotten.json.
        store.setTrust(deviceId, TrustLevel::Group);
        store.setTrust(groupId, TrustLevel::Device);
        store.forget(QByteArray("gone"), QStringLiteral("Old laptop"));
        store.forget(guestId, QString());
        store.unforget(guestId);
        PeerStore again;
        QVERIFY(again.load(dir.path()));
        QCOMPARE(again.find(guestId)->trust, TrustLevel::Guest);
        QCOMPARE(again.find(deviceId)->trust, TrustLevel::Group);
        QCOMPARE(again.find(groupId)->trust, TrustLevel::Device);
        QVERIFY(again.isForgotten(QByteArray("gone")));
        QCOMPARE(again.forgotten().value(QByteArray("gone")), QString("Old laptop")); // for the list
        QVERIFY(!again.isForgotten(guestId));
        QFile f(QDir(dir.path()).filePath(QStringLiteral("peers.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
            const QJsonObject o = v.toObject();
            const QByteArray key = QByteArray::fromBase64(o.value(QStringLiteral("publicKey")).toString().toLatin1());
            QCOMPARE(o.contains(QStringLiteral("guest")), key == guest.publicKey);
            QCOMPARE(o.contains(QStringLiteral("group")), key == device.publicKey);
        }
        QVERIFY(QFile::exists(QDir(dir.path()).filePath(QStringLiteral("forgotten.json"))));

        // 0.1.20 and 0.1.21 wrote bare ids: still tombstones, with no name.
        QFile old(QDir(dir.path()).filePath(QStringLiteral("forgotten.json")));
        QVERIFY(old.open(QIODevice::WriteOnly | QIODevice::Truncate));
        old.write(QJsonDocument(QJsonArray{QString::fromLatin1(QByteArray("gone").toBase64())}).toJson());
        old.close();
        PeerStore older;
        QVERIFY(older.load(dir.path()));
        QVERIFY(older.isForgotten(QByteArray("gone")));
        QCOMPARE(older.forgotten().value(QByteArray("gone")), QString());
    }

    void repairingKeepsTheStoredLevelByDefault()
    {
        // B keeps A as a group member while A forgets B (B was off, so it never heard). When A
        // pairs again and nobody chooses a level on B, A stays what it was on B: the level is
        // never raised quietly, and a guest stays a guest the same way.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));

        auto pairAgain = [&] {
            const QByteArray idB = b->id();
            b.reset();
            QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);
            a->unpair(idB);
            QVERIFY(!a->isTrusted(idB));
            b = test::makeNode(dirB.path());
            QVERIFY(b->isTrusted(a->id()));
            b->setExploring(true);
            a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
            auto portOnA = [&] {
                for (const PeerView &p : a->peers()) {
                    if (p.id == idB)
                        return p.port;
                }
                return quint16(0);
            };
            QTRY_COMPARE(portOnA(), b->dataPort()); // B came back on another port
            QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
            QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
            QVERIFY(a->requestPairing(idB)); // no level chosen on either side
            QTRY_VERIFY(pinShown.size() && pinAsked.size());
            QVERIFY(a->submitPin(idB, pinShown.first().at(2).toString()));
            QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(idB) && b->isConnected(a->id()), 10000);
        };
        pairAgain();
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Group);
        QCOMPARE(a->trustLevel(b->id()), TrustLevel::Device); // new on A: the default

        b->setTrust(a->id(), TrustLevel::Guest);
        pairAgain();
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Guest);
    }

    void offlineMembersAreNotShownAsHavingChosenLess()
    {
        // What a peer declared is known only from its hello: until then the view reports the
        // level given here (a member that has not connected since the start never looks demoted),
        // while the group logic still counts it as Device.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto viewOnB = [&](const QByteArray &id) {
            for (const PeerView &p : b->peers()) {
                if (p.id == id)
                    return p.effectiveTrust;
            }
            return TrustLevel::Guest;
        };
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Device, TrustLevel::Group)); // A: "this device only"
        QTRY_COMPARE(viewOnB(a->id()), TrustLevel::Device);
        QCOMPARE(b->effectiveTrust(a->id()), TrustLevel::Device);

        a->setTrust(b->id(), TrustLevel::Group);
        QTRY_COMPARE(viewOnB(a->id()), TrustLevel::Group);
        const QByteArray idB = b->id();
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);
        b = test::makeNode(dirB.path());
        QCOMPARE(viewOnB(a->id()), TrustLevel::Group); // not heard from yet: what B gave it
        QCOMPARE(b->effectiveTrust(a->id()), TrustLevel::Device);
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(a->id()), 10000);
        QTRY_COMPARE(b->effectiveTrust(a->id()), TrustLevel::Group);
        QCOMPARE(viewOnB(a->id()), TrustLevel::Group);
    }

    void unpairingForgetsWhatThePeerDeclared()
    {
        // The level a peer declared goes with the pairing: after a Forget on either side and a
        // new pairing, it is unknown (Device) again until the new hello, so a stale Group never
        // lets A introduce B to others in the meantime.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(a->effectiveTrust(b->id()), TrustLevel::Group);
        std::optional<TrustLevel> atSuccess;
        connect(a.get(), &Node::pairingFinished, a.get(), [&](const QByteArray &id, bool ok, const QString &) {
            if (ok && id == b->id())
                atSuccess = a->effectiveTrust(id);
        });

        // B forgets A, then pairs again making A a guest on B; A keeps choosing Group.
        b->unpair(a->id());
        QTRY_VERIFY(!a->isTrusted(b->id()));
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Guest));
        QVERIFY(atSuccess.has_value());
        QCOMPARE(*atSuccess, TrustLevel::Device);
        QTRY_COMPARE(a->effectiveTrust(b->id()), TrustLevel::Guest);

        // A forgets B (B is told), then pairs again as Group on both sides.
        atSuccess.reset();
        a->unpair(b->id());
        QTRY_VERIFY(!b->isTrusted(a->id()));
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QVERIFY(atSuccess.has_value());
        QCOMPARE(*atSuccess, TrustLevel::Device);
        QTRY_COMPARE(a->effectiveTrust(b->id()), TrustLevel::Group);
    }

    void repeatingARequestKeepsTheChosenLevel()
    {
        // Pair pressed again while a request to the same device is pending (the prompt was
        // dismissed, the agent restarted): the first request is cancelled, the level chosen
        // with the second one is the one applied.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));
        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QVERIFY(a->requestPairing(b->id(), TrustLevel::Group));
        QTRY_COMPARE(pinShown.size(), 1);
        // The prompt was shown on A as well: B's PIN being out does not mean A already has the
        // challenge (on Linux one event-loop pass delivers one socket's datagrams), and only a
        // request that got its challenge is cancelled with the session id B knows.
        QTRY_COMPARE(pinAsked.size(), 1);
        QVERIFY(a->requestPairing(b->id(), TrustLevel::Group));
        QTRY_COMPARE(finishedA.size(), 1); // the first request, cancelled
        QVERIFY(!finishedA.first().at(1).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(pinShown.size(), 2, 10000);
        QTRY_COMPARE(pinAsked.size(), 2); // and A has the new challenge
        QVERIFY(a->submitPin(b->id(), pinShown.last().at(2).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(b->id()) && b->isConnected(a->id()), 10000);
        QCOMPARE(a->trustLevel(b->id()), TrustLevel::Group);
    }

    void theRequestersLevelIsSuggestedToTheResponder()
    {
        // "All my devices" chosen on A: B's PIN dialog offers This device only and points at the
        // suggestion (joining the group lets A introduce devices to all of B's, so it is B's
        // user's pick); picking it forms the group, and A meets B's other member C without a PIN.
        QTemporaryDir dirA, dirB, dirC;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto c = test::makeNode(dirC.path());
        QVERIFY(test::pairNodes(*b, *c, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(b->effectiveTrust(c->id()), TrustLevel::Group);

        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));
        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QVERIFY(a->requestPairing(b->id(), TrustLevel::Group));
        QTRY_VERIFY(pinShown.size() && pinAsked.size());
        QCOMPARE(b->pairingTrust(a->id()), TrustLevel::Device); // offered in the dialog
        QCOMPARE(b->pairingSuggestion(a->id()), std::optional<TrustLevel>(TrustLevel::Group)); // pointed at
        QVERIFY(b->setPairTrust(a->id(), TrustLevel::Group)); // B's user picks it
        QCOMPARE(b->pairingSuggestion(a->id()), std::nullopt);
        QVERIFY(a->submitPin(b->id(), pinShown.first().at(2).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(b->id()) && b->isConnected(a->id()), 10000);
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Group);
        QTRY_COMPARE(a->effectiveTrust(b->id()), TrustLevel::Group);
        QTRY_VERIFY_WITH_TIMEOUT(a->isTrusted(c->id()) && c->isTrusted(a->id()), 10000);
        QCOMPARE(b->pairingSuggestion(a->id()), std::nullopt); // the pairing is over

        // Left as offered, a Group suggestion makes the device This device only: it never joins
        // the group without B's user picking it, and D is told B gave it less than it chose.
        QTemporaryDir dirD;
        auto d = test::makeNode(dirD.path());
        QSignalSpy differsOnD(d.get(), &Node::pairingLevelDiffers);
        QVERIFY(test::pairNodes(*d, *b, TrustLevel::Group));
        QCOMPARE(b->trustLevel(d->id()), TrustLevel::Device);
        QTRY_COMPARE(differsOnD.size(), 1);
        QCOMPARE(differsOnD.first().at(2).value<TrustLevel>(), TrustLevel::Device);
        QTest::qWait(300);
        QVERIFY(!d->isTrusted(c->id()));

        // A guest suggestion is offered as it is (to a device B does not know).
        QTemporaryDir dirE;
        auto e = test::makeNode(dirE.path());
        QVERIFY(test::pairNodes(*e, *b, TrustLevel::Guest));
        QCOMPARE(b->trustLevel(e->id()), TrustLevel::Guest);
    }

    void aSuggestionNeverPromotesAGuestAndAShortfallIsReported()
    {
        // B made A a guest; A forgets B while B is off, then pairs again asking for Group. B's
        // dialog offers Guest (what A already is there), not A's wish; A learns from B's hello
        // that B gave it less than it chose, and says so (pairingLevelDiffers).
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Device, TrustLevel::Guest));
        const QByteArray idB = b->id();
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);
        a->unpair(idB);
        b = test::makeNode(dirB.path());
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Guest);
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        auto portOnA = [&] {
            for (const PeerView &p : a->peers()) {
                if (p.id == idB)
                    return p.port;
            }
            return quint16(0);
        };
        QTRY_COMPARE(portOnA(), b->dataPort());
        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy differsOnA(a.get(), &Node::pairingLevelDiffers);
        QSignalSpy differsOnB(b.get(), &Node::pairingLevelDiffers);
        QVERIFY(a->requestPairing(idB, TrustLevel::Group));
        QTRY_VERIFY(pinShown.size() && pinAsked.size());
        QCOMPARE(b->pairingTrust(a->id()), TrustLevel::Guest);
        QVERIFY(a->submitPin(idB, pinShown.first().at(2).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(idB) && b->isConnected(a->id()), 10000);
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Guest);
        QTRY_COMPARE(differsOnA.size(), 1);
        QCOMPARE(differsOnA.first().at(0).toByteArray(), idB);
        QCOMPARE(differsOnA.first().at(1).value<TrustLevel>(), TrustLevel::Group);
        QCOMPARE(differsOnA.first().at(2).value<TrustLevel>(), TrustLevel::Guest);
        QTest::qWait(300);
        QCOMPARE(differsOnB.size(), 0); // B chose Guest: anything A declares is as much
        QCOMPARE(differsOnA.size(), 1); // once per pairing, not on every hello
        a->setTrust(idB, TrustLevel::Device);
        QTest::qWait(300);
        QCOMPARE(differsOnB.size(), 0);
    }

    void aPairingChoiceIsTakenOnlyWhilePairing()
    {
        // A choice for a pairing that already ended (a late click in the PIN dialog, a stray
        // command) is refused, and the next pairing with that device starts from its default.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(!b->setPairTrust(a->id(), TrustLevel::Group));
        QVERIFY(test::pairNodes(*a, *b));
        QVERIFY(!b->setPairTrust(a->id(), TrustLevel::Guest));
        QCOMPARE(b->pairingTrust(a->id()), TrustLevel::Device);
        b->unpair(a->id());
        QTRY_VERIFY(!a->isTrusted(b->id()));
        QVERIFY(test::pairNodes(*a, *b));
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Device);
    }

    void anIntroductionOfAForgottenDeviceIsReportedAndCanBeAllowedAgain()
    {
        // A forgot X. B, a group member X also belongs to, lists X to A: A does not take it and
        // says so once (not on every hello of B). "Allow again" lets B's next list bring it in.
        QTemporaryDir dirA, dirB, dirX;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto x = test::makeNode(dirX.path());
        QVERIFY(test::pairNodes(*a, *x));
        const QString nameX = x->name();
        a->unpair(x->id());
        QTRY_VERIFY(!x->isTrusted(a->id()));
        QVERIFY(a->forgottenPeers().contains(x->id()));
        QCOMPARE(a->forgottenPeers().value(x->id()), nameX);
        QVERIFY(test::pairNodes(*b, *x, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(b->effectiveTrust(x->id()), TrustLevel::Group);

        QSignalSpy skipped(a.get(), &Node::forgottenIntroductionSkipped);
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(skipped.size(), 1);
        QCOMPARE(skipped.first().at(0).toByteArray(), b->id());
        QCOMPARE(skipped.first().at(1).toByteArray(), x->id());
        QCOMPARE(skipped.first().at(2).toString(), nameX);
        QVERIFY(!a->isTrusted(x->id()));
        a->setName(QStringLiteral("A renamed")); // a hello to B, which lists its members again
        QTest::qWait(500);
        QCOMPARE(skipped.size(), 1);

        a->unforget(x->id());
        QVERIFY(!a->forgottenPeers().contains(x->id()));
        QTRY_VERIFY_WITH_TIMEOUT(a->isTrusted(x->id()), 10000);
        QCOMPARE(a->trustLevel(x->id()), TrustLevel::Group);
    }

    void introductionsAreBoundedAndTakeOnlyUnicastHints()
    {
        // A member's list enrolls at most 64 devices per message, and an address hint that is
        // not one device's address (broadcast, multicast, wildcard, no port) is dropped, so that
        // a member cannot make this device send handshakes anywhere it likes.
        QTemporaryDir dirA, dirD;
        auto a = test::makeNode(dirA.path());
        auto d = test::makeNode(dirD.path());
        QVERIFY(test::pairNodes(*a, *d, TrustLevel::Group, TrustLevel::Group));
        QTRY_COMPARE(a->effectiveTrust(d->id()), TrustLevel::Group);

        auto entry = [](const QByteArray &key, const QString &address, int port) {
            QCborMap e;
            e.insert(QStringLiteral("k"), key);
            e.insert(QStringLiteral("n"), QStringLiteral("x"));
            if (!address.isEmpty()) {
                e.insert(QStringLiteral("a"), address);
                e.insert(QStringLiteral("p"), port);
            }
            return e;
        };
        auto members = [](const QCborArray &m) {
            QCborMap msg;
            msg.insert(QStringLiteral("t"), QStringLiteral("group.members"));
            msg.insert(QStringLiteral("m"), m);
            return msg;
        };
        auto addressOnA = [&](const QByteArray &id) {
            for (const PeerView &p : a->peers()) {
                if (p.id == id)
                    return p.address;
            }
            return QHostAddress(QStringLiteral("1.2.3.4"));
        };
        const QStringList bad{QStringLiteral("255.255.255.255"), QStringLiteral("224.0.0.1"), QStringLiteral("0.0.0.0"),
                              QStringLiteral("ff02::1"), QStringLiteral("::"), QStringLiteral("127.0.0.1")};
        QList<crypto::KeyPair> keys;
        QCborArray list;
        for (int i = 0; i < bad.size(); ++i) {
            keys.append(crypto::generateKeyPair());
            list.append(entry(keys.last().publicKey, bad.at(i), i == bad.size() - 1 ? 0 : 4242));
        }
        list.append(entry(keys.first().publicKey, QStringLiteral("255.255.255.255"), 4243)); // listed twice: once
        QVERIFY(d->sendMessage(a->id(), members(list)));
        QTRY_VERIFY(a->isTrusted(crypto::deviceIdFromPublicKey(keys.last().publicKey)));
        for (const crypto::KeyPair &k : keys) {
            const QByteArray id = crypto::deviceIdFromPublicKey(k.publicKey);
            QVERIFY(a->isTrusted(id));
            QVERIFY2(addressOnA(id).isNull(), qPrintable(addressOnA(id).toString()));
        }
        QCOMPARE(a->peers().size(), 1 + bad.size());

        QCborArray many;
        for (int i = 0; i < 70; ++i)
            many.append(entry(crypto::generateKeyPair().publicKey, QStringLiteral("127.0.0.1"), 4242));
        QVERIFY(d->sendMessage(a->id(), members(many)));
        QTRY_COMPARE(a->peers().size(), 1 + bad.size() + 64);
        QTest::qWait(300);
        QCOMPARE(a->peers().size(), 1 + bad.size() + 64);
    }

    void refusedPairingDropsTheChosenLevel()
    {
        // The level chosen for a pairing that the service refuses (nobody signed in) must not
        // apply to the next pairing with that device.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        b->setLocalUser(QString()); // users apart, nobody signed in
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));
        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);
        QVERIFY(!b->setPairTrust(a->id(), TrustLevel::Group)); // no pairing under way yet
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(pinShown.size(), 1);
        QVERIFY(b->setPairTrust(a->id(), TrustLevel::Group));
        QTRY_COMPARE(pinAsked.size(), 1); // A got the challenge and waits for the PIN
        QVERIFY(a->submitPin(b->id(), pinShown.first().at(2).toString()));
        QTRY_COMPARE(finishedB.size(), 1);
        QVERIFY(!finishedB.first().at(1).toBool());
        QVERIFY(!b->isTrusted(a->id()));
        QCOMPARE(b->pairingTrust(a->id()), TrustLevel::Device);

        a->unpair(b->id());
        b->setLocalUser(QStringLiteral("alice"));
        QVERIFY(test::pairNodes(*a, *b)); // nothing chosen this time
        QCOMPARE(b->trustLevel(a->id()), TrustLevel::Device);
    }

    void repairingIgnoresAnotherUsersLevel()
    {
        // Shared computer: alice paired D as a group member; while bob is signed in, D pairs
        // again and bob chooses nothing. D becomes bob's as Device: alice's choice is not his.
        QTemporaryDir dirA, dirD;
        auto a = test::makeNode(dirA.path());
        auto d = test::makeNode(dirD.path());
        a->setLocalUser(QStringLiteral("alice"));
        QVERIFY(test::pairNodes(*d, *a, TrustLevel::Device, TrustLevel::Group));
        QCOMPARE(a->trustLevel(d->id()), TrustLevel::Group);
        QCOMPARE(a->pairingTrust(d->id()), TrustLevel::Group);

        a->setLocalUser(QStringLiteral("bob"));
        QCOMPARE(a->pairingTrust(d->id()), TrustLevel::Device);
        d->unpair(a->id());
        QVERIFY(test::pairNodes(*d, *a));
        QCOMPARE(a->trustLevel(d->id()), TrustLevel::Device);
        bool otherUser = true;
        for (const PeerView &p : a->peers()) {
            if (p.id == d->id())
                otherUser = p.otherUser;
        }
        QVERIFY(!otherUser);
    }

    void wrongPinFails()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(pinAsked.size(), 1);
        const QString pin = pinShown.first().at(2).toString();
        const QString wrong = QStringLiteral("%1").arg((pin.toInt() + 1) % 1000000, 6, 10, QLatin1Char('0'));
        QVERIFY(a->submitPin(b->id(), wrong));

        QTRY_COMPARE(finishedA.size(), 1);
        QTRY_COMPARE(finishedB.size(), 1);
        QVERIFY(!finishedA.first().at(1).toBool());
        QVERIFY(!finishedB.first().at(1).toBool());
        QTest::qWait(500);
        QVERIFY(!a->isConnected(b->id()));
    }

    void repeatedWrongPinsBlockPairing()
    {
        // Each request gets a new PIN; after 3 failed attempts B refuses requests for 5 minutes,
        // even with the right PIN, and after the first failure its answers wait 300 ms.
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);
        for (int attempt = 1; attempt <= PairingManager::kMaxFailedAttempts; ++attempt) {
            QVERIFY(a->requestPairing(b->id()));
            QTRY_COMPARE(pinShown.size(), attempt);
            QTRY_COMPARE(pinAsked.size(), attempt); // A got the challenge and waits for the PIN
            const QString pin = pinShown.last().at(2).toString();
            const QString wrong = QStringLiteral("%1").arg((pin.toInt() + 1) % 1000000, 6, 10, QLatin1Char('0'));
            QElapsedTimer answer;
            answer.start();
            QVERIFY(a->submitPin(b->id(), wrong));
            QTRY_COMPARE(finishedA.size(), attempt);
            QTRY_COMPARE(finishedB.size(), attempt);
            if (attempt > 1)
                QVERIFY2(answer.elapsed() >= PairingManager::kRetryDelayMs, qPrintable(QString::number(answer.elapsed())));
        }
        QVERIFY(finishedB.last().at(2).toString().contains(QStringLiteral("blocked")));

        // Locked: no PIN is shown any more, and the request fails.
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(finishedA.size(), PairingManager::kMaxFailedAttempts + 1);
        QVERIFY(!finishedA.last().at(1).toBool());
        QVERIFY2(finishedA.last().at(2).toString().contains(QStringLiteral("Too many")),
                 qPrintable(finishedA.last().at(2).toString()));
        QCOMPARE(pinShown.size(), PairingManager::kMaxFailedAttempts);
        QVERIFY(!a->isConnected(b->id()));
    }

    void pairingRequiresExploration()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(false);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(finishedA.size(), 1);
        QVERIFY(!finishedA.first().at(1).toBool());
    }

    void pairingsBelongToTheUserWhoMadeThem()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        a->setLocalUser(QStringLiteral("alice"));
        QVERIFY(test::pairNodes(*a, *b));

        // Another user signs in on A: B is out of reach, and not theirs to see, use or remove.
        a->setLocalUser(QStringLiteral("bob"));
        QVERIFY(!a->isConnected(b->id()));
        QVERIFY(!a->sendMessage(b->id(), QCborMap{{QStringLiteral("t"), QStringLiteral("x")}}));
        QVERIFY(!a->openStream(b->id()));
        bool otherUser = false;
        for (const PeerView &p : a->peers())
            otherUser |= p.id == b->id() && p.otherUser;
        QVERIFY(otherUser);
        a->unpair(b->id());
        QVERIFY(a->isTrusted(b->id()));
        QTest::qWait(1500); // B keeps trying: A does not answer
        QVERIFY(!a->isConnected(b->id()));

        // Back to the user who paired them: they reconnect.
        a->setLocalUser(QStringLiteral("alice"));
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(b->id()) && b->isConnected(a->id()), 10000);

        // Nobody signed in (the sign-in screen): nothing either.
        a->setLocalUser(QString());
        QVERIFY(!a->isConnected(b->id()));

        // Pairings made before users were told apart go to the first user seen.
        QTemporaryDir dirC, dirD;
        auto c = makeNode(dirC.path());
        auto d = makeNode(dirD.path());
        QVERIFY(test::pairNodes(*c, *d));
        c->setLocalUser(QStringLiteral("carol"));
        QVERIFY(c->isConnected(d->id()));
        c->setLocalUser(QStringLiteral("dave"));
        QVERIFY(!c->isConnected(d->id()));
    }

    void beaconsCannotRenameOrFloodPeers()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        auto nameOf = [&](const QByteArray &id) {
            for (const PeerView &p : a->peers()) {
                if (p.id == id)
                    return p.name;
            }
            return QString();
        };
        const QString name = nameOf(b->id());
        QVERIFY(!name.isEmpty());

        auto beacon = [](const QByteArray &id, const QByteArray &key, const QString &name) {
            QCborMap m;
            m.insert(QStringLiteral("i"), id);
            m.insert(QStringLiteral("n"), name);
            m.insert(QStringLiteral("k"), key);
            m.insert(QStringLiteral("o"), QStringLiteral("windows"));
            return wire::cborPacket(wire::Type::Beacon, m);
        };
        QUdpSocket attacker;
        QVERIFY(attacker.bind(QHostAddress::LocalHost, 0));
        // An unauthenticated beacon with B's identity and another name.
        QByteArray bKey;
        {
            QFile f(QDir(dirB.path()).filePath(QStringLiteral("identity.json")));
            QVERIFY(f.open(QIODevice::ReadOnly));
            bKey = crypto::publicKeyFromSecret(QByteArray::fromBase64(
                QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("secretKey")).toString().toLatin1()));
        }
        QCOMPARE(crypto::deviceIdFromPublicKey(bKey), b->id());
        attacker.writeDatagram(beacon(b->id(), bKey, QStringLiteral("Evil")), QHostAddress::LocalHost, a->dataPort());
        QTest::qWait(300);
        QCOMPARE(nameOf(b->id()), name);

        // Hundreds of made-up devices: only a bounded number is kept.
        for (int i = 0; i < 300; ++i) {
            const crypto::KeyPair k = crypto::generateKeyPair();
            attacker.writeDatagram(beacon(crypto::deviceIdFromPublicKey(k.publicKey), k.publicKey,
                                          QStringLiteral("fake %1").arg(i)),
                                   QHostAddress::LocalHost, a->dataPort());
        }
        QTest::qWait(500);
        QVERIFY2(a->peers().size() <= 65, qPrintable(QString::number(a->peers().size())));
        QVERIFY(a->isConnected(b->id()));
    }

    void forgedBeaconsDoNotBlockReconnection()
    {
        // While B is away, a stranger keeps sending beacons with B's identity from its own
        // address. A must not follow them (every handshake would go to the forger) and must
        // answer B's own initiation when B comes back. The device with the smaller id used to
        // refuse a peer's initiation while its own was pending, so it plays the victim.
        QTemporaryDir dir1, dir2;
        auto first = test::makeNode(dir1.path());
        auto second = test::makeNode(dir2.path());
        const bool firstIsSmaller = first->id() < second->id();
        std::unique_ptr<Node> &a = firstIsSmaller ? first : second;
        std::unique_ptr<Node> &b = firstIsSmaller ? second : first;
        const QString dirB = firstIsSmaller ? dir2.path() : dir1.path();
        QVERIFY(test::pairNodes(*a, *b));
        const QByteArray idB = b->id();
        const QByteArray keyB = publicKeyIn(dirB);
        QCOMPARE(crypto::deviceIdFromPublicKey(keyB), idB);
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);

        QUdpSocket forger;
        QVERIFY(forger.bind(QHostAddress::LocalHost, 0));
        QTimer beacons;
        connect(&beacons, &QTimer::timeout, &forger, [&] {
            forger.writeDatagram(beaconFor(idB, keyB, QStringLiteral("B")), QHostAddress::LocalHost, a->dataPort());
        });
        beacons.start(400);
        QTest::qWait(1500); // A has been initiating to B's old address and to the forger's
        QVERIFY(!a->isConnected(idB));

        // B comes back on another port and initiates to A's stored address: no discovery here,
        // so only A answering that initiation can reconnect them.
        b = test::makeNode(dirB);
        QCOMPARE(b->id(), idB);
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(idB) && b->isConnected(a->id()), 10000);
        QCOMPARE(portOf(*a, idB), b->dataPort()); // where it was authenticated, not the forger
        QSignalSpy gotB(b.get(), &Node::messageReceived);
        QVERIFY(a->sendMessage(idB, QCborMap{{QStringLiteral("t"), QStringLiteral("x")}}));
        QTRY_COMPARE(gotB.size(), 1);
        QTest::qWait(500); // the beacons go on: the session stays where it is
        QCOMPARE(portOf(*a, idB), b->dataPort());
        QVERIFY(a->isConnected(idB));
    }

    void silentConnectionsDoNotStarveStreams()
    {
        // Sixteen TCP connections that never send a byte, from eight addresses: a paired
        // device's stream still gets through (the oldest pending one makes room; from one
        // address, the ninth silent connection evicts the oldest silent one there), and the
        // silent ones are closed within a couple of seconds rather than the full handshake time.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));

        std::vector<std::unique_ptr<QTcpSocket>> silent;
        for (int i = 0; i < 16; ++i) {
            auto s = std::make_unique<QTcpSocket>();
            if (!s->bind(QHostAddress(QStringLiteral("127.0.0.%1").arg(2 + i / 2)), 0))
                QSKIP("Loopback aliases 127.0.0.2-9 are not available here");
            s->connectToHost(QHostAddress::LocalHost, b->tcpPort());
            silent.push_back(std::move(s));
        }
        for (const auto &s : silent)
            QVERIFY(s->waitForConnected(3000));
        QTest::qWait(300); // B has accepted them all

        QSignalSpy accepted(b.get(), &Node::streamAccepted);
        std::unique_ptr<SecureStream> client(a->openStream(b->id()));
        QVERIFY(client);
        connect(client.get(), &SecureStream::established, client.get(), [&client] { client->send("hello"); });
        QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(silent.cbegin(), silent.cend(), [](const auto &s) {
                                     return s->state() == QAbstractSocket::UnconnectedState;
                                 }),
                                 4000);

        // A paired device's own streams, several at once from the one address, are not refused
        // for the per-address cap (it only counts connections that have said nothing, and it is
        // well above what one device opens together).
        std::vector<std::unique_ptr<SecureStream>> burst;
        for (int i = 0; i < 4; ++i) {
            burst.emplace_back(a->openStream(b->id()));
            QVERIFY(burst.back());
            SecureStream *s = burst.back().get();
            connect(s, &SecureStream::established, s, [s] { s->send("hello"); });
        }
        QTRY_COMPARE_WITH_TIMEOUT(accepted.size(), 5, 5000);
    }

    void replayedInitiationsFromEarlierBootsAreRefused()
    {
        // Someone who recorded A's handshake initiations replays them to B. Within a boot of A
        // only a counter above the last one taken is answered; a boot B has already seen stays
        // refused after A restarts (B remembers the last epochs), and the replays never spoil
        // what A and B are doing meanwhile.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        const QByteArray idA = a->id();
        const crypto::KeyPair keysA = keysIn(dirA.path());
        const QByteArray keyB = publicKeyIn(dirB.path());
        QUdpSocket replayer;
        QVERIFY(replayer.bind(QHostAddress::LocalHost, 0));
        auto initiation = [&](const QByteArray &bootId, qint64 counter) {
            noise::IKInitiator init(keysA, keyB, QByteArray("deskflower/1"));
            QCborMap payload;
            payload.insert(QStringLiteral("t"), counter);
            payload.insert(QStringLiteral("b"), bootId);
            QByteArray packet = wire::header(wire::Type::HandshakeInit);
            wire::appendU32(packet, 0x7e57);
            return packet + init.writeMessage1(QCborValue(payload).toCbor());
        };
        auto answered = [&](const QByteArray &packet) {
            replayer.writeDatagram(packet, QHostAddress::LocalHost, b->dataPort());
            bool response = false;
            return QTest::qWaitFor([&] {
                while (replayer.hasPendingDatagrams())
                    response |= wire::parseType(replayer.receiveDatagram().data()) == wire::Type::HandshakeResp;
                return response;
            }, 500);
        };
        const QByteArray recorded("boot-before-last");
        QVERIFY(answered(initiation(recorded, 1)));  // an epoch B has not seen: answered
        QVERIFY(!answered(initiation(recorded, 1))); // the same one again: not
        QVERIFY(answered(initiation(recorded, 2)));  // a later one of that boot: answered
        QVERIFY(!answered(initiation(recorded, 1))); // an earlier one: never again
        QVERIFY(b->isConnected(idA));

        // A restarts: its own new epoch is taken, the recorded one stays refused, and the pair
        // keeps talking while the replays go on.
        a.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!b->isConnected(idA), 10000);
        a = test::makeNode(dirA.path());
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(b->id()) && b->isConnected(idA), 10000);
        QSignalSpy gotB(b.get(), &Node::messageReceived);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!answered(initiation(recorded, 2)));
            QVERIFY(!answered(initiation(recorded, 1)));
        }
        QVERIFY(a->sendMessage(b->id(), QCborMap{{QStringLiteral("t"), QStringLiteral("x")}}));
        QTRY_COMPARE(gotB.size(), 1);
        QVERIFY(a->isConnected(b->id()) && b->isConnected(idA));
    }

    void abortsNeedTheSessionId()
    {
        // A PairAbort ends a pairing only with the secret that identifies it (the request's
        // nonce until the challenge, then the session id) and from the address the pairing
        // talks to: a stranger knowing the public ids cannot cancel it. The reason it carries
        // is shown in local words, or capped.
        QTemporaryDir dir;
        Identity me;
        QString error;
        QVERIFY2(me.loadOrCreate(dir.path(), &error), qPrintable(error));
        QList<QByteArray> sent;
        PairingManager pm(me, [&sent](const QByteArray &packet, const QHostAddress &, quint16) { sent.append(packet); });
        QSignalSpy failed(&pm, &PairingManager::failed);
        QSignalSpy pinAsked(&pm, &PairingManager::pinRequired);
        const crypto::KeyPair peer = crypto::generateKeyPair();
        const QByteArray peerId = crypto::deviceIdFromPublicKey(peer.publicKey);
        const QHostAddress addr(QStringLiteral("10.0.0.2"));
        const quint16 port = 4242;
        auto abort = [&](QCborMap fields, const QHostAddress &from = QHostAddress(QStringLiteral("10.0.0.2")),
                         quint16 fromPort = 4242) {
            fields.insert(QStringLiteral("i"), peerId);
            pm.handlePacket(wire::Type::PairAbort, fields, from, fromPort);
        };
        auto lastNonce = [&] { return wire::parseCborPacket(sent.last())->value(QStringLiteral("s")).toByteArray(); };

        QVERIFY(pm.start(peerId, addr, port));
        QCOMPARE(sent.size(), 1);
        QVERIFY(wire::parseType(sent.last()) == wire::Type::PairRequest);
        const QByteArray nonce = lastNonce();
        QCOMPARE(nonce.size(), 16);
        abort({{QStringLiteral("r"), QStringLiteral("go away")}}, QHostAddress(QStringLiteral("10.0.0.3")));
        abort({{QStringLiteral("r"), QStringLiteral("go away")}}, addr, 4243);
        abort({{QStringLiteral("s"), QByteArray(16, 'x')}, {QStringLiteral("r"), QStringLiteral("go away")}});
        abort({{QStringLiteral("s"), nonce}, {QStringLiteral("r"), QStringLiteral("go away")}},
              QHostAddress(QStringLiteral("10.0.0.3")));
        abort({{QStringLiteral("s"), nonce}, {QStringLiteral("r"), QStringLiteral("go away")}}, addr, 4243);
        QCOMPARE(failed.size(), 0);
        // The responder's refusal echoes the nonce: taken, and its text capped.
        abort({{QStringLiteral("s"), nonce}, {QStringLiteral("r"), QString(5000, 'x')}});
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.last().at(1).toString().size(), PairingManager::kMaxReasonLength);

        // A responder from before 0.1.21 refuses without the nonce: taken while the request is
        // unanswered, from the address it went to, naming that device (so the user reads why).
        QVERIFY(pm.start(peerId, addr, port));
        abort({{QStringLiteral("r"), QStringLiteral("not in discovery mode")}});
        QCOMPARE(failed.size(), 2);
        QCOMPARE(failed.last().at(1).toString(), QString("not in discovery mode"));

        // After the challenge the session id is the secret, and the nonce no longer counts; nor
        // does an abort without it, from anywhere.
        QVERIFY(pm.start(peerId, addr, port));
        const QByteArray nonce2 = lastNonce();
        const QByteArray sid = crypto::randomBytes(16);
        QCborMap challenge;
        challenge.insert(QStringLiteral("i"), peerId);
        challenge.insert(QStringLiteral("n"), QStringLiteral("Peer"));
        challenge.insert(QStringLiteral("k"), peer.publicKey);
        challenge.insert(QStringLiteral("o"), QStringLiteral("linux"));
        challenge.insert(QStringLiteral("s"), sid);
        pm.handlePacket(wire::Type::PairChallenge, challenge, addr, port);
        QCOMPARE(pinAsked.size(), 1);
        abort({{QStringLiteral("r"), QStringLiteral("x")}});
        abort({{QStringLiteral("s"), nonce2}, {QStringLiteral("r"), QStringLiteral("x")}});
        abort({{QStringLiteral("s"), QByteArray(16, 'y')}, {QStringLiteral("r"), QStringLiteral("x")}});
        abort({{QStringLiteral("s"), sid}, {QStringLiteral("e"), QStringLiteral("busy")}},
              QHostAddress(QStringLiteral("10.0.0.3")));
        QCOMPARE(failed.size(), 2);
        abort({{QStringLiteral("s"), sid}, {QStringLiteral("e"), QStringLiteral("busy")}});
        QCOMPARE(failed.size(), 3);
        QCOMPARE(failed.last().at(1).toString(), PairingManager::reasonText(QStringLiteral("busy"), {}, 0));
        QVERIFY(failed.last().at(1).toString().contains(QStringLiteral("another device")));
        QVERIFY(PairingManager::reasonText(QStringLiteral("locked"), {}, 3).contains(QStringLiteral("3 minute")));
    }

    void strangersGetNeitherTheChallengeNorThePin()
    {
        // B shows a PIN for A. A stranger who repeats A's public ids from another address must
        // not get the challenge (it carries the session id, with which it could end the pairing
        // or spend the PIN), and a session id it got some other way is no use from there either:
        // only the socket the request came from may answer the challenge.
        QTemporaryDir dir;
        Identity me;
        QString error;
        QVERIFY2(me.loadOrCreate(dir.path(), &error), qPrintable(error));
        QList<QByteArray> sent;
        QList<QHostAddress> sentTo;
        PairingManager pm(me, [&](const QByteArray &packet, const QHostAddress &addr, quint16) {
            sent.append(packet);
            sentTo.append(addr);
        });
        pm.setAcceptingRequests(true);
        QSignalSpy pinShown(&pm, &PairingManager::pinGenerated);
        QSignalSpy failed(&pm, &PairingManager::failed);
        QSignalSpy succeeded(&pm, &PairingManager::succeeded);
        const crypto::KeyPair peer = crypto::generateKeyPair();
        const QByteArray peerId = crypto::deviceIdFromPublicKey(peer.publicKey);
        const QHostAddress addr(QStringLiteral("10.0.0.2"));
        const QHostAddress stranger(QStringLiteral("10.0.0.3"));
        const quint16 port = 4242;
        auto request = [&](const QHostAddress &from, quint16 fromPort) {
            QCborMap m;
            m.insert(QStringLiteral("i"), peerId);
            m.insert(QStringLiteral("n"), QStringLiteral("A"));
            m.insert(QStringLiteral("k"), peer.publicKey);
            m.insert(QStringLiteral("o"), QStringLiteral("linux"));
            m.insert(QStringLiteral("s"), crypto::randomBytes(16));
            pm.handlePacket(wire::Type::PairRequest, m, from, fromPort);
        };
        auto lastType = [&] { return wire::parseType(sent.last()); };

        request(addr, port);
        QCOMPARE(pinShown.size(), 1);
        QCOMPARE(sent.size(), 1);
        QVERIFY(lastType() == wire::Type::PairChallenge);
        const QByteArray sid = wire::parseCborPacket(sent.last())->value(QStringLiteral("s")).toByteArray();
        QCOMPARE(sid.size(), 16);
        const QString pin = pinShown.first().at(2).toString();

        // The requester repeating itself gets the challenge again; anyone else a refusal.
        request(addr, port);
        QCOMPARE(sent.size(), 2);
        QVERIFY(lastType() == wire::Type::PairChallenge);
        request(stranger, port);
        request(addr, 4243);
        QCOMPARE(sent.size(), 4);
        QVERIFY(lastType() == wire::Type::PairAbort);
        QVERIFY(wire::parseType(sent.at(2)) == wire::Type::PairAbort);
        QCOMPARE(sentTo.at(2), stranger);
        QCOMPARE(pinShown.size(), 1);

        // With the session id, from elsewhere: ignored, the PIN still stands for the requester.
        const QByteArray channel = peerId + peer.publicKey + me.id() + me.keys().publicKey;
        CPace cpace(pin, sid, channel);
        QVERIFY(cpace.isValid());
        auto msg1 = [&](const QByteArray &share, const QHostAddress &from, quint16 fromPort) {
            QCborMap m;
            m.insert(QStringLiteral("i"), peerId);
            m.insert(QStringLiteral("s"), sid);
            m.insert(QStringLiteral("y"), share);
            pm.handlePacket(wire::Type::PairMsg1, m, from, fromPort);
        };
        msg1(QByteArray(32, 'x'), stranger, port); // garbage that would fail the pairing
        msg1(cpace.share(), stranger, port);       // the real share, from the wrong socket
        msg1(cpace.share(), addr, 4243);
        QCOMPARE(sent.size(), 4);
        QCOMPARE(failed.size(), 0);

        // The requester itself completes the pairing.
        msg1(cpace.share(), addr, port);
        QCOMPARE(sent.size(), 5);
        QVERIFY(lastType() == wire::Type::PairMsg2);
        const QCborMap msg2 = *wire::parseCborPacket(sent.last());
        const auto isk = cpace.finish(msg2.value(QStringLiteral("y")).toByteArray(), true);
        QVERIFY(isk);
        QVERIFY(crypto::constantTimeEqual(msg2.value(QStringLiteral("c")).toByteArray(),
                                          CPace::confirmationTag(*isk, 'B')));
        QCborMap m3;
        m3.insert(QStringLiteral("i"), peerId);
        m3.insert(QStringLiteral("s"), sid);
        m3.insert(QStringLiteral("c"), CPace::confirmationTag(*isk, 'A'));
        pm.handlePacket(wire::Type::PairMsg3, m3, stranger, port); // the confirmation, from elsewhere
        QCOMPARE(succeeded.size(), 0);
        pm.handlePacket(wire::Type::PairMsg3, m3, addr, port);
        QCOMPARE(succeeded.size(), 1);
        QVERIFY(lastType() == wire::Type::PairDone);
        QCOMPARE(failed.size(), 0);
    }

    void unansweredChallengesDoNotCountTowardTheLockout()
    {
        // A stranger could request a PIN and walk away three times to lock the device out of
        // pairing: a challenge that gets no guess (timed out, or given up before the PIN) is no
        // attempt at the PIN, so it does not count.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        Node::Options o;
        o.configDir = dirB.path();
        o.dataPort = 0;
        o.enableDiscovery = false;
        o.pinTimeoutMs = 800;
        auto b = std::make_unique<Node>(o);
        QString error;
        QVERIFY2(b->start(&error), qPrintable(error));
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));
        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);
        int n = 0;
        for (int i = 0; i < PairingManager::kMaxFailedAttempts; ++i) { // nobody types the PIN
            ++n;
            QVERIFY(a->requestPairing(b->id()));
            QTRY_COMPARE(pinShown.size(), n);
            QTRY_COMPARE_WITH_TIMEOUT(finishedB.size(), n, 5000);
            QTRY_COMPARE(finishedA.size(), n); // B's abort, with the session id, reached A
            QVERIFY2(finishedA.last().at(2).toString().contains(QStringLiteral("Timed out")),
                     qPrintable(finishedA.last().at(2).toString()));
        }
        for (int i = 0; i < PairingManager::kMaxFailedAttempts; ++i) { // A gives up before typing
            ++n;
            QVERIFY(a->requestPairing(b->id()));
            QTRY_COMPARE(pinShown.size(), n);
            a->cancelPairing(b->id());
            QTRY_COMPARE(finishedB.size(), n);
            QCOMPARE(finishedA.size(), n);
        }
        QVERIFY2(!finishedB.last().at(2).toString().contains(QStringLiteral("blocked")),
                 qPrintable(finishedB.last().at(2).toString()));
        QVERIFY(test::pairNodes(*a, *b)); // not locked
    }

    // --- Routers and NAT (natproxy.h) --------------------------------------------------------
    // A sits behind a router that translates its address (127.0.0.11 inside, 127.0.0.20 public);
    // B is outside (127.0.0.12) and can reach neither A's own address nor, over TCP, the router.

    void connectsThroughNatAndKeepsEveryEndpoint()
    {
        NatWorld w;
        w.startA();
        w.startB();
        QVERIFY(test::pairNodes(*w.a, *w.b, std::nullopt, std::nullopt, w.addrB));

        // B sees A at the router, and knows it: A's hello lists its own address.
        QTRY_VERIFY(test::viewOf(*w.b, w.a->id())->viaNat);
        QCOMPARE(test::viewOf(*w.b, w.a->id())->address, w.nat.publicAddress());
        QVERIFY(!test::viewOf(*w.a, w.b->id())->viaNat);
        QCOMPARE(test::viewOf(*w.a, w.b->id())->address, w.addrB);

        QSignalSpy got(w.b.get(), &Node::messageReceived);
        QCborMap msg;
        msg.insert(QStringLiteral("t"), QStringLiteral("test"));
        QVERIFY(w.a->sendMessage(w.b->id(), msg));
        QTRY_COMPARE(got.size(), 1);

        // peers.json: B keeps A's own address beside the router's mapping (which does not replace
        // it), and A keeps the address its user typed for B as configured.
        QTRY_VERIFY(storedEndpoint(w.dirB.path(), w.a->id(), w.addrA, QStringLiteral("self")));
        QTRY_VERIFY(storedEndpoint(w.dirB.path(), w.a->id(), w.nat.publicAddress(), QStringLiteral("observed")));
        QTRY_VERIFY(storedEndpoint(w.dirA.path(), w.b->id(), w.addrB, QStringLiteral("configured")));
    }

    void pairingWorksFromOutsideTheNat()
    {
        // B, outside, pairs with A behind the router: the pairing talks to the router's mapping
        // throughout (it pins the address and port the request went to).
        NatWorld w;
        w.startA();
        w.startB();
        w.a->setExploring(true);
        w.a->addManualPeer(w.addrB, w.b->dataPort()); // what lets B see A (as discovery would)
        QTRY_VERIFY(test::knows(*w.b, w.a->id()));
        QSignalSpy pinShown(w.a.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(w.b.get(), &Node::pairingPinRequired);
        QVERIFY(w.b->requestPairing(w.a->id()));
        QTRY_VERIFY(pinShown.size() == 1 && pinAsked.size() == 1);
        QVERIFY(w.b->submitPin(w.a->id(), pinShown.first().at(2).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(test::connectedWithHello(*w.a, w.b->id()) && test::connectedWithHello(*w.b, w.a->id()),
                                 10000);
    }

    void outsideDeviceReachesNatDeviceByDialBack()
    {
        NatWorld w;
        w.startA();
        w.startB();
        QVERIFY(test::pairNodes(*w.a, *w.b, std::nullopt, std::nullopt, w.addrB));
        QTRY_VERIFY(test::viewOf(*w.b, w.a->id())->viaNat);

        // A stream from B: no TCP connection reaches A, so A connects to B instead (well before
        // the tunnel would start).
        QSignalSpy accepted(w.a.get(), &Node::streamAccepted);
        QElapsedTimer clock;
        clock.start();
        std::unique_ptr<SecureStream> stream(w.b->openStream(w.a->id()));
        QVERIFY(stream);
        QSignalSpy established(stream.get(), &SecureStream::established);
        QTRY_COMPARE(established.size(), 1);
        QVERIFY2(clock.elapsed() < 4000, "took the tunnel, not the dial-back");
        QVERIFY(stream->send("ping"));
        QTRY_COMPARE(accepted.size(), 1);
        auto *served = accepted.first().at(0).value<SecureStream *>();
        QCOMPARE(served->peerId(), w.b->id());
        QSignalSpy received(served, &SecureStream::received);
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().at(0).toByteArray(), QByteArray("ping"));

        // Given up before any path won: the dial-back that comes later is refused, nothing leaks.
        SecureStream *abandoned = w.b->openStream(w.a->id());
        QCoreApplication::processEvents(); // its first step: the dial-back request goes out
        delete abandoned;
        QTest::qWait(1000);

        // And what streams are for: B fetches a file A offers.
        QVERIFY(fetchWorks(*w.a, *w.b, randomData(300 * 1024), 15000));
    }

    void tunnelCarriesStreamsWhenNoTcpGetsThrough()
    {
        // TCP blocked both ways (two routers, or firewalls that only let UDP through): the stream
        // goes through the UDP session, both directions, and the session stays responsive.
        NatWorld w;
        w.startA([](Node::Options &o) { o.connectHook = [](const QHostAddress &, quint16) { return false; }; });
        w.startB();
        QVERIFY(test::pairNodes(*w.a, *w.b, std::nullopt, std::nullopt, w.addrB));

        // Control messages sent all along the transfer arrive promptly (their own channel).
        QElapsedTimer clock;
        clock.start();
        qint64 worst = 0;
        int answered = 0;
        connect(w.b.get(), &Node::messageReceived, this, [&](const QByteArray &, const QCborMap &msg) {
            worst = std::max(worst, clock.elapsed() - msg.value(QStringLiteral("s")).toInteger());
            ++answered;
        });
        QTimer chat;
        chat.setInterval(200);
        connect(&chat, &QTimer::timeout, this, [&] {
            QCborMap msg;
            msg.insert(QStringLiteral("t"), QStringLiteral("test"));
            msg.insert(QStringLiteral("s"), clock.elapsed());
            w.a->sendMessage(w.b->id(), msg);
        });
        chat.start();
        const bool fetched = fetchWorks(*w.a, *w.b, randomData(2 * 1024 * 1024), 30000);
        chat.stop();
        QVERIFY(fetched);
        QVERIFY(answered > 10);
        QVERIFY2(worst < 1000, qPrintable(QString::number(worst)));
        QVERIFY(fetchWorks(*w.b, *w.a, randomData(100 * 1024), 30000)); // the other way too
    }

    void hairpinSessionMovesToTheDirectAddress()
    {
        // A and C share a LAN behind one router and met through its public address (the router
        // sends their packets back in: hairpin NAT). Streams to the router's address would go
        // nowhere: the session moves to their own addresses.
        QTemporaryDir dirC;
        NatWorld w(test::NatProxy::Filtering::FullCone);
        const QHostAddress addrC(QStringLiteral("127.0.0.13"));
        w.lanA = {w.addrA, addrC};
        w.startA();
        w.startB();
        QVERIFY(test::pairNodes(*w.a, *w.b, std::nullopt, std::nullopt, w.addrB)); // A gets a mapping
        const quint16 mappedA = w.nat.publicPort(w.slotA);
        QVERIFY(mappedA);

        auto slotC = std::make_shared<Node *>(nullptr);
        auto c = test::makeNodeAt(dirC.path(), addrC.toString(),
                                  [&](Node::Options &o) { o.datagramHook = w.nat.insideHook(slotC, w.lanA); });
        *slotC = c.get();
        QVERIFY(test::pairNodes(*c, *w.a, std::nullopt, std::nullopt, w.nat.publicAddress(), mappedA));
        QTRY_VERIFY_WITH_TIMEOUT(test::viewOf(*c, w.a->id())->address == w.addrA
                                     && test::viewOf(*w.a, c->id())->address == addrC,
                                 15000);
        QVERIFY(!test::viewOf(*c, w.a->id())->viaNat);
        QVERIFY(test::connectedWithHello(*c, w.a->id()));
        *slotC = nullptr;
    }

    void reconnectsAfterRestartsAndNewMappings()
    {
        NatWorld w;
        w.startA();
        w.startB();
        QVERIFY(test::pairNodes(*w.a, *w.b, std::nullopt, std::nullopt, w.addrB));
        const quint16 portB = w.b->dataPort();
        const QByteArray idA = w.a->id(), idB = w.b->id();

        // B restarts while the router forgets the mapping: B only knows dead endpoints for A
        // (and A's own address, which it cannot reach); A finds B again.
        w.b.reset();
        w.nat.newMappings();
        w.startB(portB);
        QTRY_VERIFY_WITH_TIMEOUT(w.a->isConnected(idB) && w.b->isConnected(idA), 15000);

        // A long outage: both sessions time out and the router maps A elsewhere afterwards.
        w.nat.setBlackout(true);
        QTRY_VERIFY_WITH_TIMEOUT(!w.a->isConnected(idB) && !w.b->isConnected(idA), 15000);
        w.nat.newMappings();
        w.nat.setBlackout(false);
        QTRY_VERIFY_WITH_TIMEOUT(w.a->isConnected(idB) && w.b->isConnected(idA), 15000);

        // A restarts: same story from the other side.
        w.a.reset();
        w.startA();
        QTRY_VERIFY_WITH_TIMEOUT(w.a->isConnected(idB) && w.b->isConnected(idA), 15000);
        QTRY_VERIFY(test::connectedWithHello(*w.b, idA));
        QVERIFY(fetchWorks(*w.a, *w.b, randomData(64 * 1024), 15000)); // dial-back still works
    }

    void groupHintsCarryEveryEndpoint()
    {
        // B introduces A (behind the router) to C (outside, 127.0.0.14) with every endpoint it
        // knows for A; A, told about C, reaches it through the router.
        QTemporaryDir dirC;
        NatWorld w;
        w.startA();
        w.startB();
        const QHostAddress addrC(QStringLiteral("127.0.0.14"));
        auto c = test::makeNodeAt(dirC.path(), addrC.toString(), [&](Node::Options &o) {
            test::outsideHooks(o, {w.addrA}, w.nat.publicAddress());
        });
        QVERIFY(test::pairNodes(*w.a, *w.b, TrustLevel::Group, TrustLevel::Group, w.addrB));
        QTRY_VERIFY(test::viewOf(*w.b, w.a->id())->viaNat); // B has A's hello
        QVERIFY(test::pairNodes(*c, *w.b, TrustLevel::Group, TrustLevel::Group, w.addrB));
        QTRY_VERIFY(storedEndpoint(dirC.path(), w.a->id(), w.addrA, QStringLiteral("group")));
        QTRY_VERIFY(storedEndpoint(dirC.path(), w.a->id(), w.nat.publicAddress(), QStringLiteral("group")));
        QTRY_VERIFY_WITH_TIMEOUT(c->isConnected(w.a->id()) && w.a->isConnected(c->id()), 20000);
    }

    void hairpinMoveSurvivesALostProbe()
    {
        // As hairpinSessionMovesToTheDirectAddress, but the first handshake each side sends to
        // the other's own address is lost: the probe is tried again 5 s later (not only at the
        // next rekey, 2 minutes on).
        QTemporaryDir dirC;
        NatWorld w(test::NatProxy::Filtering::FullCone);
        const QHostAddress addrC(QStringLiteral("127.0.0.13"));
        w.lanA = {w.addrA, addrC};
        const auto losingFirstProbe = [](auto hook, QHostAddress to) {
            auto lost = std::make_shared<bool>(false);
            return [hook, to, lost](const QByteArray &datagram, const QHostAddress &addr, quint16 port) {
                if (addr == to && !*lost && wire::parseType(datagram) == wire::Type::HandshakeInit) {
                    *lost = true;
                    return true; // dropped
                }
                return hook(datagram, addr, port);
            };
        };
        w.startA([&](Node::Options &o) { o.datagramHook = losingFirstProbe(o.datagramHook, addrC); });
        w.startB();
        QVERIFY(test::pairNodes(*w.a, *w.b, std::nullopt, std::nullopt, w.addrB));
        const quint16 mappedA = w.nat.publicPort(w.slotA);
        QVERIFY(mappedA);

        auto slotC = std::make_shared<Node *>(nullptr);
        auto c = test::makeNodeAt(dirC.path(), addrC.toString(), [&](Node::Options &o) {
            o.datagramHook = losingFirstProbe(w.nat.insideHook(slotC, w.lanA), w.addrA);
        });
        *slotC = c.get();
        QVERIFY(test::pairNodes(*c, *w.a, std::nullopt, std::nullopt, w.nat.publicAddress(), mappedA));
        QTRY_VERIFY_WITH_TIMEOUT(test::viewOf(*c, w.a->id())->address == w.addrA
                                     && test::viewOf(*w.a, c->id())->address == addrC,
                                 20000);
        *slotC = nullptr;
    }

    void tunnelsWorkAfterOneSideAloneLostTheSession()
    {
        // No TCP at all (streams go through the tunnel). B stops hearing A for long enough that
        // its session times out while A's does not; they reconnect (same boots). The tunnel
        // channel must not start over on one side only: later tunnels still work.
        QTemporaryDir dirA, dirB;
        auto muteA = std::make_shared<bool>(false);
        const auto noTcp = [](const QHostAddress &, quint16) { return false; };
        auto a = test::makeNodeAt(dirA.path(), QStringLiteral("127.0.0.31"), [&](Node::Options &o) {
            o.connectHook = noTcp;
            o.datagramHook = [muteA](const QByteArray &, const QHostAddress &, quint16) { return *muteA; };
        });
        auto b = test::makeNodeAt(dirB.path(), QStringLiteral("127.0.0.32"),
                                  [&](Node::Options &o) { o.connectHook = noTcp; });
        QVERIFY(test::pairNodes(*a, *b, std::nullopt, std::nullopt, QHostAddress(QStringLiteral("127.0.0.32"))));
        QTRY_VERIFY(test::connectedWithHello(*a, b->id()) && test::connectedWithHello(*b, a->id()));
        QVERIFY(fetchWorks(*a, *b, randomData(200 * 1024), 30000));

        QSignalSpy aLost(a.get(), &Node::peerDisconnected);
        *muteA = true;
        QTRY_VERIFY_WITH_TIMEOUT(!b->isConnected(a->id()), 15000);
        *muteA = false;
        QTRY_VERIFY_WITH_TIMEOUT(b->isConnected(a->id()) && a->isConnected(b->id()), 15000);
        QVERIFY2(aLost.isEmpty(), "both sides lost the session: the test proves nothing");

        QVERIFY(fetchWorks(*a, *b, randomData(200 * 1024), 30000));
        QVERIFY(fetchWorks(*b, *a, randomData(100 * 1024), 30000));
    }

    void secureStreamReadPause()
    {
        // Over TCP: frames held while the receiver is paused arrive in order on resume, and the
        // sender's close while paused does not lose the frames before it.
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        checkReadPause(*a, *b, 1200, true, 10000); // 72 MB: more than any loopback buffers
        if (QTest::currentTestFailed())
            return;
        checkReadPause(*b, *a, 1200, true, 10000); // the other way round
    }

    void secureStreamReadPauseOverTunnel()
    {
        // The same through the UDP session (no TCP at all), and a stream paused for too long
        // fails once its tunnel holds more than kMaxPausedBacklog, rather than growing.
        QTemporaryDir dirA, dirB;
        const auto noTcp = [](const QHostAddress &, quint16) { return false; };
        auto a = test::makeNodeAt(dirA.path(), QStringLiteral("127.0.0.41"),
                                  [&](Node::Options &o) { o.connectHook = noTcp; });
        auto b = test::makeNodeAt(dirB.path(), QStringLiteral("127.0.0.42"),
                                  [&](Node::Options &o) { o.connectHook = noTcp; });
        QVERIFY(test::pairNodes(*a, *b, std::nullopt, std::nullopt, QHostAddress(QStringLiteral("127.0.0.42"))));
        QTRY_VERIFY(test::connectedWithHello(*a, b->id()) && test::connectedWithHello(*b, a->id()));
        checkReadPause(*a, *b, 20, false, 20000);
        if (QTest::currentTestFailed())
            return;

        constexpr int kSize = SecureStream::kMaxFrame;
        PausedStream s;
        QVERIFY(s.open(*a, *b, 20000));
        const int frames = int(SecureStream::kMaxPausedBacklog / kSize) + 64;
        pumpFrames(s.client.get(), 0, frames, kSize, false, &s.guard);
        QTRY_VERIFY_WITH_TIMEOUT(s.closedReason.has_value(), 120000);
        QCOMPARE(*s.closedReason, QStringLiteral("The receiving disk is too slow for this connection"));
        QCOMPARE(s.got.size(), 0);
    }

    void takenOverStreamSeesAnEarlierEnd()
    {
        // The attempt reads the handshake reply only once its transport has ended (the reply and
        // the end came together). It establishes, the proxy takes it over in that slot, and the
        // transport will not report its end again: the proxy still closes, at once.
        const crypto::KeyPair keysA = crypto::generateKeyPair();
        const crypto::KeyPair keysB = crypto::generateKeyPair();
        auto *tA = new PipeTransport;
        auto *tB = new PipeTransport;
        tA->peer = tB;
        tB->peer = tA;
        std::unique_ptr<SecureStream> proxy(SecureStream::pending(keysA, "B", 0));
        std::unique_ptr<SecureStream> attempt(
            SecureStream::connectOver(tA, keysA, keysB.publicKey, "B", QStringLiteral("pipe")));
        std::unique_ptr<SecureStream> responder(SecureStream::acceptOver(
            tB, keysB, [](const QByteArray &) { return std::optional<QByteArray>("A"); }));
        bool tookOver = false;
        connect(attempt.get(), &SecureStream::established, proxy.get(),
                [&] { tookOver = proxy->takeOver(attempt.get()); });
        QSignalSpy proxyClosed(proxy.get(), &SecureStream::closed);

        emit tB->readyRead(); // message 1 -> message 2, waiting in tA
        QVERIFY(!tA->in.isEmpty());
        tA->ended = true;
        emit tA->disconnected();
        QVERIFY(tookOver);
        QVERIFY(proxy->isEstablished());
        QTRY_COMPARE_WITH_TIMEOUT(proxyClosed.size(), 1, 2000);
        QCOMPARE(proxyClosed.at(0).at(0).toString(), QStringLiteral("Connection closed"));
    }

    void dialBacksNobodyAskedForAreRefused()
    {
        // A TCP stream from a paired device that presents a dial-back nonce this device never
        // sent (or one already used) is closed, and never served as a stream.
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_VERIFY(test::connectedWithHello(*a, b->id()));
        QSignalSpy accepted(a.get(), &Node::streamAccepted);
        const QByteArray magic = QByteArrayLiteral("\xff" "DFDIAL1");
        std::unique_ptr<SecureStream> forged(SecureStream::connectTo(keysIn(dirB.path()), publicKeyIn(dirA.path()),
                                                                     a->id(), QHostAddress::LocalHost, a->tcpPort()));
        connect(forged.get(), &SecureStream::established, this,
                [&] { forged->send(magic + crypto::randomBytes(16)); });
        QSignalSpy closed(forged.get(), &SecureStream::closed);
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
        QCOMPARE(accepted.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestNode)
#include "tst_node.moc"
