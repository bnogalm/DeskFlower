// Two paired nodes with fake session helpers: the master's helper reports an edge hit and motion,
// the slave's helper must receive absolute positions, buttons and keys, and the master must get
// its input back when the cursor crosses back.

#include "inputcontroller.h"
#include "testutil.h"
#include "workspace.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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
        QCOMPARE(off.value(QStringLiteral("x")).toInteger(), 1918); // one pixel inside A
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
