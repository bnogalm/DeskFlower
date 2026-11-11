// ServiceController over its local IPC (a pipe / socket of a test instance of its own, no network
// listener besides the loopback node), with an agent and a session helper as plain clients:
//  - the Control/Command swap chosen for a device is kept in the settings and applied again by the
//    next service;
//  - what the helper reports keeps it from working (inputStatus) reaches the agents, with a link
//    only to the system's own settings, and goes away with the helper;
//  - a notice from the helper reaches the agents;
//  - local paste gestures (localPress, localPressSupport) are taken from the helper only;
//  - copied files are transferred (clipFetch) only for a paste: without a press, clipFetched says
//    noPaste and nothing moves;
//  - a copy too large to list in its announcement is listed (clipList) in chunks, once per
//    clipboard; a streaming paste (clipFetch {stream}) is told the transfer's folder (clipStarted)
//    before the end, belongs to its connection (closing it cancels the transfer) and is answered
//    at once after the transfer; Receive (clipReceive) takes the files with no paste;
//  - the agents see a transfer's folder and whether it can be cancelled, a cancelled transfer
//    ends as such, and a refused paste is offered to them once per clipboard (clipWaiting) until
//    the files arrive, the clipboard changes, the user dismisses it or the console user changes.

#include "servicecontroller.h"
#include "crypto.h"
#include "testutil.h"
#include "wire.h"

#include <QCborArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {

class NoInstaller : public PackageInstaller
{
public:
    bool isSupported() const override { return false; }
    bool launch(const QString &, std::function<void(bool, const QString &)>, QString *error) override
    {
        *error = QStringLiteral("not in tests");
        return false;
    }
};

const QByteArray kToken = "tst-session-token";

// An agent or the session helper: what it was sent, in order.
struct Client {
    QLocalSocket socket;
    std::unique_ptr<ipc::Connection> conn;
    QList<QCborMap> received;

    // render: the server pastes are rendered through (ipc::renderServerName).
    bool connectTo(const QString &instance, bool render = false)
    {
        socket.connectToServer(render ? ipc::renderServerName(instance) : ipc::serverName(instance));
        if (!socket.waitForConnected(5000))
            return false;
        conn = std::make_unique<ipc::Connection>(&socket);
        QObject::connect(conn.get(), &ipc::Connection::messageReceived,
                         [this](const QCborMap &m) { received.append(m); });
        return true;
    }
    void send(const QString &cmd, QCborMap args = {})
    {
        args.insert(QStringLiteral("cmd"), cmd);
        conn->send(args);
    }
    // The last event of that name (empty map: none yet).
    QCborMap last(const QString &ev) const
    {
        for (auto it = received.crbegin(); it != received.crend(); ++it) {
            if (it->value(QStringLiteral("ev")).toString() == ev)
                return *it;
        }
        return {};
    }
    // Every event of that name, in order.
    QList<QCborMap> all(const QString &ev) const
    {
        QList<QCborMap> out;
        for (const QCborMap &m : received) {
            if (m.value(QStringLiteral("ev")).toString() == ev)
                out.append(m);
        }
        return out;
    }
    // The position of the first event of that name (-1: none).
    qsizetype indexOf(const QString &ev) const
    {
        for (qsizetype i = 0; i < received.size(); ++i) {
            if (received.at(i).value(QStringLiteral("ev")).toString() == ev)
                return i;
        }
        return -1;
    }
};

// Another device, paired with the service's node: it copies files and publishes them.
struct Peer {
    QTemporaryDir dir;
    std::unique_ptr<Node> node;
    std::unique_ptr<FileTransfer> transfer;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<ClipboardSync> clip;

    explicit Peer(Node &service)
    {
        node = test::makeNode(dir.path());
        transfer = std::make_unique<FileTransfer>(node.get());
        settings = std::make_unique<Settings>(dir.path());
        ClipboardSync::Presence none{[] { return QByteArray(); },
                                     [] { return false; },
                                     [](qint64) { return false; },
                                     [](const QByteArray &, qint64) { return false; },
                                     [](qint64) { return false; },
                                     [] { return qint64(-1); },
                                     [] { return false; }};
        const QString received = dir.filePath(QStringLiteral("Received"));
        clip = std::make_unique<ClipboardSync>(node.get(), transfer.get(), settings.get(), none,
                                               [received] { return received; });
        paired = test::pairNodes(service, *node);
    }
    bool paired = false;

    // Publishes `offer` and announces it to `to` by hand, as a 0.1.30 publisher announces a copy
    // too large to list (no fl), with sp and `to` let in. Returns its kid.
    QByteArray announce(const QByteArray &to, const Offer &offer, qint64 stamp)
    {
        const quint64 offerId = transfer->publish(offer);
        transfer->allowPeer(offerId, to, true);
        const QByteArray kid = crypto::randomBytes(16);
        QCborMap msg;
        msg.insert(QStringLiteral("t"), QStringLiteral("clip"));
        msg.insert(QStringLiteral("v"), stamp);
        msg.insert(QStringLiteral("a"), node->id());
        msg.insert(QStringLiteral("kid"), kid);
        msg.insert(QStringLiteral("sf"), QCborMap());
        msg.insert(QStringLiteral("big"), QCborMap());
        msg.insert(QStringLiteral("fb"), offer.fileBytes());
        msg.insert(QStringLiteral("fc"), int(offer.files.size()));
        msg.insert(QStringLiteral("dc"), int(offer.dirs.size()));
        msg.insert(QStringLiteral("o"), 0);
        msg.insert(QStringLiteral("fo"), qint64(offerId));
        msg.insert(QStringLiteral("fa"), true);
        msg.insert(QStringLiteral("sp"), true);
        return node->sendMessage(to, msg) ? kid : QByteArray();
    }
};

