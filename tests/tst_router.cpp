#include "router.h"

#include <QTest>

#include <algorithm>

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

    // The stretches of a device's edges behind which another device lies, as cross() sees them.
    void neighbourSpans()
    {
        Router r;
        r.set(screens(), offsets());
        const auto find = [](const QList<EdgeSpan> &spans, Side side, const QByteArray &device) {
            for (const EdgeSpan &s : spans) {
                if (s.side == side && s.device == device)
                    return s;
            }
            return EdgeSpan{};
        };
        // A: B behind the right edge of its second monitor (plane y 200..1279), C below the first.
        QList<EdgeSpan> a = r.neighbourSpans("A");
        QCOMPARE(a.size(), 2);
        EdgeSpan s = find(a, Side::Right, "B");
        QCOMPARE(s.screen, QRect(1920, 0, 2560, 1440));
        QCOMPARE(s.from, 200);
        QCOMPARE(s.to, 1279);
        s = find(a, Side::Bottom, "C");
        QCOMPARE(s.screen, QRect(0, 0, 1920, 1080));
        QCOMPARE(s.from, 0);
        QCOMPARE(s.to, 1279);
        // B: A along its whole left edge. C: A above, in C's own coordinates (origin -500,-200).
        const QList<EdgeSpan> b = r.neighbourSpans("B");
        QCOMPARE(b.size(), 1);
        QCOMPARE(b.first().side, Side::Left);
        QCOMPARE(b.first().device, QByteArray("A"));
        QCOMPARE(b.first().from, 0);
        QCOMPARE(b.first().to, 1079);
        const QList<EdgeSpan> c = r.neighbourSpans("C");
        QCOMPARE(c.size(), 1);
        QCOMPARE(c.first().side, Side::Top);
        QCOMPARE(c.first().screen, QRect(-500, -200, 1280, 1024));
        QCOMPARE(c.first().from, -500);
        QCOMPARE(c.first().to, 779);
        QVERIFY(r.neighbourSpans("D").isEmpty());

        // The same gap rule as cross().
        auto off = offsets();
        off["B"] = QPoint(4480 + Router::kMaxGap, 200);
        r.set(screens(), off);
        QCOMPARE(find(r.neighbourSpans("A"), Side::Right, "B").to, 1279);
        off["B"] = QPoint(4480 + Router::kMaxGap + 2, 200);
        r.set(screens(), off);
        QCOMPARE(r.neighbourSpans("A").size(), 1);
        QVERIFY(r.neighbourSpans("B").isEmpty());

        // B above A's first monitor: a span along the top edge, in x.
        off["B"] = QPoint(300, -1080);
        r.set(screens(), off);
        s = find(r.neighbourSpans("A"), Side::Top, "B");
        QCOMPARE(s.screen, QRect(0, 0, 1920, 1080));
        QCOMPARE(s.from, 300);
        QCOMPARE(s.to, 1919);

        // Two screens of B stacked behind the right edge: one span each.
        auto two = screens();
        two["B"] = {ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true},
                    ScreenInfo{"b2", "", QRect(0, 1080, 1920, 360), 1, false}};
        off["B"] = QPoint(4480, 0);
        r.set(two, off);
        a = r.neighbourSpans("A");
        QCOMPARE(a.size(), 3);
        QList<QPair<int, int>> ranges;
        for (const EdgeSpan &e : a) {
            if (e.side == Side::Right)
                ranges.append({e.from, e.to});
        }
        std::sort(ranges.begin(), ranges.end());
        QCOMPARE(ranges, (QList<QPair<int, int>>{{0, 1079}, {1080, 1439}}));
    }

    // Two devices behind the same stretch of an edge, at different gaps: the stretch goes to the
    // nearer one, which cross() picks there, and the other keeps only what is left.
    void overlappingNeighboursGoToTheNearer()
    {
        const auto find = [](const QList<EdgeSpan> &spans, const QByteArray &device) {
            for (const EdgeSpan &s : spans) {
                if (s.device == device)
                    return s;
            }
            return EdgeSpan{};
        };
        Router r;
        auto off = offsets();
        // C to the right of A's second monitor as well, 20 px further than B, plane y 700..1723.
        off["C"] = QPoint(5000, 900);
        r.set(screens(), off);
        QCOMPARE(r.cross("A", QPoint(4479, 1000), Side::Right)->device, QByteArray("B"));
        QCOMPARE(r.cross("A", QPoint(4479, 1300), Side::Right)->device, QByteArray("C"));
        QList<EdgeSpan> a = r.neighbourSpans("A");
        QCOMPARE(a.size(), 2);
        for (const EdgeSpan &s : a) {
            QCOMPARE(s.side, Side::Right);
            QCOMPARE(s.screen, QRect(1920, 0, 2560, 1440));
        }
        QCOMPARE(find(a, "B").from, 200);
        QCOMPARE(find(a, "B").to, 1279);
        QCOMPARE(find(a, "C").from, 1280);
        QCOMPARE(find(a, "C").to, 1439);

        // The other way round (C nearer): B keeps the stretch above C only.
        off["B"] = QPoint(4500, 200);
        off["C"] = QPoint(4980, 900);
        r.set(screens(), off);
        QCOMPARE(r.cross("A", QPoint(4479, 1000), Side::Right)->device, QByteArray("C"));
        a = r.neighbourSpans("A");
        QCOMPARE(a.size(), 2);
        QCOMPARE(find(a, "B").from, 200);
        QCOMPARE(find(a, "B").to, 699);
        QCOMPARE(find(a, "C").from, 700);
        QCOMPARE(find(a, "C").to, 1439);
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
