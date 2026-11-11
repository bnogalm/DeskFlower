// Two paired nodes with fake session helpers: the master's helper reports an edge hit and motion,
// the slave's helper must receive absolute positions, buttons and keys, and the master must get
// its input back when the cursor crosses back.

#include "buildinfo.h"
#include "inputcontroller.h"
#include "testutil.h"
#include "workspace.h"

#include <QCborArray>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <optional>

using namespace df;

namespace {
QCborMap cmd(const QString &name, std::initializer_list<std::pair<QString, QCborValue>> args = {})
{
    QCborMap m;
    m.insert(QStringLiteral("cmd"), name);
    for (const auto &[k, v] : args)
        m.insert(k, v);
    return m;
}

// Last event with the given name, or an empty map.
QCborMap lastEvent(const QSignalSpy &spy, const QString &name)
{
    for (qsizetype i = spy.size() - 1; i >= 0; --i) {
        const QCborMap m = spy.at(i).at(0).value<QCborMap>();
        if (m.value(QStringLiteral("ev")).toString() == name)
            return m;
    }
    return {};
}

int count(const QSignalSpy &spy, const QString &name)
{
    int n = 0;
    for (const auto &args : spy)
        n += args.at(0).value<QCborMap>().value(QStringLiteral("ev")).toString() == name;
    return n;
}

// A (1920x1080) on the left, sharing, B on its right with the given screens; both input
// controllers; B's session helper connected unless told otherwise.
struct Pair {
    QTemporaryDir dirA, dirB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Workspace> wa, wb;
    std::unique_ptr<InputController> ia, ib;
    std::unique_ptr<QSignalSpy> toA, toB;

    bool setUp(const QList<ScreenInfo> &screensB, bool sessionB = true)
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        wa = std::make_unique<Workspace>(a.get(), dirA.path());
        wb = std::make_unique<Workspace>(b.get(), dirB.path());
        wa->setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        wb->setLocalScreens(screensB);
        if (!test::pairNodes(*a, *b) || !QTest::qWaitFor([&] { return wa->devices().size() == 2; }, 5000))
            return false;
        wa->setLayout({{a->id(), {0, 0}}, {b->id(), {1920, 0}}});
        wa->setSharing(true);
        if (!QTest::qWaitFor([&] { return wb->masterId() == a->id(); }, 5000))
            return false;
        ia = std::make_unique<InputController>(a.get(), wa.get());
        ib = std::make_unique<InputController>(b.get(), wb.get());
        toA = std::make_unique<QSignalSpy>(ia.get(), &InputController::toSession);
        toB = std::make_unique<QSignalSpy>(ib.get(), &InputController::toSession);
        ia->sessionConnected();
        if (sessionB)
            ib->sessionConnected();
        return true;
    }

    void enterB(int y = 500)
    {
        ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", y}}));
    }

    // The helper captured at the edge on its own (edge map) and says so.
    void softEdge(Side side, int x, int y)
    {
        ia->handleSessionMessage(cmd("edge", {{"s", int(side)}, {"x", x}, {"y", y}, {"c", true}}));
    }
};

// The spans of the last "edges" event the helper got.
QCborArray edgesOf(const QSignalSpy &spy)
{
    return lastEvent(spy, "edges").value(QStringLiteral("e")).toArray();
}
} // namespace