// `dirCount` folders under "tree" and `fileCount` files in them, the files pointing at a path
// that need not exist (only listed, never read).
Offer syntheticCopy(int dirCount, int fileCount)
{
    Offer offer;
    offer.gated = true;
    offer.dirs.append(QStringLiteral("tree"));
    for (int d = 1; d < dirCount; ++d)
        offer.dirs.append(QStringLiteral("tree/d%1").arg(d));
    for (int f = 0; f < fileCount; ++f)
        offer.files.append(Offer::File{QStringLiteral("/nowhere/x"),
                                       QStringLiteral("tree/d%1/f%2.txt").arg(1 + f % std::max(dirCount - 1, 1)).arg(f),
                                       10});
    return offer;
}

QString hex(const QByteArray &id) { return QString::fromLatin1(id.toHex()); }

// The clipSet the clipboard owner (here the session helper) was last sent for `kid`.
QCborMap clipSetFor(const Client &owner, const QByteArray &kid)
{
    for (auto it = owner.received.crbegin(); it != owner.received.crend(); ++it) {
        if (it->value(QStringLiteral("ev")).toString() == QLatin1String("clipSet")
            && it->value(QStringLiteral("kid")).toByteArray() == kid)
            return *it;
    }
    return {};
}

// What runNode builds around the controller, on loopback and temporary folders.
struct Service {
    QTemporaryDir dir;
    QString instance = QStringLiteral("tst-servicecontroller-%1").arg(QCoreApplication::applicationPid());
    std::unique_ptr<Settings> settings;
    std::unique_ptr<Node> node;
    std::unique_ptr<Workspace> workspace;
    std::unique_ptr<InputController> input;
    std::unique_ptr<FileTransfer> transfer;
    std::unique_ptr<ClipboardSync> clipboard;
    std::unique_ptr<web::PhoneServer> phone;
    std::unique_ptr<RemoteView> remoteView;
    NoInstaller installer;
    std::unique_ptr<Updater> updater;
    std::unique_ptr<ServiceController> controller;

    bool start()
    {
        settings = std::make_unique<Settings>(dir.path());
        node = test::makeNode(dir.path());
        workspace = std::make_unique<Workspace>(node.get(), dir.path());
        input = std::make_unique<InputController>(node.get(), workspace.get());
        transfer = std::make_unique<FileTransfer>(node.get());
        ClipboardSync::Presence presence{[] { return QByteArray(); },
                                         [] { return false; },
                                         [](qint64) { return false; },
                                         [](const QByteArray &, qint64) { return false; },
                                         [](qint64) { return false; },
                                         [in = input.get()] { return in->lastPasteGestureAt(); },
                                         [in = input.get()] { return in->localPressSupported(); }};
        const QString received = dir.filePath(QStringLiteral("Received"));
        clipboard = std::make_unique<ClipboardSync>(node.get(), transfer.get(), settings.get(), presence,
                                                    [received] { return received; });
        web::PhoneServer::Options phoneOptions; // never started (phone access is off)
        phoneOptions.configDir = dir.path();
        phoneOptions.port = 0;
        const QString shared = dir.filePath(QStringLiteral("Shared"));
        phoneOptions.folder = [shared] { return shared; };
        phone = std::make_unique<web::PhoneServer>(phoneOptions);
        remoteView = std::make_unique<RemoteView>(node.get(), workspace.get());
        updater = std::make_unique<Updater>(node.get(), &installer,
                                            Updater::Options::defaults(dir.filePath(QStringLiteral("updates"))));
        return restartController();
    }

    bool restartController()
    {
        controller.reset();
        const QString shared = dir.filePath(QStringLiteral("Shared"));
        const QString received = dir.filePath(QStringLiteral("Received"));
        controller = std::make_unique<ServiceController>(
            node.get(), workspace.get(), input.get(), clipboard.get(), transfer.get(), settings.get(), phone.get(),
            remoteView.get(), updater.get(), [shared] { return shared; }, [received] { return received; }, instance,
            false, kToken);
        QString error;
        if (!controller->listen(&error)) {
            qWarning() << error;
            return false;
        }
        return true;
    }

    bool connectSession(Client &session)
    {
        if (!session.connectTo(instance))
            return false;
        QCborMap hello;
        hello.insert(QStringLiteral("role"), QStringLiteral("session"));
        hello.insert(QStringLiteral("token"), QString::fromLatin1(kToken));
        session.send(QStringLiteral("hello"), hello);
        return true;
    }
};

} // namespace

class TestServiceController : public QObject
{
    Q_OBJECT
private slots:
    void swapOfControlAndCommandIsKept()
    {
        Service s;
        QVERIFY(s.start());
        Client agent;
        QVERIFY(agent.connectTo(s.instance));
        const QByteArray device = QByteArray::fromHex("0123456789abcdef");
        QCborMap on;
        on.insert(QStringLiteral("id"), QString::fromLatin1(device.toHex()));
        on.insert(QStringLiteral("on"), true);
        agent.send(QStringLiteral("setSwapCtrlCmd"), on);
        QTRY_VERIFY(s.input->swapsCtrlCmd(device));
        QCOMPARE(s.settings->value(QStringLiteral("swapCtrlCmd")).toArray(),
                 QJsonArray{QString::fromLatin1(device.toHex())});

        // The next service applies it from the settings.
        agent.socket.abort();
        s.input->setSwapCtrlCmd({});
        QVERIFY(s.restartController());
        QVERIFY(s.input->swapsCtrlCmd(device));

        Client again;
        QVERIFY(again.connectTo(s.instance));
        QCborMap off = on;
        off.insert(QStringLiteral("on"), false);
        again.send(QStringLiteral("setSwapCtrlCmd"), off);
        QTRY_VERIFY(!s.input->swapsCtrlCmd(device));
        QVERIFY(s.settings->value(QStringLiteral("swapCtrlCmd")).toArray().isEmpty());
    }

