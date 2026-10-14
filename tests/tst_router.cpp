#include "router.h"

#include <QTest>

using namespace df;

namespace {
// A: two monitors side by side (1920x1080 + 2560x1440, second one starting at x=1920).
// B: one 1920x1080 monitor whose native origin is (0,0), placed to the right of A.
// C: one 1280x1024 monitor placed below A's first monitor.
QHash<QByteArray, QList<ScreenInfo>> screens()
{
    return {
        {"A", {ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true},
               ScreenInfo{"a2", "", QRect(1920, 0, 2560, 1440), 1, false}}},
        {"B", {ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}}},
        {"C", {ScreenInfo{"c1", "", QRect(-500, -200, 1280, 1024), 1, true}}},
    };
}

QMap<QByteArray, QPoint> offsets()
{
    return {{"A", {0, 0}}, {"B", {4480, 200}}, {"C", {500, 1280}}};
}
} // namespace

class TestRouter : public QObject
{
    Q_OBJECT
private slots:
    void crossRightIntoNeighbour()
    {
        Router r;
        r.set(screens(), offsets());
        // Right edge of A's second monitor at y=500 -> B's left edge; B is 200 lower in the plane.
        const auto c = r.cross("A", QPoint(4479, 500), Side::Right);
        QVERIFY(c);
        QCOMPARE(c->device, QByteArray("B"));
        QCOMPARE(c->point, QPoint(0, 300));
    }

    void noNeighbourAboveTheTarget()
    {
        Router r;
        r.set(screens(), offsets());
        // y=100 is above B's top (B starts at 200 in the plane).
        QVERIFY(!r.cross("A", QPoint(4479, 100), Side::Right));
        QVERIFY(!r.cross("A", QPoint(0, 500), Side::Left));
    }

    void crossBackLeft()
    {
        Router r;
        r.set(screens(), offsets());
        const auto c = r.cross("B", QPoint(0, 300), Side::Left);
        QVERIFY(c);
        QCOMPARE(c->device, QByteArray("A"));
        QCOMPARE(c->point, QPoint(4479, 500));
    }

    void crossDownWithNativeOriginOffset()
    {
        Router r;
        r.set(screens(), offsets());
        // C's native origin is (-500,-200), its pack offset (500,1280) => plane rect (0,1080) 1280x1024.
        const auto c = r.cross("A", QPoint(100, 1079), Side::Bottom);
        QVERIFY(c);
        QCOMPARE(c->device, QByteArray("C"));
        QCOMPARE(c->point, QPoint(100 - 500, -200));
        // Past C's width there is nothing below A's first monitor.
        QVERIFY(!r.cross("A", QPoint(1500, 1079), Side::Bottom));
    }

    void smallGapsAreTolerated()
    {
        Router r;
        auto off = offsets();
        off["B"] = QPoint(4480 + Router::kMaxGap, 200);
        r.set(screens(), off);
        QVERIFY(r.cross("A", QPoint(4479, 500), Side::Right));
        off["B"] = QPoint(4480 + Router::kMaxGap + 2, 200);
        r.set(screens(), off);
        QVERIFY(!r.cross("A", QPoint(4479, 500), Side::Right));
    }

    void containsAndClamp()
    {
        Router r;
        r.set(screens(), offsets());
        QVERIFY(r.contains("A", QPoint(1920, 1200)));  // second monitor is taller
        QVERIFY(!r.contains("A", QPoint(100, 1200)));  // below the first monitor
        // Moving down from the first monitor clamps to its bottom row.
        QCOMPARE(r.clamp("A", QPoint(100, 1000), QPoint(100, 1300)), QPoint(100, 1079));
        QCOMPARE(r.primaryCenter("A"), QRect(0, 0, 1920, 1080).center());
    }
};

QTEST_GUILESS_MAIN(TestRouter)
#include "tst_router.moc"