class TestInput : public QObject
{
    Q_OBJECT
private slots:
    void masterDrivesSlaveAndGetsInputBack()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        Workspace wa(a.get(), dirA.path());
        Workspace wb(b.get(), dirB.path());
        wa.setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        wb.setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 2560, 1440), 1, true}});
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(wa.devices().size(), 2);
        QTRY_COMPARE(wb.devices().size(), 2);
        // B to the right of A, 100 px lower.
        wa.setLayout({{a->id(), {0, 0}}, {b->id(), {1920, 100}}});
        wa.setSharing(true);
        QTRY_COMPARE(wb.masterId(), a->id());
        QTRY_COMPARE(wb.layout().offsets.value(b->id()), QPoint(1920, 100));

        InputController ia(a.get(), &wa);
        InputController ib(b.get(), &wb);
        QSignalSpy toA(&ia, &InputController::toSession);
        QSignalSpy toB(&ib, &InputController::toSession);
        ia.sessionConnected();
        ib.sessionConnected();
        QCOMPARE(lastEvent(toA, "config").value(QStringLiteral("master")).toBool(), true);
        QCOMPARE(lastEvent(toB, "config").value(QStringLiteral("master")).toBool(), false);

        // An edge hit above B's top does nothing.
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 50}}));
        QCOMPARE(ia.activeDevice(), a->id());

        // Pushing past A's right edge at y=600 enters B at (1, 500): one pixel inside, so jitter
        // cannot bounce straight back.
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 600}}));
        QCOMPARE(ia.activeDevice(), b->id());
        QCOMPARE(lastEvent(toA, "capture").value(QStringLiteral("on")).toBool(), true);
        QTRY_VERIFY(!lastEvent(toB, "mabs").isEmpty());
        QCOMPARE(lastEvent(toB, "mabs").value(QStringLiteral("x")).toInteger(), 1);
        QCOMPARE(lastEvent(toB, "mabs").value(QStringLiteral("y")).toInteger(), 500);

        // Motion becomes absolute positions on B.
        ia.handleSessionMessage(cmd("mm", {{"x", 300}, {"y", -20}}));
        QTRY_COMPARE(lastEvent(toB, "mabs").value(QStringLiteral("x")).toInteger(), 301);
        QCOMPARE(lastEvent(toB, "mabs").value(QStringLiteral("y")).toInteger(), 480);

        // Clicks carry the position; keys are forwarded as scancodes.
        ia.handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}}));
        ia.handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        ia.handleSessionMessage(cmd("key", {{"s", 0x1E}, {"d", true}}));
        ia.handleSessionMessage(cmd("key", {{"s", 0x1E}, {"d", false}}));
        ia.handleSessionMessage(cmd("mw", {{"x", 0}, {"y", -120}}));
        QTRY_COMPARE(count(toB, "key"), 2);
        QCOMPARE(count(toB, "mb"), 2);
        QCOMPARE(lastEvent(toB, "mb").value(QStringLiteral("x")).toInteger(), 301);
        QCOMPARE(lastEvent(toB, "key").value(QStringLiteral("s")).toInteger(), 0x1E);
        QTRY_COMPARE(count(toB, "mw"), 1);

        // Pushing past B's bottom is blocked (nothing there): the cursor is clamped.
        ia.handleSessionMessage(cmd("mm", {{"x", 0}, {"y", 5000}}));
        QCOMPARE(ia.activeDevice(), b->id());
        QCOMPARE(ia.cursor(), QPoint(301, 1439));

        // Back up and out of B's left edge at y=600 (plane y 700): A gets its input at (1919, 700)?
        // A is only 1080 high, so first move up to plane y 500 (B y=400).
        ia.handleSessionMessage(cmd("mm", {{"x", 0}, {"y", -1039}}));
        QCOMPARE(ia.cursor(), QPoint(301, 400));
        ia.handleSessionMessage(cmd("mm", {{"x", -400}, {"y", 0}}));
        QCOMPARE(ia.activeDevice(), a->id());
        const QCborMap off = lastEvent(toA, "capture");
        QCOMPARE(off.value(QStringLiteral("on")).toBool(), false);
        // 301 - 400 is 99 pixels past B's edge: the first is A's edge (1919), the rest goes on.
        QCOMPARE(off.value(QStringLiteral("x")).toInteger(), 1821);
        QCOMPARE(off.value(QStringLiteral("y")).toInteger(), 500);
        QTRY_COMPARE(count(toB, "releaseAll"), 1);

        // An edge hit on the edge we just came back through is a bounce: ignored for a moment...
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}}));
        QCOMPARE(ia.activeDevice(), a->id());
        // ...then a deliberate push crosses again.
        QTest::qWait(300);
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}}));
        QCOMPARE(ia.activeDevice(), b->id());

        // A locks while the cursor is on B: the cursor comes home and what is typed next (the
        // lock screen password) never reaches B.
        QTRY_COMPARE(count(toB, "mabs") > 0, true);
        const int keysOnB = count(toB, "key");
        ia.returnHome();
        QCOMPARE(ia.activeDevice(), a->id());
        QCOMPARE(lastEvent(toA, "capture").value(QStringLiteral("on")).toBool(), false);
        ia.handleSessionMessage(cmd("key", {{"s", 0x1E}, {"d", true}}));
        ia.handleSessionMessage(cmd("key", {{"s", 0x1E}, {"d", false}}));
        QTRY_COMPARE(count(toB, "releaseAll"), 2);
        QTest::qWait(200);
        QCOMPARE(count(toB, "key"), keysOnB);
    }

    // The cursor is on B when B's screens change: it stays on what is left, and when no edge leads
    // back to A any more it comes home instead of being stuck there.
    void alwaysAWayHome()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true},
                         ScreenInfo{"b2", "", QRect(1920, 0, 1920, 1080), 1, false}}));
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        p.ia->handleSessionMessage(cmd("mm", {{"x", 2500}, {"y", 0}}));
        QCOMPARE(p.ia->cursor(), QPoint(2501, 500)); // on b2, away from A

        // b2 gets a lower resolution: the cursor moves onto what is left of it.
        p.wb->setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true},
                               ScreenInfo{"b2", "", QRect(1920, 0, 400, 300), 1, false}});
        QTRY_COMPARE(p.ia->cursor(), QPoint(2319, 299));
        QCOMPARE(p.ia->activeDevice(), p.b->id());

        // b1 goes off (only b2 is left, further right): no edge leads back to A.
        p.wb->setLocalScreens({ScreenInfo{"b2", "", QRect(1920, 0, 400, 300), 1, true}});
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(lastEvent(*p.toA, "capture").value(QStringLiteral("on")).toBool(), false);

        // Back on B (b1 is back), whose screens then disappear altogether.
        p.wb->setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}});
        QTRY_VERIFY(p.wa->screens(p.b->id()).size() == 1);
        QTest::qWait(300); // past the guard of the edge we came back through
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        p.wb->setLocalScreens({});
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
    }

    // B paired A as a guest: A's keyboard and mouse never drive B, and A keeps its input. B said
    // so in its hello, so A does not even try: B leaves the helper's edge map and an edge hit
    // towards it stays home, with no refusal round trip (and no capture thrown back every few
    // seconds). Should A enter anyway (before the hello), B refuses.
    void aGuestMasterIsRefused()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        QTRY_COMPARE(edgesOf(*p.toA).size(), 1);
        QSignalSpy refused(p.ia.get(), &InputController::inputRefused);
        p.b->setTrust(p.a->id(), TrustLevel::Guest);
        QTRY_COMPARE(p.a->declaredTrust(p.b->id()), std::optional<TrustLevel>(TrustLevel::Guest));
        QTRY_VERIFY(edgesOf(*p.toA).isEmpty());
        p.enterB();
        QTest::qWait(300);
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(count(*p.toB, "mabs"), 0);
        QCOMPARE(refused.size(), 0);

        // The refusal itself, for a master that did not know yet.
        QSignalSpy toMaster(p.a.get(), &Node::messageReceived);
        QCborMap enter;
        enter.insert(QStringLiteral("t"), QStringLiteral("in.enter"));
        enter.insert(QStringLiteral("x"), 10);
        enter.insert(QStringLiteral("y"), 10);
        QVERIFY(p.a->sendMessage(p.b->id(), enter));
        QTRY_VERIFY(!toMaster.isEmpty());
        QCOMPARE(toMaster.last().at(1).value<QCborMap>().value(QStringLiteral("t")).toString(),
                 QString("in.refused"));
        QCOMPARE(count(*p.toB, "mabs"), 0);
    }

    // The cursor stopping at the edge of a device that declared this one a guest is explained,
    // once in a while rather than on every push (whether the helper reported a plain edge hit or
    // captured on its own). A device that refuses the input explains itself (inputRefused): the
    // pushes during the pause that follows add nothing, with the production intervals.
    void aBlockedCrossingIsExplained()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        QSignalSpy blocked(p.ia.get(), &InputController::crossingBlocked);
        p.b->setTrust(p.a->id(), TrustLevel::Guest);
        QTRY_COMPARE(p.a->declaredTrust(p.b->id()), std::optional<TrustLevel>(TrustLevel::Guest));
        p.enterB();
        QCOMPARE(blocked.size(), 1);
        QCOMPARE(blocked.first().at(0).toByteArray(), p.b->id());
        p.enterB();
        p.softEdge(Side::Right, 1919, 400);
        QCOMPARE(blocked.size(), 1); // not again so soon
        QCOMPARE(lastEvent(*p.toA, "capture").value(QStringLiteral("on")).toBool(), false);
        QCOMPARE(lastEvent(*p.toA, "capture").value(QStringLiteral("ge")).toInteger(), int(Side::Right));
        QCOMPARE(p.ia->activeDevice(), p.a->id());

        p.ia->setBlockedNoticeInterval(200);
        QTest::qWait(250);
        p.softEdge(Side::Right, 1919, 400);
        QCOMPARE(blocked.size(), 2);
        p.ia->setBlockedNoticeInterval(InputController::kBlockedNoticeMs);

        // B takes A back: nothing blocks any more. Then B's helper goes: B refuses the input,
        // which says so (inputRefused); pushing again during the pause is silent.
        p.b->setTrust(p.a->id(), TrustLevel::Device);
        QTRY_COMPARE(p.a->declaredTrust(p.b->id()), std::optional<TrustLevel>(TrustLevel::Device));
        p.ib->sessionDisconnected();
        QSignalSpy refused(p.ia.get(), &InputController::inputRefused);
        p.enterB();
        QTRY_COMPARE(refused.size(), 1);
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(refused.size(), 1);
        QCOMPARE(blocked.size(), 2);
        // After the pause B is entered again, and refuses again: one more notice of its own.
        QTest::qWait(InputController::kRefusedRetryMs + 100);
        p.enterB();
        QTRY_COMPARE(refused.size(), 2);
        QCOMPARE(blocked.size(), 2);
    }

    // A is flagged as a guest while its cursor is on B: B drops its input from then on, so it
    // sends the cursor home at once instead of leaving it stuck, and no longer counts it as here.
    void demotingTheMasterSendsItsCursorHome()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        QTRY_VERIFY(p.ib->cursorArrived());
        const int releases = count(*p.toB, "releaseAll");
        p.b->setTrust(p.a->id(), TrustLevel::Guest);
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(p.ib->cursorArrived(), false);
        QCOMPARE(count(*p.toB, "releaseAll"), releases + 1);
    }

    // Nothing runs in B's session to move its cursor: B refuses the input and A keeps it, instead
    // of capturing the keyboard and mouse for a cursor nobody sees.
    void aDeviceThatCannotTakeInputRefusesIt()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}, false));
        QSignalSpy refused(p.ia.get(), &InputController::inputRefused);
        p.enterB();
        QTRY_COMPARE(refused.size(), 1);
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(lastEvent(*p.toA, "capture").value(QStringLiteral("on")).toBool(), false);
        // Pushing on the edge does not go straight back to it.
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.a->id());

        // Its session comes back: once the pause is over, the cursor goes there again...
        p.ib->sessionConnected();
        QTest::qWait(InputController::kRefusedRetryMs + 100);
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        QTRY_VERIFY(!lastEvent(*p.toB, "mabs").isEmpty());
        // ...and if it goes away while the cursor is there, the input comes home.
        p.ib->sessionDisconnected();
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(refused.size(), 2);
    }

    // The master's helper could not keep the keyboard and mouse (Windows took its hooks away):
    // the input is local again, where the helper left the cursor.
    void aBusyDeviceJumpsToTheLatestPosition()
    {
        // A device that falls behind (busy CPU) must not replay every old position in slow motion
        // when it catches up: it goes straight to the latest one.
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.enterB();
        QTRY_VERIFY(!lastEvent(*p.toB, "mabs").isEmpty());
        const QPoint start = p.ia->cursor();
        const int before = count(*p.toB, "mabs");

        // 300 small moves while B processes nothing (they pile up in its socket).
        constexpr int kMoves = 300;
        for (int n = 0; n < kMoves; ++n)
            p.ia->handleSessionMessage(cmd("mm", {{"x", n % 2 ? 2 : 1}, {"y", 1}}));
        const QPoint end = p.ia->cursor();
        QCOMPARE(end, start + QPoint(kMoves / 2 * 3, kMoves));
        QTRY_COMPARE(lastEvent(*p.toB, "mabs").value("x").toInteger(), end.x());
        QCOMPARE(lastEvent(*p.toB, "mabs").value("y").toInteger(), end.y());
        const int applied = count(*p.toB, "mabs") - before;
        QVERIFY2(applied <= 5, qPrintable(QStringLiteral("%1 positions replayed").arg(applied)));

        // A click right after lands where the cursor is, after its last move.
        p.ia->handleSessionMessage(cmd("mm", {{"x", 7}, {"y", 0}}));
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}}));
        QTRY_VERIFY(!lastEvent(*p.toB, "mb").isEmpty());
        QCOMPARE(lastEvent(*p.toB, "mb").value("x").toInteger(), end.x() + 7);
        QTRY_COMPARE(lastEvent(*p.toB, "mabs").value("x").toInteger(), end.x() + 7);
    }

    void theMotionPastAnEdgeGoesOn()
    {
        // A fast (or coalesced) move that crosses an edge does not stop at it: what is left of the
        // move goes on past the edge, as on a single desktop.
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            QCOMPARE(p.ia->cursor(), QPoint(1, 500));
            p.ia->handleSessionMessage(cmd("mm", {{"x", 20}, {"y", 0}}));
            QCOMPARE(p.ia->cursor(), QPoint(21, 500));
            // 21 - 30 is 9 pixels past B's left edge: A's edge pixel, then 8 more.
            p.ia->handleSessionMessage(cmd("mm", {{"x", -30}, {"y", 0}}));
            QCOMPARE(p.ia->activeDevice(), p.a->id());
            QCOMPARE(lastEvent(*p.toA, "capture").value("x").toInteger(), 1911);
        }
        {
            // Past the whole target screen: one pixel inside it.
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            p.ia->handleSessionMessage(cmd("mm", {{"x", -5000}, {"y", 0}}));
            QCOMPARE(p.ia->activeDevice(), p.a->id());
            QCOMPARE(lastEvent(*p.toA, "capture").value("x").toInteger(), 1918);
        }
    }

    void lostCaptureBringsTheInputHome()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        p.ia->handleSessionMessage(cmd("captureLost", {{"x", 960}, {"y", 540}}));
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(p.ia->cursor(), QPoint(960, 540));
        QTRY_VERIFY(count(*p.toB, "releaseAll") >= 1); // B released what it was holding
        // Nothing more reaches B.
        const int keysOnB = count(*p.toB, "key");
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1E}, {"d", true}}));
        QTest::qWait(200);
        QCOMPARE(count(*p.toB, "key"), keysOnB);
    }

    // The master's helper is told where a push past an edge will cross, so that it can capture at
    // once (InputBackend::setEdgeMap): B's stretch of A's right edge, as long as B is connected and
    // A is the master.
    void theHelperKnowsWhereTheEdgesLead()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        QTRY_COMPARE(edgesOf(*p.toA).size(), 1);
        const QCborMap span = edgesOf(*p.toA).first().toMap();
        QCOMPARE(span.value("s").toInteger(), int(Side::Right));
        QCOMPARE(QRect(int(span.value("x").toInteger()), int(span.value("y").toInteger()),
                       int(span.value("w").toInteger()), int(span.value("h").toInteger())),
                 QRect(0, 0, 1920, 1080));
        QCOMPARE(span.value("a").toInteger(), 0);
        QCOMPARE(span.value("b").toInteger(), 1079);
        // B is not the master: nothing to capture early there.
        QVERIFY(!lastEvent(*p.toB, "edges").isEmpty());
        QVERIFY(edgesOf(*p.toB).isEmpty());

        // A stops sharing: its helper's map empties; sharing again fills it.
        const int sent = count(*p.toA, "edges");
        p.wa->setSharing(false);
        QTRY_VERIFY(count(*p.toA, "edges") > sent);
        QVERIFY(edgesOf(*p.toA).isEmpty());
        p.wa->setSharing(true);
        QTRY_COMPARE(edgesOf(*p.toA).size(), 1);

        // B flagged as remote view is viewed, not entered: off the map, unless remote view is
        // switched off on A altogether, which enters it as usual.
        p.wa->setRemoteView(p.b->id(), true);
        QTRY_VERIFY(edgesOf(*p.toA).isEmpty());
        p.a->setRemoteViewEnabled(false);
        QTRY_COMPARE(edgesOf(*p.toA).size(), 1);
        p.a->setRemoteViewEnabled(true);
        QTRY_VERIFY(edgesOf(*p.toA).isEmpty());
        p.wa->setRemoteView(p.b->id(), false);
        QTRY_COMPARE(edgesOf(*p.toA).size(), 1);

        // B goes away: off the map.
        p.a->unpair(p.b->id());
        QTRY_VERIFY(edgesOf(*p.toA).isEmpty());
    }

    // B refuses the input (nothing runs in its session): it leaves the map until the pause is
    // over, so the helper does not capture for a device that will not take it.
    void aRefusingDeviceLeavesTheMapForAMoment()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}, false));
        QTRY_COMPARE(edgesOf(*p.toA).size(), 1);
        p.enterB();
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QTRY_VERIFY(edgesOf(*p.toA).isEmpty());
        QTest::qWait(InputController::kRefusedRetryMs + 200);
        QCOMPARE(edgesOf(*p.toA).size(), 1);
    }

    // The helper captured at the edge on its own but nothing crosses there: it is told to let the
    // cursor go where it was, and not to capture on that edge again for a moment (the cursor is
    // still pushed against it).
    void anEarlyCaptureIsDeclinedWhenNothingCrosses()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        // Nothing above A.
        int captures = count(*p.toA, "capture");
        p.softEdge(Side::Top, 700, 0);
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(count(*p.toA, "capture"), captures + 1);
        QCborMap off = lastEvent(*p.toA, "capture");
        QCOMPARE(off.value("on").toBool(), false);
        QCOMPARE(off.value("x").toInteger(), 700);
        QCOMPARE(off.value("y").toInteger(), 0);
        QCOMPARE(off.value("ge").toInteger(), int(Side::Top));

        // Back from B through A's right edge: the return carries that edge (the helper does not
        // capture early there for a moment either)...
        p.softEdge(Side::Right, 1919, 500);
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        p.ia->handleSessionMessage(cmd("mm", {{"x", -30}, {"y", 0}}));
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        off = lastEvent(*p.toA, "capture");
        QCOMPARE(off.value("on").toBool(), false);
        QCOMPARE(off.value("ge").toInteger(), int(Side::Right));
        // ...and an early capture on it within the guard is declined at the edge point.
        captures = count(*p.toA, "capture");
        p.softEdge(Side::Right, 1919, 480);
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(count(*p.toA, "capture"), captures + 1);
        off = lastEvent(*p.toA, "capture");
        QCOMPARE(off.value("on").toBool(), false);
        QCOMPARE(off.value("x").toInteger(), 1919);
        QCOMPARE(off.value("y").toInteger(), 480);
        QCOMPARE(off.value("ge").toInteger(), int(Side::Right));

        // A helper that is not the master's is always declined.
        p.ib->handleSessionMessage(cmd("edge", {{"s", int(Side::Left)}, {"x", 0}, {"y", 10}, {"c", true}}));
        QCOMPARE(lastEvent(*p.toB, "capture").value("on").toBool(), false);
        QCOMPARE(lastEvent(*p.toB, "capture").value("x").toInteger(), 0);
    }

    // The helper captured at the edge on its own while the input was already on its way to B (the
    // crossing went through a plain report, or a drag catch, just before): the capture is what the
    // service wants, so it is kept; declining it would free the mouse while the input is still
    // routed to B.
    void anEarlyCaptureWhileAwayIsKept()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.enterB();
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        int captures = count(*p.toA, "capture");
        p.softEdge(Side::Right, 1919, 500);
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        QCOMPARE(count(*p.toA, "capture"), captures);

        // Back home, then out again with the button held: the crossing waits for the drag catch,
        // and the button is released on the edge meanwhile.
        p.ia->handleSessionMessage(cmd("mm", {{"x", -30}, {"y", 0}}));
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QTest::qWait(300); // past the guard of the edge we came back through
        QSignalSpy catches(p.ia.get(), &InputController::dragCatchRequested);
        p.ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}, {"lb", true}}));
        QCOMPARE(catches.size(), 1);
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        // An early capture meanwhile is declined, the edge guarded...
        captures = count(*p.toA, "capture");
        p.softEdge(Side::Right, 1919, 500);
        QCOMPARE(count(*p.toA, "capture"), captures + 1);
        QCOMPARE(lastEvent(*p.toA, "capture").value("on").toBool(), false);
        QCOMPARE(lastEvent(*p.toA, "capture").value("ge").toInteger(), int(Side::Right));
        // ...and once the answer lets the crossing go ahead, one in flight is kept.
        p.ia->dragCaught(p.a->id(), false);
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        QCOMPARE(lastEvent(*p.toA, "capture").value("on").toBool(), true);
        captures = count(*p.toA, "capture");
        p.softEdge(Side::Right, 1919, 500);
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        QCOMPARE(count(*p.toA, "capture"), captures);
    }

    // B's helper goes away while its cursor is driven: a position still in flight is not kept for
    // the next helper, which gets its configuration first and nothing from before.
    void nothingFromABygoneSessionReachesTheNextHelper()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.enterB();
        p.ia->handleSessionMessage(cmd("mm", {{"x", 300}, {"y", 0}}));
        QTRY_COMPARE(lastEvent(*p.toB, "mabs").value("x").toInteger(), 301);
        p.ib->sessionDisconnected();
        p.ia->handleSessionMessage(cmd("mm", {{"x", 10}, {"y", 0}})); // sent before B's refusal arrives
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QTest::qWait(100);
        const qsizetype before = p.toB->size();
        p.ib->sessionConnected();
        QTest::qWait(200);
        QVERIFY(p.toB->size() > before);
        QCOMPARE(p.toB->at(before).at(0).value<QCborMap>().value("ev").toString(), QStringLiteral("config"));
        for (qsizetype n = before; n < p.toB->size(); ++n)
            QVERIFY(p.toB->at(n).at(0).value<QCborMap>().value("ev").toString() != QLatin1String("mabs"));
    }

    // The helper captured at the edge on its own and the crossing goes ahead: the capture is
    // confirmed, B gets the entry position, and the motion reported right after the edge (what
    // was moved while the service answered) goes on from there, nothing lost.
    void anEarlyCaptureLosesNoMotion()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.softEdge(Side::Right, 1919, 500);
        p.ia->handleSessionMessage(cmd("mm", {{"x", 300}, {"y", -20}}));
        p.ia->handleSessionMessage(cmd("mm", {{"x", 5}, {"y", 0}}));
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        QCOMPARE(lastEvent(*p.toA, "capture").value("on").toBool(), true);
        QCOMPARE(p.ia->cursor(), QPoint(306, 480));
        QTRY_COMPARE(lastEvent(*p.toB, "mabs").value("x").toInteger(), 306);
        QCOMPARE(lastEvent(*p.toB, "mabs").value("y").toInteger(), 480);
        // The entry position went first.
        bool entered = false;
        for (const auto &args : *p.toB) {
            const QCborMap m = args.at(0).value<QCborMap>();
            if (m.value("ev").toString() == QLatin1String("mabs")) {
                entered = m.value("x").toInteger() == 1 && m.value("y").toInteger() == 500;
                break;
            }
        }
        QVERIFY(entered);
    }

    // Moves read together from the helper make one datagram (the latest position), sent once the
    // batch is handled; a click right after still comes after that position.
    void theMasterSendsOnePositionPerPass()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        p.enterB();
        QTRY_VERIFY(!lastEvent(*p.toB, "mabs").isEmpty());
        QCoreApplication::processEvents();
        const quint32 seq = p.ia->motionSeq();
        constexpr int kMoves = 300;
        for (int n = 0; n < kMoves; ++n)
            p.ia->handleSessionMessage(cmd("mm", {{"x", 1}, {"y", 1}}));
        const QPoint end = p.ia->cursor();
        QCOMPARE(end, QPoint(1 + kMoves, 500 + kMoves));
        QCOMPARE(p.ia->motionSeq(), seq); // nothing sent yet
        QCoreApplication::processEvents();
        QCOMPARE(p.ia->motionSeq(), seq + 1); // one datagram
        QTRY_COMPARE(lastEvent(*p.toB, "mabs").value("x").toInteger(), end.x());

        p.ia->handleSessionMessage(cmd("mm", {{"x", 7}, {"y", 0}}));
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}}));
        QCOMPARE(p.ia->motionSeq(), seq + 2); // flushed before the button
        QTRY_VERIFY(!lastEvent(*p.toB, "mb").isEmpty());
        QCOMPARE(lastEvent(*p.toB, "mb").value("x").toInteger(), end.x() + 7);
        QTRY_COMPARE(lastEvent(*p.toB, "mabs").value("x").toInteger(), end.x() + 7);
    }

    void onlyTheMasterIsObeyed()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        Workspace wa(a.get(), dirA.path());
        Workspace wb(b.get(), dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        InputController ib(b.get(), &wb);
        QSignalSpy toB(&ib, &InputController::toSession);
        ib.sessionConnected();

        // A is not the master: its input messages are ignored.
        QCborMap key;
        key.insert(QStringLiteral("t"), QStringLiteral("in.key"));
        key.insert(QStringLiteral("s"), 0x1E);
        key.insert(QStringLiteral("d"), true);
        a->sendMessage(b->id(), key);
        QTest::qWait(300);
        QCOMPARE(count(toB, "key"), 0);

        wa.setSharing(true);
        QTRY_COMPARE(wb.masterId(), a->id());
        a->sendMessage(b->id(), key);
        QTRY_COMPARE(count(toB, "key"), 1);
    }

    // Keyboard diagnostics follow each key at both ends: forwarded by the master (to whom), received
    // by the device it drives (from whom); one the helper sends while the input is home is
    // reported as not forwarded (empty peer).
    void theKeyTraceSeesBothEnds()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        struct Trace {
            QByteArray peer;
            quint32 sc;
            bool down, sent;
        };
        QList<Trace> onA, onB;
        p.ia->setKeyTrace([&](const QByteArray &peer, quint32 sc, bool down, bool sent) { onA.append({peer, sc, down, sent}); });
        p.ib->setKeyTrace([&](const QByteArray &peer, quint32 sc, bool down, bool sent) { onB.append({peer, sc, down, sent}); });

        p.ia->handleSessionMessage(cmd("key", {{"s", 0xE05B}, {"d", true}}));
        QCOMPARE(onA.size(), 1);
        QVERIFY(onA.at(0).peer.isEmpty()); // the input is home: not forwarded

        p.enterB();
        p.ia->handleSessionMessage(cmd("key", {{"s", 0xE05B}, {"d", true}})); // Win
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x20}, {"d", true}}));   // D
        QCOMPARE(onA.size(), 3);
        QCOMPARE(onA.at(1).peer, p.b->id());
        QCOMPARE(onA.at(1).sc, 0xE05Bu);
        QVERIFY(onA.at(1).down && onA.at(1).sent);
        QTRY_COMPARE(onB.size(), 2);
        QCOMPARE(onB.at(0).peer, p.a->id());
        QCOMPARE(onB.at(0).sc, 0xE05Bu);
        QVERIFY(onB.at(0).down && !onB.at(0).sent);
        QCOMPARE(onB.at(1).sc, 0x20u);

        // A key from a device that does not drive this one is reported as ignored, with why.
        QStringList ignoredOnA;
        p.ia->setIgnoredKeyTrace([&](const QByteArray &peer, quint32 sc, bool down, const QString &why) {
            if (peer == p.b->id() && sc == 0x20 && down)
                ignoredOnA << why;
        });
        QCborMap key;
        key.insert(QStringLiteral("t"), QStringLiteral("in.key"));
        key.insert(QStringLiteral("s"), 0x20);
        key.insert(QStringLiteral("d"), true);
        QVERIFY(p.b->sendMessage(p.a->id(), key));
        QTRY_COMPARE(ignoredOnA.size(), 1);
        QVERIFY(ignoredOnA.at(0).contains(QStringLiteral("master")));
        p.ia->setIgnoredKeyTrace({});
        p.ia->setKeyTrace({});
        p.ib->setKeyTrace({});
    }

    // The master watches the keys it forwards for a paste chord (Ctrl+V, Shift+Insert, Ctrl+Shift+V)
    // and records any press per device, for the sealed clipboard's paste grant. The held set is
    // cleared when the cursor enters a device or comes back.
    void pasteChordIsRecognisedOnForwardedKeys()
    {
        // Ctrl+V: a chord. Ctrl alone is not; either press is input on B.
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            const QByteArray b = p.b->id();
            QVERIFY(!p.ia->inputOn(b, 1000));
            QVERIFY(!p.ia->pasteOrderedOn(b, 1000));
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}})); // Ctrl
            QVERIFY(p.ia->inputOn(b, 1000));
            QVERIFY(!p.ia->pasteOrderedOn(b, 1000)); // Ctrl alone is no chord
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", true}})); // V
            QVERIFY(p.ia->pasteOrderedOn(b, 1000));
            QVERIFY(!p.ia->pasteOrderedOn(QByteArray("other"), 1000)); // only the device it was ordered on
            // B judges the same keys on arrival: its own paste evidence (file data).
            QTRY_VERIFY(p.ib->lastPasteGestureAt() >= 0);
            QCOMPARE(p.ia->lastPasteGestureAt(), -1); // nothing happened at A itself
        }
        // Shift+Insert is a chord; Ctrl+Shift+V is a chord.
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x2A}, {"d", true}}));   // Left Shift
            p.ia->handleSessionMessage(cmd("key", {{"s", 0xE052}, {"d", true}})); // Insert (extended)
            QVERIFY(p.ia->pasteOrderedOn(p.b->id(), 1000));
        }
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}})); // Ctrl
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x2A}, {"d", true}})); // Shift
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", true}})); // V
            QVERIFY(p.ia->pasteOrderedOn(p.b->id(), 1000));
        }
        // The held set is cleared on a crossing: Ctrl held before leaving does not combine with a V
        // pressed after coming back.
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}})); // Ctrl held
            p.ia->handleSessionMessage(cmd("mm", {{"x", -5000}, {"y", 0}}));    // back to A (clears held)
            QCOMPARE(p.ia->activeDevice(), p.a->id());
            QTest::qWait(300);
            p.enterB();
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", true}})); // V alone
            QVERIFY(p.ia->inputOn(p.b->id(), 1000));
            QVERIFY(!p.ia->pasteOrderedOn(p.b->id(), 1000)); // no Ctrl held: not a chord
        }
        // Win+V (the scancodes Command travels as) is a chord only on a Mac: on Windows it opens
        // the clipboard history and pastes nothing, so it must not grant a paste there.
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            p.ia->handleSessionMessage(cmd("key", {{"s", 0xE05B}, {"d", true}})); // Left Win / Command
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", true}}));   // V
            QVERIFY(p.ia->inputOn(p.b->id(), 1000));
            const bool bIsMac = build::platform().startsWith(QLatin1String("macos")); // B runs here
            QCOMPARE(p.ia->pasteOrderedOn(p.b->id(), 1000), bIsMac);
            QTRY_COMPARE(count(*p.toB, "key"), 2);
            QCOMPARE(p.ib->lastPasteGestureAt() >= 0, bIsMac);
        }
        // A button press from the master to a device is input (inputArrived), not a chord.
        {
            Pair p;
            QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
            p.enterB();
            QTRY_VERIFY(p.ib->cursorArrived());
            QVERIFY(!p.ib->inputArrived(1000));
            // The master forwards a key to B; B records it arrived.
            p.ia->handleSessionMessage(cmd("key", {{"s", 0x1E}, {"d", true}}));
            QTRY_VERIFY(p.ib->inputArrived(1000));
            QCOMPARE(p.ib->lastPasteGestureAt(), -1); // a letter is no paste gesture
            // A button press is one (a click on Paste, a menu, Retry...).
            p.ia->handleSessionMessage(cmd("mb", {{"b", 2}, {"d", true}}));
            QTRY_VERIFY(p.ib->lastPasteGestureAt() >= 0);
        }
    }

    void controlAndCommandAreSwappedForChosenDevices()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        const QByteArray b = p.b->id();
        QVERIFY(!p.ia->swapsCtrlCmd(b));
        p.enterB();
        const auto lastKey = [&p] { return lastEvent(*p.toB, "key").value(QStringLiteral("s")).toInteger(); };

        // Off: the codes go as they are.
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}}));
        QTRY_COMPARE(lastKey(), 0x1D);
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", false}}));
        QTRY_COMPARE(count(*p.toB, "key"), 2);

        // On: Ctrl and the Windows key (Command on a Mac) trade places, both ways and both sides;
        // other keys are untouched.
        p.ia->setSwapCtrlCmd({b});
        QVERIFY(p.ia->swapsCtrlCmd(b));
        const QList<QPair<int, int>> swapped = {{0x1D, 0xE05B}, {0xE01D, 0xE05C}, {0xE05B, 0x1D}, {0xE05C, 0xE01D},
                                                {0x2F, 0x2F}};
        int keys = 2;
        for (const auto &[from, to] : swapped) {
            p.ia->handleSessionMessage(cmd("key", {{"s", from}, {"d", true}}));
            ++keys;
            QTRY_COMPARE(count(*p.toB, "key"), keys);
            QCOMPARE(lastKey(), qint64(to));
            p.ia->handleSessionMessage(cmd("key", {{"s", from}, {"d", false}}));
            ++keys;
            QTRY_COMPARE(count(*p.toB, "key"), keys);
            QCOMPARE(lastKey(), qint64(to));
        }

        // The paste chord is judged on what the device gets: Ctrl+V typed here is Command+V there,
        // a paste only on a Mac (the device here runs this platform).
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}}));
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", true}}));
        const bool bIsMac = build::platform().startsWith(QLatin1String("macos"));
        QCOMPARE(p.ia->pasteOrderedOn(b, 1000), bIsMac);
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", false}}));
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", false}}));

        // Another device is not affected.
        p.ia->setSwapCtrlCmd({QByteArray("other")});
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}}));
        QTRY_COMPARE(lastKey(), 0x1D);
    }

    void swappedControlPastesOnAMac()
    {
        // B says it is a Mac (its hello): with the swap, Ctrl+V typed here is Command+V there, the
        // paste chord of a Mac (judged on what the device gets, whatever this host runs).
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        QCborMap hello;
        hello.insert(QStringLiteral("t"), QStringLiteral("hello"));
        hello.insert(QStringLiteral("n"), p.b->name());
        hello.insert(QStringLiteral("tp"), p.b->tcpPort());
        hello.insert(QStringLiteral("v"), QStringLiteral("0.1.22"));
        hello.insert(QStringLiteral("pl"), QStringLiteral("macos-arm64"));
        hello.insert(QStringLiteral("tl"), QStringLiteral("device"));
        p.b->sendMessage(p.a->id(), hello);
        QTRY_VERIFY([&] {
            for (const PeerView &v : p.a->peers())
                if (v.id == p.b->id())
                    return v.platform == QLatin1String("macos-arm64");
            return false;
        }());
        const QByteArray b = p.b->id();
        p.ia->setSwapCtrlCmd({b});
        p.enterB();
        const auto lastKey = [&p] { return lastEvent(*p.toB, "key").value(QStringLiteral("s")).toInteger(); };
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x1D}, {"d", true}}));
        QTRY_COMPARE(lastKey(), 0xE05B); // Command
        QVERIFY(!p.ia->pasteOrderedOn(b, 1000));
        p.ia->handleSessionMessage(cmd("key", {{"s", 0x2F}, {"d", true}}));
        QVERIFY(p.ia->pasteOrderedOn(b, 1000));
        // B itself judges what it gets, Command+V, by the platform it really runs (this host).
        QTRY_COMPARE(count(*p.toB, "key"), 2);
        QCOMPARE(p.ib->lastPasteGestureAt() >= 0, build::platform().startsWith(QLatin1String("macos")));
    }

    // Paste evidence for file data on the device the master drives: forwarded paste chords,
    // gesture keys (Enter, Apps, Shift+F10) and button presses count; letters, V alone, a modifier
    // alone, motion and the wheel do not. The keys held there are forgotten on in.leave and on
    // releaseAll, so a Control held before does not make a later V a chord.
    void forwardedPasteGesturesAreEvidence()
    {
        Pair p;
        QVERIFY(p.setUp({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}));
        QSignalSpy gestures(p.ib.get(), &InputController::pasteGesture);
        p.enterB();
        QTRY_VERIFY(p.ib->cursorArrived());
        int keys = 0;
        const auto key = [&](int sc, bool down) {
            p.ia->handleSessionMessage(cmd("key", {{"s", sc}, {"d", down}}));
            ++keys;
            return QTest::qWaitFor([&] { return count(*p.toB, "key") == keys; }, 5000);
        };
        const auto tapKey = [&](int sc) { return key(sc, true) && key(sc, false); };

        // Nothing that is not a gesture.
        p.ia->handleSessionMessage(cmd("mm", {{"x", 40}, {"y", 30}}));
        p.ia->handleSessionMessage(cmd("mw", {{"x", 0}, {"y", -120}}));
        QTRY_VERIFY(!lastEvent(*p.toB, "mw").isEmpty());
        QVERIFY(tapKey(0x2F)); // V alone
        QVERIFY(tapKey(0x1E)); // A
        QVERIFY(tapKey(0x44)); // F10 alone
        QVERIFY(tapKey(0x1D)); // Control alone
        QCOMPARE(gestures.size(), 0);
        QCOMPARE(p.ib->lastPasteGestureAt(), -1);

        // Ctrl+V, Enter, keypad Enter, Apps, Shift+F10: one gesture each, autorepeat included.
        QVERIFY(key(0x1D, true));
        QVERIFY(key(0x2F, true));
        QCOMPARE(gestures.size(), 1);
        const qint64 first = p.ib->lastPasteGestureAt();
        QVERIFY(first >= 0);
        QVERIFY(key(0x2F, true)); // autorepeat of V with Control still held
        QCOMPARE(gestures.size(), 2);
        QVERIFY(p.ib->lastPasteGestureAt() >= first);
        QVERIFY(key(0x2F, false));
        QVERIFY(key(0x1D, false));
        QVERIFY(tapKey(0x2F)); // V alone again: Control was released
        QCOMPARE(gestures.size(), 2);
        QVERIFY(tapKey(0x1C));
        QVERIFY(tapKey(0xE01C));
        QVERIFY(tapKey(0xE05D));
        QVERIFY(key(0x36, true)); // Right Shift
        QVERIFY(tapKey(0x44));    // F10
        QVERIFY(key(0x36, false));
        QCOMPARE(gestures.size(), 6);

        // A button press is a gesture; its release is not.
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}}));
        QTRY_COMPARE(gestures.size(), 7);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE(count(*p.toB, "mb"), 2);
        QCOMPARE(gestures.size(), 7);

        // in.leave forgets the held keys: Control held, back to A and in again, V alone.
        QVERIFY(key(0x1D, true));
        p.ia->handleSessionMessage(cmd("mm", {{"x", -5000}, {"y", 0}}));
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QTRY_VERIFY(!p.ib->cursorArrived());
        QTest::qWait(300); // past the return guard
        p.enterB();
        QTRY_VERIFY(p.ib->cursorArrived());
        QVERIFY(tapKey(0x2F));
        QCOMPARE(gestures.size(), 7);

        // releaseAll forgets them too (here: a view request being decided blocks another peer).
        QVERIFY(key(0x1D, true));
        p.ib->setRemoteInputBlocked(QByteArray("someone else"), true);
        p.ib->setRemoteInputBlocked(QByteArray("someone else"), false);
        QVERIFY(tapKey(0x2F));
        QCOMPARE(gestures.size(), 7);

        // So does a new helper on B (a restarted one replacing the old before it was dropped).
        QVERIFY(key(0x1D, true));
        p.ib->sessionConnected();
        QVERIFY(tapKey(0x2F));
        QCOMPARE(gestures.size(), 7);
    }

    // Local presses come from the session helper (localPress, through the service): each one is a
    // gesture. Whether the helper reports them at all is its own say (localPressSupport), false
    // until then and again once it goes, or when another helper connects.
    void localPressesAndTheirSupportFollowTheHelper()
    {
        QTemporaryDir dir;
        auto node = test::makeNode(dir.path());
        Workspace w(node.get(), dir.path());
        InputController input(node.get(), &w);
        QSignalSpy gestures(&input, &InputController::pasteGesture);
        QCOMPARE(input.lastPasteGestureAt(), -1);
        QVERIFY(!input.localPressSupported());

        input.sessionConnected();
        QVERIFY(!input.localPressSupported());
        input.setLocalPressSupported(true);
        QVERIFY(input.localPressSupported());
        input.noteLocalPasteGesture(); // the input is home: taken all the same
        QCOMPARE(gestures.size(), 1);
        QVERIFY(input.lastPasteGestureAt() >= 0);

        input.sessionDisconnected();
        QVERIFY(!input.localPressSupported());
        input.sessionConnected();
        input.setLocalPressSupported(true);
        input.sessionConnected(); // a restarted helper replaces it before saying anything
        QVERIFY(!input.localPressSupported());
    }

    void masterDisconnectReleasesKeys()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        Workspace wa(a.get(), dirA.path());
        Workspace wb(b.get(), dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        wa.setSharing(true);
        QTRY_COMPARE(wb.masterId(), a->id());
        InputController ib(b.get(), &wb);
        QSignalSpy toB(&ib, &InputController::toSession);
        ib.sessionConnected();
        a.reset();
        QTRY_VERIFY_WITH_TIMEOUT(count(toB, "releaseAll") >= 1, 10000);
    }
};

QTEST_GUILESS_MAIN(TestInput)
#include "tst_input.moc"