    void inputProblemReachesTheAgentsWithASafeLink()
    {
        Service s;
        QVERIFY(s.start());
        Client agent, session;
        QVERIFY(agent.connectTo(s.instance));
        QVERIFY(s.connectSession(session));
        const auto state = [&agent] { return agent.last(QStringLiteral("state")); };

        const QString settingsLink =
            QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility");
        QCborMap status;
        status.insert(QStringLiteral("problem"), QStringLiteral("Allow DeskFlower in Accessibility"));
        status.insert(QStringLiteral("link"), settingsLink);
        session.send(QStringLiteral("inputStatus"), status);
        QTRY_COMPARE(state().value(QStringLiteral("inputProblem")).toString(),
                     QStringLiteral("Allow DeskFlower in Accessibility"));
        QCOMPARE(state().value(QStringLiteral("inputProblemLink")).toString(), settingsLink);

        // Anything but the system's settings is not offered as a link (the agent opens it).
        status.insert(QStringLiteral("problem"), QStringLiteral("Something else"));
        status.insert(QStringLiteral("link"), QStringLiteral("https://example.com/"));
        session.send(QStringLiteral("inputStatus"), status);
        QTRY_COMPARE(state().value(QStringLiteral("inputProblem")).toString(), QStringLiteral("Something else"));
        QCOMPARE(state().value(QStringLiteral("inputProblemLink")).toString(), QString());

        // Without a helper there is nothing to report: the next one says its own.
        session.socket.abort();
        QTRY_VERIFY(state().value(QStringLiteral("inputProblem")).toString().isEmpty());
    }

    // A connected paired device of another protocol is reported to the agents (its protocol,
    // older or newer, and whether this device's package would update it), so that they show the
    // alert; transfers with it are refused, the connection stays.
    void stateReportsProtocolMismatch()
    {
        Service s;
        QVERIFY(s.start());
        Peer p(*s.node);
        QVERIFY(p.paired);
        Client agent;
        QVERIFY(agent.connectTo(s.instance));
        const QString id = hex(p.node->id());
        auto peerState = [&]() -> QCborMap {
            for (const QCborValue &v : agent.last(QStringLiteral("state")).value(QStringLiteral("peers")).toArray()) {
                if (v.toMap().value(QStringLiteral("id")).toString() == id)
                    return v.toMap();
            }
            return {};
        };
        QTRY_COMPARE(peerState().value(QStringLiteral("protocol")).toInteger(), wire::kProtocolVersion);
        QCOMPARE(peerState().value(QStringLiteral("protocolMismatch")).toString(), QString());
        QCOMPARE(peerState().value(QStringLiteral("canServeUpdate")).toBool(), false); // no package here

        std::optional<int> pv;
        p.node->setHelloOverrideForTests([&](QCborMap &hello) {
            hello.insert(QStringLiteral("v"), QStringLiteral("0.1.30"));
            hello.remove(QStringLiteral("pv"));
            if (pv)
                hello.insert(QStringLiteral("pv"), *pv);
        });
        p.node->setCapability(QLatin1String(kDragCapability), true); // resends the hello
        QTRY_COMPARE(peerState().value(QStringLiteral("protocolMismatch")).toString(), QString("older"));
        QCOMPARE(peerState().value(QStringLiteral("protocol")).toInteger(), 2);
        QVERIFY(peerState().value(QStringLiteral("connected")).toBool());
        QVERIFY(!s.transfer->incompatibleReason(p.node->id()).isEmpty());

        pv = wire::kProtocolVersion + 1;
        p.node->setCapability(QLatin1String(kDragCapability), false);
        QTRY_COMPARE(peerState().value(QStringLiteral("protocolMismatch")).toString(), QString("newer"));
        QCOMPARE(peerState().value(QStringLiteral("protocol")).toInteger(), wire::kProtocolVersion + 1);

        p.node->setHelloOverrideForTests({});
        p.node->setCapability(QLatin1String(kDragCapability), true);
        QTRY_COMPARE(peerState().value(QStringLiteral("protocolMismatch")).toString(), QString());
        QVERIFY(s.transfer->incompatibleReason(p.node->id()).isEmpty());
    }

    void noticeFromTheHelperReachesTheAgents()
    {
        Service s;
        QVERIFY(s.start());
        Client agent, session;
        QVERIFY(agent.connectTo(s.instance));
        QVERIFY(s.connectSession(session));
        QCborMap empty;
        empty.insert(QStringLiteral("text"), QString());
        session.send(QStringLiteral("notice"), empty); // nothing to say: nothing shown
        QCborMap notice;
        notice.insert(QStringLiteral("text"), QStringLiteral("The copied items are here"));
        session.send(QStringLiteral("notice"), notice);
        QTRY_COMPARE(agent.last(QStringLiteral("notice")).value(QStringLiteral("message")).toString(),
                     QStringLiteral("The copied items are here"));
        int notices = 0;
        for (const QCborMap &m : std::as_const(agent.received))
            notices += m.value(QStringLiteral("ev")).toString() == QLatin1String("notice");
        QCOMPARE(notices, 1);
    }

