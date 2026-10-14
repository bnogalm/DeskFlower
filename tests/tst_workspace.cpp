#include "testutil.h"
#include "workspace.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
QList<ScreenInfo> twoScreens(const QString &prefix)
{
    return {ScreenInfo{prefix + "1", "Left", QRect(0, 0, 1920, 1080), 1.0, true},
            ScreenInfo{prefix + "2", "Right", QRect(1920, 0, 2560, 1440), 1.5, false}};
}

struct Pair {
    QTemporaryDir dirA, dirB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Workspace> wa, wb;

    Pair()
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        wa = std::make_unique<Workspace>(a.get(), dirA.path());
        wb = std::make_unique<Workspace>(b.get(), dirB.path());
    }
    bool start() { return test::pairNodes(*a, *b); }
};
} // namespace

class TestWorkspace : public QObject
{
    Q_OBJECT
private slots:
    void stampOrdering()
    {
        QVERIFY((Stamp{2, "a"}).newerThan(Stamp{1, "z"}));
        QVERIFY((Stamp{1, "b"}).newerThan(Stamp{1, "a"}));
        QVERIFY(!(Stamp{1, "a"}).newerThan(Stamp{1, "a"}));
    }

    void screensReplicate()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        QVERIFY(p.start());
        // Sent on connect...
        QTRY_COMPARE(p.wb->screens(p.a->id()), twoScreens("A"));
        // ...and on change.
        QList<ScreenInfo> one = {twoScreens("A").first()};
        p.wa->setLocalScreens(one);
        QTRY_COMPARE(p.wb->screens(p.a->id()), one);
    }

    void thumbnailsNeedViewGrant()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        QVERIFY(p.start());
        QTRY_COMPARE(p.wb->screens(p.a->id()).size(), 2);

        // No grant: B is told so, gets no pixels, and A does not even capture for it.
        QSignalSpy requested(p.wa.get(), &Workspace::localThumbnailsRequested);
        p.wa->setLocalThumbnail("A1", "jpeg-bytes");
        p.wb->requestRemoteThumbnails();
        QTRY_VERIFY(p.wb->thumbnailsDenied(p.a->id()));
        QTest::qWait(200);
        QVERIFY(p.wb->thumbnail(p.a->id(), "A1").isEmpty());
        QCOMPARE(requested.size(), 0);

        // Granting answers B's pending request.
        p.a->setViewAllowed(p.b->id(), true);
        QTRY_COMPARE(p.wb->thumbnail(p.a->id(), "A1"), QByteArray("jpeg-bytes"));
        QVERIFY(!p.wb->thumbnailsDenied(p.a->id()));
        QCOMPARE(requested.size(), 1);
        p.wa->setLocalThumbnail("A1", "fresh");
        QTRY_COMPARE(p.wb->thumbnail(p.a->id(), "A1"), QByteArray("fresh"));

        // Revoking drops B's copy and stops further updates.
        p.a->setViewAllowed(p.b->id(), false);
        QTRY_VERIFY(p.wb->thumbnailsDenied(p.a->id()));
        QVERIFY(p.wb->thumbnails(p.a->id()).isEmpty());
        p.wa->setLocalThumbnail("A1", "after-revoke");
        QTest::qWait(300);
        QVERIFY(p.wb->thumbnail(p.a->id(), "A1").isEmpty());
    }

    void unpairDropsThumbnails()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        QVERIFY(p.start());
        p.wa->setLocalThumbnail("A1", "jpeg-bytes");
        p.a->setViewAllowed(p.b->id(), true);
        QTRY_COMPARE(p.wb->thumbnail(p.a->id(), "A1"), QByteArray("jpeg-bytes"));
        p.a->unpair(p.b->id());
        QTRY_VERIFY(p.wb->thumbnails(p.a->id()).isEmpty());
    }

    void disconnectDropsThumbnails()
    {
        // A revoke or unpair that never reaches the viewer must not leave the pixels there.
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        QVERIFY(p.start());
        p.wa->setLocalThumbnail("A1", "jpeg-bytes");
        p.a->setViewAllowed(p.b->id(), true);
        QTRY_COMPARE(p.wb->thumbnail(p.a->id(), "A1"), QByteArray("jpeg-bytes"));
        const QByteArray aId = p.a->id();
        p.wa.reset();
        p.a.reset(); // gone silently
        QTRY_VERIFY_WITH_TIMEOUT(p.wb->thumbnails(aId).isEmpty(), 20000);
    }

    void thumbnailsGoOnlyToRequester()
    {
        QTemporaryDir dirA, dirB, dirC;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto c = test::makeNode(dirC.path());
        Workspace wa(a.get(), dirA.path());
        Workspace wb(b.get(), dirB.path());
        Workspace wc(c.get(), dirC.path());
        wa.setLocalScreens(twoScreens("A"));
        QVERIFY(test::pairNodes(*a, *b));
        QVERIFY(test::pairNodes(*a, *c));
        a->setViewAllowed(b->id(), true);
        a->setViewAllowed(c->id(), true);
        QTRY_COMPARE(wc.screens(a->id()).size(), 2);
        QTest::qWait(5500); // the requests sent on connect/grant expire

        QSignalSpy requested(&wa, &Workspace::localThumbnailsRequested);
        wb.requestRemoteThumbnails();
        QTRY_COMPARE(requested.size(), 1);
        wa.setLocalThumbnail("A2", "for-b");
        QTRY_COMPARE(wb.thumbnail(a->id(), "A2"), QByteArray("for-b"));
        QTest::qWait(300);
        QVERIFY(wc.thumbnails(a->id()).isEmpty()); // granted, but did not ask
    }

    void oversizedOrUnknownThumbnailsAreIgnored()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        QVERIFY(p.start());
        QTRY_COMPARE(p.wb->screens(p.a->id()).size(), 2);
        // B asks for A's thumbnails on connecting and A refuses (no view grant), which drops the ones
        // B has: wait for that answer so it cannot arrive after the thumbnails sent below.
        QTRY_VERIFY(p.wb->thumbnailsDenied(p.a->id()));
        QCborMap unknown;
        unknown.insert(QStringLiteral("t"), QStringLiteral("thumb"));
        unknown.insert(QStringLiteral("s"), QStringLiteral("nope"));
        unknown.insert(QStringLiteral("d"), QByteArray("x"));
        p.a->sendMessage(p.b->id(), unknown);
        QCborMap big = unknown;
        big.insert(QStringLiteral("s"), QStringLiteral("A1"));
        big.insert(QStringLiteral("d"), QByteArray(300 * 1024, 'x'));
        p.a->sendMessage(p.b->id(), big);
        QCborMap ok = unknown;
        ok.insert(QStringLiteral("s"), QStringLiteral("A2"));
        p.a->sendMessage(p.b->id(), ok);
        // The oversized one may take a while over loopback UDP; "ok" queues behind it.
        QTRY_COMPARE_WITH_TIMEOUT(p.wb->thumbnail(p.a->id(), "A2"), QByteArray("x"), 30000);
        QCOMPARE(p.wb->thumbnails(p.a->id()).size(), 1);
    }

    void defaultLayoutPlacesDevicesSideBySide()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        p.wb->setLocalScreens(twoScreens("B"));
        QVERIFY(p.start());
        QTRY_COMPARE(p.wa->devices().size(), 2);
        const auto layout = p.wa->effectiveLayout();
        QCOMPARE(layout.value(p.a->id()), QPoint(0, 0));
        QCOMPARE(layout.value(p.b->id()), QPoint(1920 + 2560, 0)); // right of A's two screens
    }

    void layoutConvergesAfterConcurrentEdits()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        p.wb->setLocalScreens(twoScreens("B"));
        QVERIFY(p.start());

        // Both edit "at the same time": same version, the higher author id wins on both sides.
        QMap<QByteArray, QPoint> la{{p.a->id(), {0, 0}}, {p.b->id(), {-4480, 0}}};
        QMap<QByteArray, QPoint> lb{{p.a->id(), {0, 0}}, {p.b->id(), {0, 1080}}};
        p.wa->setLayout(la);
        p.wb->setLayout(lb);
        const auto expected = p.a->id() > p.b->id() ? la : lb;
        QTRY_COMPARE(p.wa->layout().offsets, expected);
        QTRY_COMPARE(p.wb->layout().offsets, expected);

        // A later edit wins everywhere.
        QMap<QByteArray, QPoint> later{{p.a->id(), {10, 10}}, {p.b->id(), {20, 20}}};
        p.wb->setLayout(later);
        QTRY_COMPARE(p.wa->layout().offsets, later);
    }

    void masterHandOver()
    {
        Pair p;
        QVERIFY(p.start());
        p.wa->setSharing(true);
        QTRY_COMPARE(p.wb->masterId(), p.a->id());
        p.wb->setSharing(true); // B takes over
        QTRY_COMPARE(p.wa->masterId(), p.b->id());
        p.wa->setSharing(false); // A is not the master: no effect
        QTest::qWait(200);
        QCOMPARE(p.wb->masterId(), p.b->id());
        p.wb->setSharing(false);
        QTRY_VERIFY(p.wa->masterId().isEmpty());
    }

    void stateIsPersisted()
    {
        QTemporaryDir dirA, dirB;
        QByteArray idA, idB;
        {
            auto a = test::makeNode(dirA.path());
            auto b = test::makeNode(dirB.path());
            Workspace wa(a.get(), dirA.path());
            Workspace wb(b.get(), dirB.path());
            wb.setLocalScreens(twoScreens("B"));
            QVERIFY(test::pairNodes(*a, *b));
            QTRY_COMPARE(wa.screens(b->id()).size(), 2);
            wa.setLayout({{a->id(), {0, 0}}, {b->id(), {0, -1440}}});
            wa.setSharing(true);
            idA = a->id();
            idB = b->id();
        }
        auto a = test::makeNode(dirA.path());
        Workspace wa(a.get(), dirA.path());
        QCOMPARE(wa.masterId(), idA);
        QCOMPARE(wa.layout().offsets.value(idB), QPoint(0, -1440));
        QCOMPARE(wa.screens(idB).size(), 2); // cached while B is offline
    }
};

QTEST_GUILESS_MAIN(TestWorkspace)
#include "tst_workspace.moc"