    // Paste evidence from this device's own keyboard and mouse comes only from the session helper:
    // an agent (any console process) sending localPress or localPressSupport changes nothing. The
    // helper's are taken, and its support flag goes with it.
    void localPressComesOnlyFromTheHelper()
    {
        Service s;
        QVERIFY(s.start());
        Client agent, session;
        QVERIFY(agent.connectTo(s.instance));
        QCOMPARE(s.input->lastPasteGestureAt(), -1);

        QCborMap on;
        on.insert(QStringLiteral("on"), true);
        agent.send(QStringLiteral("localPress"));
        agent.send(QStringLiteral("localPressSupport"), on);
        // Something the agent may do, sent after them on the same pipe: once it is applied, they
        // were read too.
        const QByteArray device = QByteArray::fromHex("0123456789abcdef");
        QCborMap swap = on;
        swap.insert(QStringLiteral("id"), QString::fromLatin1(device.toHex()));
        agent.send(QStringLiteral("setSwapCtrlCmd"), swap);
        QTRY_VERIFY(s.input->swapsCtrlCmd(device));
        QCOMPARE(s.input->lastPasteGestureAt(), -1);
        QVERIFY(!s.input->localPressSupported());

        QVERIFY(s.connectSession(session));
        session.send(QStringLiteral("localPressSupport"), on);
        QTRY_VERIFY(s.input->localPressSupported());
        session.send(QStringLiteral("localPress"));
        QTRY_VERIFY(s.input->lastPasteGestureAt() >= 0);

        session.socket.abort();
        QTRY_VERIFY(!s.input->localPressSupported());
    }

    // A clipboard owner asking for copied files with no press at this device just before (a
    // reader, not a paste) is told so (code "noPaste") after the grace, and no transfer starts;
    // after the helper reports a press, the same request transfers them. A stale kid is refused.
    void clipFetchNeedsAPasteGesture()
    {
        Service s;
        QVERIFY(s.start());
        QTemporaryDir peerDir, source;
        auto peer = test::makeNode(peerDir.path());
        QVERIFY(test::pairNodes(*s.node, *peer));
        FileTransfer peerTransfer(peer.get());
        Settings peerSettings(peerDir.path());
        ClipboardSync::Presence none{[] { return QByteArray(); },
                                     [] { return false; },
                                     [](qint64) { return false; },
                                     [](const QByteArray &, qint64) { return false; },
                                     [](qint64) { return false; },
                                     [] { return qint64(-1); },
                                     [] { return false; }};
        const QString peerReceived = peerDir.filePath(QStringLiteral("Received"));
        ClipboardSync peerClip(peer.get(), &peerTransfer, &peerSettings, none, [peerReceived] { return peerReceived; });
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        const QString file = source.filePath(QStringLiteral("copied.txt"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("copied on the peer");
        }
        peerClip.localChanged({}, {file});
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty() && applied.last().at(3).toInt() == 1, 15000);
        const QByteArray kid = applied.last().at(0).toByteArray();
        QSignalSpy streams(peer.get(), &Node::streamAccepted);

        Client agent, session;
        QVERIFY(agent.connectTo(s.instance));
        QCborMap fetch;
        fetch.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
        agent.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY(!agent.last(QStringLiteral("clipFetched")).isEmpty());
        const QCborMap refused = agent.last(QStringLiteral("clipFetched"));
        QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
        QCOMPARE(refused.value(QStringLiteral("code")).toString(), QStringLiteral("noPaste"));
        QTest::qWait(200);
        QCOMPARE(streams.size(), 0);
        for (const QCborMap &m : std::as_const(agent.received))
            QVERIFY(m.value(QStringLiteral("ev")).toString() != QLatin1String("transfer"));

        // A stale clipboard object.
        agent.received.clear();
        QCborMap stale;
        stale.insert(QStringLiteral("kid"), QStringLiteral("00112233445566778899aabbccddeeff"));
        agent.send(QStringLiteral("clipFetch"), stale);
        QTRY_VERIFY(!agent.last(QStringLiteral("clipFetched")).isEmpty());
        QCOMPARE(agent.last(QStringLiteral("clipFetched")).value(QStringLiteral("code")).toString(),
                 QStringLiteral("changed"));

        // The helper reports a press: the paste transfers them.
        QVERIFY(s.connectSession(session));
        session.send(QStringLiteral("localPress"));
        QTRY_VERIFY(s.input->lastPasteGestureAt() >= 0);
        agent.received.clear();
        agent.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY_WITH_TIMEOUT(!agent.last(QStringLiteral("clipFetched")).isEmpty(), 15000);
        const QCborMap fetched = agent.last(QStringLiteral("clipFetched"));
        QVERIFY2(fetched.value(QStringLiteral("ok")).toBool(),
                 qPrintable(fetched.value(QStringLiteral("error")).toString()));
        QVERIFY(!fetched.contains(QStringLiteral("code")));
        const QCborArray files = fetched.value(QStringLiteral("files")).toArray();
        QCOMPARE(files.size(), 1);
        QFile got(QDir::fromNativeSeparators(files.at(0).toString()));
        QVERIFY(got.open(QIODevice::ReadOnly));
        QCOMPARE(got.readAll(), QByteArray("copied on the peer"));
        QCOMPARE(streams.size(), 1);
    }

    // A copy too large to list in its announcement (19,300 entries): the owner is told it may be
    // pasted while it arrives (clipSet stream), clipList answers in chunks of at most 5,000 items
    // that add up to the copy, and a second clipList is answered without asking the publisher
    // again. A stale kid gets one refusal.
    void clipListIsChunkedAndCachedPerClipboard()
    {
        Service s;
        QVERIFY(s.start());
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, agent;
        QVERIFY(s.connectSession(session));
        QVERIFY(agent.connectTo(s.instance));
        QSignalSpy streams(peer.node.get(), &Node::streamAccepted);
        const QByteArray kid = peer.announce(s.node->id(), syntheticCopy(300, 19000), 5);
        QVERIFY(!kid.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!clipSetFor(session, kid).isEmpty(), 15000);
        const QCborMap set = clipSetFor(session, kid);
        QCOMPARE(set.value(QStringLiteral("pendingFiles")).toInteger(), 19300);
        QVERIFY(set.value(QStringLiteral("stream")).toBool());
        QVERIFY(set.value(QStringLiteral("pendingList")).toArray().isEmpty());

        for (int round = 0; round < 2; ++round) {
            agent.received.clear();
            QCborMap list;
            list.insert(QStringLiteral("kid"), hex(kid));
            agent.send(QStringLiteral("clipList"), list);
            QTRY_VERIFY_WITH_TIMEOUT(!agent.last(QStringLiteral("clipListed")).isEmpty()
                                         && !agent.last(QStringLiteral("clipListed")).value(QStringLiteral("more")).toBool(),
                                     30000);
            const QList<QCborMap> chunks = agent.all(QStringLiteral("clipListed"));
            QCOMPARE(chunks.size(), 4);
            qsizetype items = 0;
            for (qsizetype i = 0; i < chunks.size(); ++i) {
                const QCborMap &c = chunks.at(i);
                QVERIFY2(c.value(QStringLiteral("ok")).toBool(), qPrintable(c.value(QStringLiteral("error")).toString()));
                QCOMPARE(c.value(QStringLiteral("kid")).toString(), hex(kid));
                QCOMPARE(c.value(QStringLiteral("more")).toBool(), i + 1 < chunks.size());
                const qsizetype n = c.value(QStringLiteral("items")).toArray().size();
                QVERIFY(n <= 5000);
                items += n;
            }
            QCOMPARE(items, 19300);
            QCOMPARE(chunks.first().value(QStringLiteral("items")).toArray().at(0).toArray().at(0).toString(),
                     QStringLiteral("tree"));
            QCOMPARE(streams.size(), 1);
        }

        agent.received.clear();
        QCborMap stale;
        stale.insert(QStringLiteral("kid"), QStringLiteral("00112233445566778899aabbccddeeff"));
        agent.send(QStringLiteral("clipList"), stale);
        QTRY_VERIFY(!agent.last(QStringLiteral("clipListed")).isEmpty());
        QCOMPARE(agent.all(QStringLiteral("clipListed")).size(), 1);
        const QCborMap refused = agent.last(QStringLiteral("clipListed"));
        QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
        QVERIFY(!refused.value(QStringLiteral("error")).toString().isEmpty());
        QVERIFY(!refused.value(QStringLiteral("more")).toBool());
        QCOMPARE(streams.size(), 1);
    }

    // A streaming paste needs a paste too (noPaste, and no clipStarted). With a press it is told
    // the transfer's folder (clipStarted) before the answer, and the files are there; another one
    // after the transfer is answered at once with the same folder, with no transfer.
    void streamingClipFetchIsToldItsFolderFirst()
    {
        Service s;
        QVERIFY(s.start());
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, paste;
        QVERIFY(s.connectSession(session));
        QTemporaryDir source;
        QDir().mkpath(source.filePath(QStringLiteral("docs/sub")));
        for (const char *name : {"docs/a.txt", "docs/sub/b.txt"}) {
            QFile f(source.filePath(QString::fromLatin1(name)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(name);
        }
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        peer.clip->localChanged({}, {source.filePath(QStringLiteral("docs"))});
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty() && applied.last().at(3).toInt() == 4, 15000);
        const QByteArray kid = applied.last().at(0).toByteArray();
        QTRY_VERIFY(!clipSetFor(session, kid).isEmpty());
        QVERIFY(clipSetFor(session, kid).value(QStringLiteral("stream")).toBool());
        QSignalSpy streams(peer.node.get(), &Node::streamAccepted);

        QVERIFY(paste.connectTo(s.instance, true));
        QCborMap fetch;
        fetch.insert(QStringLiteral("kid"), hex(kid));
        fetch.insert(QStringLiteral("stream"), true);
        paste.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY(!paste.last(QStringLiteral("clipFetched")).isEmpty());
        QCOMPARE(paste.last(QStringLiteral("clipFetched")).value(QStringLiteral("code")).toString(),
                 QStringLiteral("noPaste"));
        QCOMPARE(paste.indexOf(QStringLiteral("clipStarted")), -1);
        QCOMPARE(streams.size(), 0);

        session.send(QStringLiteral("localPress"));
        QTRY_VERIFY(s.input->lastPasteGestureAt() >= 0);
        paste.received.clear();
        paste.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY_WITH_TIMEOUT(!paste.last(QStringLiteral("clipFetched")).isEmpty(), 15000);
        const QCborMap fetched = paste.last(QStringLiteral("clipFetched"));
        QVERIFY2(fetched.value(QStringLiteral("ok")).toBool(), qPrintable(fetched.value(QStringLiteral("error")).toString()));
        const qsizetype startedAt = paste.indexOf(QStringLiteral("clipStarted"));
        QVERIFY(startedAt >= 0);
        QVERIFY(startedAt < paste.indexOf(QStringLiteral("clipFetched")));
        const QString root = QDir::fromNativeSeparators(paste.last(QStringLiteral("clipStarted")).value(QStringLiteral("root")).toString());
        QVERIFY(QFileInfo(root).isDir());
        QFile b(root + QStringLiteral("/docs/sub/b.txt"));
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), QByteArray("docs/sub/b.txt"));
        QCOMPARE(streams.size(), 1);
        QCOMPARE(s.clipboard->pinnedTargets(), QStringList{root});

        Client again;
        QVERIFY(again.connectTo(s.instance, true));
        again.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY(!again.last(QStringLiteral("clipFetched")).isEmpty());
        QVERIFY(again.last(QStringLiteral("clipFetched")).value(QStringLiteral("ok")).toBool());
        QCOMPARE(QDir::fromNativeSeparators(again.last(QStringLiteral("clipStarted")).value(QStringLiteral("root")).toString()),
                 root);
        QVERIFY(again.indexOf(QStringLiteral("clipStarted")) < again.indexOf(QStringLiteral("clipFetched")));
        QCOMPARE(streams.size(), 1);

        // Their connections hold the pin; closing them releases it (and deletes nothing).
        paste.socket.abort();
        again.socket.abort();
        QTRY_VERIFY(s.clipboard->pinnedTargets().isEmpty());
        QVERIFY(QFileInfo(root).isDir());
    }

    // Closing the connection of the only streaming paste of a transfer (Explorer cancelled, or
    // gave up) cancels it: the publisher's upload ends and the folder goes.
    void closingTheStreamingConnectionCancelsItsTransfer()
    {
        Service s;
        QVERIFY(s.start());
        s.transfer->setWriteDelayForTests(50);
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, paste;
        QVERIFY(s.connectSession(session));
        QTemporaryDir source;
        const QString file = source.filePath(QStringLiteral("slow.bin"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(4 * 1024 * 1024, 's'));
        }
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        peer.clip->localChanged({}, {file});
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty() && applied.last().at(3).toInt() == 1, 15000);
        const QByteArray kid = applied.last().at(0).toByteArray();
        session.send(QStringLiteral("localPress"));
        QTRY_VERIFY(s.input->lastPasteGestureAt() >= 0);
        QSignalSpy finished(s.transfer.get(), &FileTransfer::transferFinished);

        QVERIFY(paste.connectTo(s.instance, true));
        QCborMap fetch;
        fetch.insert(QStringLiteral("kid"), hex(kid));
        fetch.insert(QStringLiteral("stream"), true);
        paste.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY_WITH_TIMEOUT(!paste.last(QStringLiteral("clipStarted")).isEmpty(), 15000);
        const QString root = QDir::fromNativeSeparators(paste.last(QStringLiteral("clipStarted")).value(QStringLiteral("root")).toString());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + QStringLiteral("/slow.bin")), 20000);
        QVERIFY(paste.last(QStringLiteral("clipFetched")).isEmpty());
        paste.socket.abort();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().at(3).toString(), QStringLiteral("cancelled"));
        QTRY_COMPARE_WITH_TIMEOUT(peer.transfer->liveUploadsForTests(), 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root), 20000);
        QVERIFY(s.clipboard->pinnedTargets().isEmpty());
    }

    // Receive (clipReceive, main pipe): the current clipboard's files with no press, then applied
    // again with the local files; a stale kid is refused.
    void clipReceiveTakesTheFilesWithoutAPaste()
    {
        Service s;
        QVERIFY(s.start());
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, agent;
        QVERIFY(s.connectSession(session));
        QVERIFY(agent.connectTo(s.instance));
        QTemporaryDir source;
        const QString file = source.filePath(QStringLiteral("notes.txt"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("remember the milk");
        }
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        peer.clip->localChanged({}, {file});
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty() && applied.last().at(3).toInt() == 1, 15000);
        const QByteArray kid = applied.last().at(0).toByteArray();
        QCOMPARE(s.input->lastPasteGestureAt(), -1);

        QCborMap stale;
        stale.insert(QStringLiteral("kid"), QStringLiteral("00112233445566778899aabbccddeeff"));
        agent.send(QStringLiteral("clipReceive"), stale);
        QTRY_VERIFY(!agent.last(QStringLiteral("clipReceived")).isEmpty());
        QVERIFY(!agent.last(QStringLiteral("clipReceived")).value(QStringLiteral("ok")).toBool());
        QCOMPARE(agent.last(QStringLiteral("clipReceived")).value(QStringLiteral("code")).toString(),
                 QStringLiteral("changed"));

        agent.received.clear();
        session.received.clear();
        QCborMap receive;
        receive.insert(QStringLiteral("kid"), hex(kid));
        agent.send(QStringLiteral("clipReceive"), receive);
        QTRY_VERIFY_WITH_TIMEOUT(!agent.last(QStringLiteral("clipReceived")).isEmpty(), 15000);
        const QCborMap got = agent.last(QStringLiteral("clipReceived"));
        QVERIFY2(got.value(QStringLiteral("ok")).toBool(), qPrintable(got.value(QStringLiteral("error")).toString()));
        QCOMPARE(got.value(QStringLiteral("kid")).toString(), hex(kid));
        QTRY_VERIFY(!clipSetFor(session, kid).isEmpty());
        const QCborMap set = clipSetFor(session, kid);
        QCOMPARE(set.value(QStringLiteral("pendingFiles")).toInteger(), 0);
        const QCborArray files = set.value(QStringLiteral("files")).toArray();
        QCOMPARE(files.size(), 1);
        QFile f(QDir::fromNativeSeparators(files.at(0).toString()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("remember the milk"));
        QVERIFY(!set.contains(QStringLiteral("stream")));
    }

    // A transfer the agents are shown names its folder and can be cancelled (cancelTransfer): it
    // then ends as "cancelled", with the same folder, which goes.
    void cancelTransferEndsCancelledWithItsFolder()
    {
        Service s;
        QVERIFY(s.start());
        s.transfer->setWriteDelayForTests(50);
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, agent;
        QVERIFY(s.connectSession(session));
        QVERIFY(agent.connectTo(s.instance));
        QTemporaryDir source;
        const QString file = source.filePath(QStringLiteral("slow.bin"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(4 * 1024 * 1024, 's'));
        }
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        peer.clip->localChanged({}, {file});
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty() && applied.last().at(3).toInt() == 1, 15000);
        const QByteArray kid = applied.last().at(0).toByteArray();

        // Receive starts a transfer the user sees (no paste needed).
        QCborMap receive;
        receive.insert(QStringLiteral("kid"), hex(kid));
        agent.send(QStringLiteral("clipReceive"), receive);
        const auto running = [&agent] {
            const QCborMap t = agent.last(QStringLiteral("transfer"));
            return t.value(QStringLiteral("state")).toString() == QLatin1String("running") ? t : QCborMap();
        };
        QTRY_VERIFY_WITH_TIMEOUT(!running().isEmpty(), 15000);
        const QCborMap first = running();
        QCOMPARE(first.value(QStringLiteral("kind")).toString(), QStringLiteral("clipboard"));
        QVERIFY(first.value(QStringLiteral("cancellable")).toBool());
        const QString target = QDir::fromNativeSeparators(first.value(QStringLiteral("target")).toString());
        QVERIFY(!target.isEmpty());
        QVERIFY(target.startsWith(s.dir.filePath(QStringLiteral("Received"))));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target + QStringLiteral("/slow.bin")), 20000);

        QCborMap cancel;
        cancel.insert(QStringLiteral("id"), first.value(QStringLiteral("id")));
        agent.send(QStringLiteral("cancelTransfer"), cancel);
        QTRY_COMPARE_WITH_TIMEOUT(agent.last(QStringLiteral("transfer")).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 10000);
        const QCborMap ended = agent.last(QStringLiteral("transfer"));
        QCOMPARE(ended.value(QStringLiteral("id")).toInteger(), first.value(QStringLiteral("id")).toInteger());
        QCOMPARE(QDir::fromNativeSeparators(ended.value(QStringLiteral("target")).toString()), target);
        QVERIFY(!ended.value(QStringLiteral("cancellable")).toBool());
        QTRY_VERIFY(!agent.last(QStringLiteral("clipReceived")).isEmpty());
        QCOMPARE(agent.last(QStringLiteral("clipReceived")).value(QStringLiteral("code")).toString(),
                 QStringLiteral("cancelled"));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(target), 20000);

        // An id that is over is ignored.
        agent.received.clear();
        agent.send(QStringLiteral("cancelTransfer"), cancel);
        QTest::qWait(200);
        QVERIFY(agent.last(QStringLiteral("transfer")).isEmpty());
    }

    // A paste refused for want of a gesture is offered to the agents once per clipboard
    // (clipWaiting: count, from), also to an agent that connects afterwards; once Receive brought
    // the files the offer ends (count 0).
    void clipWaitingIsRelayedOncePerClipboard()
    {
        Service s;
        QVERIFY(s.start());
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, agent, paste;
        QVERIFY(s.connectSession(session));
        QVERIFY(agent.connectTo(s.instance));
        QTemporaryDir source;
        const QString file = source.filePath(QStringLiteral("notes.txt"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("remember the milk");
        }
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        peer.clip->localChanged({}, {file});
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty() && applied.last().at(3).toInt() == 1, 15000);
        const QByteArray kid = applied.last().at(0).toByteArray();
        QCOMPARE(agent.indexOf(QStringLiteral("clipWaiting")), -1);

        QVERIFY(paste.connectTo(s.instance, true));
        QCborMap fetch;
        fetch.insert(QStringLiteral("kid"), hex(kid));
        for (int round = 0; round < 2; ++round) {
            paste.received.clear();
            paste.send(QStringLiteral("clipFetch"), fetch);
            QTRY_VERIFY(!paste.last(QStringLiteral("clipFetched")).isEmpty());
            QCOMPARE(paste.last(QStringLiteral("clipFetched")).value(QStringLiteral("code")).toString(),
                     QStringLiteral("noPaste"));
        }
        QTRY_VERIFY(!agent.last(QStringLiteral("clipWaiting")).isEmpty());
        QTest::qWait(200);
        QCOMPARE(agent.all(QStringLiteral("clipWaiting")).size(), 1);
        const QCborMap waiting = agent.last(QStringLiteral("clipWaiting"));
        QCOMPARE(waiting.value(QStringLiteral("kid")).toString(), hex(kid));
        QCOMPARE(waiting.value(QStringLiteral("count")).toInteger(), 1);
        QVERIFY(!waiting.value(QStringLiteral("from")).toString().isEmpty());
        // The session helper is no agent: it is not offered anything.
        QCOMPARE(session.indexOf(QStringLiteral("clipWaiting")), -1);

        // An agent that comes later is told too.
        Client later;
        QVERIFY(later.connectTo(s.instance));
        later.send(QStringLiteral("agent"));
        QTRY_VERIFY(!later.last(QStringLiteral("clipWaiting")).isEmpty());
        QCOMPARE(later.last(QStringLiteral("clipWaiting")).value(QStringLiteral("count")).toInteger(), 1);

        QCborMap receive;
        receive.insert(QStringLiteral("kid"), hex(kid));
        agent.send(QStringLiteral("clipReceive"), receive);
        QTRY_VERIFY_WITH_TIMEOUT(!agent.last(QStringLiteral("clipReceived")).isEmpty(), 15000);
        QVERIFY2(agent.last(QStringLiteral("clipReceived")).value(QStringLiteral("ok")).toBool(),
                 qPrintable(agent.last(QStringLiteral("clipReceived")).value(QStringLiteral("error")).toString()));
        QTRY_COMPARE(agent.all(QStringLiteral("clipWaiting")).size(), 2);
        QCOMPARE(agent.last(QStringLiteral("clipWaiting")).value(QStringLiteral("kid")).toString(), hex(kid));
        QCOMPARE(agent.last(QStringLiteral("clipWaiting")).value(QStringLiteral("count")).toInteger(), 0);
        QTRY_COMPARE(later.last(QStringLiteral("clipWaiting")).value(QStringLiteral("count")).toInteger(), 0);

        // The files are here: a paste of them is not refused, nor offered again.
        paste.received.clear();
        paste.send(QStringLiteral("clipFetch"), fetch);
        QTRY_VERIFY(!paste.last(QStringLiteral("clipFetched")).isEmpty());
        QVERIFY(paste.last(QStringLiteral("clipFetched")).value(QStringLiteral("ok")).toBool());
        QTest::qWait(200);
        QCOMPARE(agent.all(QStringLiteral("clipWaiting")).size(), 2);
    }

    // A Receive offer ends (count 0 for its kid) when the clipboard is replaced, when the user
    // dismisses it (clipWaitingDismiss) and when the console user changes (reset): an agent that
    // connects afterwards is not given it (it named the previous copy's device, maybe another
    // user's).
    void clipWaitingEndsAndIsNotResent()
    {
        Service s;
        QVERIFY(s.start());
        Peer peer(*s.node);
        QVERIFY(peer.paired);
        Client session, agent, paste;
        QVERIFY(s.connectSession(session));
        QVERIFY(agent.connectTo(s.instance));
        agent.send(QStringLiteral("agent"));
        QVERIFY(paste.connectTo(s.instance, true));
        QTemporaryDir source;
        QSignalSpy applied(s.clipboard.get(), &ClipboardSync::applyLocally);
        // The peer copies a file and a paste of it is refused: the offer the agent gets.
        int copies = 0;
        const auto offer = [&]() -> QByteArray {
            const QString file = source.filePath(QStringLiteral("copy%1.txt").arg(++copies));
            {
                QFile f(file);
                if (!f.open(QIODevice::WriteOnly))
                    return {};
                f.write("remember the milk");
            }
            const qsizetype before = applied.size();
            peer.clip->localChanged({}, {file});
            if (!QTest::qWaitFor([&] { return applied.size() > before && applied.last().at(3).toInt() == 1; }, 15000))
                return {};
            const QByteArray kid = applied.last().at(0).toByteArray();
            QCborMap fetch;
            fetch.insert(QStringLiteral("kid"), hex(kid));
            for (int round = 0; round < 2; ++round) {
                paste.received.clear();
                paste.send(QStringLiteral("clipFetch"), fetch);
                if (!QTest::qWaitFor([&] { return !paste.last(QStringLiteral("clipFetched")).isEmpty(); }, 15000))
                    return {};
            }
            const auto offered = [&] {
                const QCborMap w = agent.last(QStringLiteral("clipWaiting"));
                return w.value(QStringLiteral("kid")).toString() == hex(kid)
                    && w.value(QStringLiteral("count")).toInteger() == 1;
            };
            return QTest::qWaitFor(offered, 15000) ? kid : QByteArray();
        };
        const auto ended = [&agent](const QByteArray &kid) {
            const QCborMap w = agent.last(QStringLiteral("clipWaiting"));
            return w.value(QStringLiteral("kid")).toString() == hex(kid)
                && w.value(QStringLiteral("count")).toInteger() == 0;
        };
        const auto laterAgentIsOffered = [&s]() {
            Client later;
            if (!later.connectTo(s.instance))
                return true;
            later.send(QStringLiteral("agent"));
            later.send(QStringLiteral("getState"));
            (void)QTest::qWaitFor([&] { return !later.last(QStringLiteral("state")).isEmpty(); }, 5000);
            QTest::qWait(100);
            return later.indexOf(QStringLiteral("clipWaiting")) != -1;
        };

        // The clipboard is replaced (the peer copies again).
        const QByteArray first = offer();
        QVERIFY(!first.isEmpty());
        QVERIFY(laterAgentIsOffered());
        const QString file = source.filePath(QStringLiteral("other.txt"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("something else");
        }
        peer.clip->localChanged({}, {file});
        QTRY_VERIFY_WITH_TIMEOUT(ended(first), 15000);
        QVERIFY(!laterAgentIsOffered());

        // The user dismisses the offer: the other agents hide it too, and it is not made again.
        const QByteArray second = offer();
        QVERIFY(!second.isEmpty());
        QCborMap other;
        other.insert(QStringLiteral("kid"), hex(first)); // not the current offer: ignored
        agent.send(QStringLiteral("clipWaitingDismiss"), other);
        QTest::qWait(200);
        QVERIFY(laterAgentIsOffered());
        QCborMap dismiss;
        dismiss.insert(QStringLiteral("kid"), hex(second));
        agent.send(QStringLiteral("clipWaitingDismiss"), dismiss);
        QTRY_VERIFY(ended(second));
        QVERIFY(!laterAgentIsOffered());

        // The console user changes.
        const QByteArray third = offer();
        QVERIFY(!third.isEmpty());
        s.clipboard->reset();
        QTRY_VERIFY(ended(third));
        QVERIFY(!laterAgentIsOffered());
    }
};

QTEST_MAIN(TestServiceController)
#include "tst_servicecontroller.moc"
