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

    void aGuestCannotTakeTheKeyboardOrArrangeTheScreens()
    {
        Pair p;
        QVERIFY(p.start());
        p.a->setTrust(p.b->id(), TrustLevel::Guest); // B is a guest for A
        QMap<QByteArray, QPoint> mine{{p.a->id(), {0, 0}}, {p.b->id(), {1920, 0}}};
        p.wa->setLayout(mine);
        QTRY_COMPARE(p.wb->layout().offsets, mine); // the guest still learns the layout...
        p.wb->setLayout({{p.a->id(), {0, 0}}, {p.b->id(), {0, 1080}}});
        p.wb->setSharing(true);
        QTest::qWait(300);
        QCOMPARE(p.wa->layout().offsets, mine); // ...but cannot change it
        QVERIFY(p.wa->masterId().isEmpty());     // nor take the keyboard and mouse
    }

    // The guest hears why its edits do not travel: A answers its own layout edit and master claim
    // with ws.refused (at most once per kind every few seconds), which B reports (editRefused).
    void aGuestIsToldItsEditsAreRefused()
    {
        Pair p;
        QVERIFY(p.start());
        QSignalSpy refusedOnB(p.wb.get(), &Workspace::editRefused);
        QSignalSpy refusedOnA(p.wa.get(), &Workspace::editRefused);
        p.a->setTrust(p.b->id(), TrustLevel::Guest);
        p.wb->setLayout({{p.a->id(), {0, 0}}, {p.b->id(), {0, 1080}}});
        QTRY_COMPARE(refusedOnB.size(), 1);
        QCOMPARE(refusedOnB.first().at(0).toByteArray(), p.a->id());
        QCOMPARE(refusedOnB.first().at(1).toString(), QString("layout"));
        QCOMPARE(refusedOnB.first().at(2).toString(), QString("guest"));
        p.wb->setLayout({{p.a->id(), {0, 0}}, {p.b->id(), {0, -1080}}});
        p.wb->setSharing(true);
        QTRY_COMPARE(refusedOnB.size(), 2);
        QCOMPARE(refusedOnB.last().at(1).toString(), QString("master"));
        QTest::qWait(300);
        QCOMPARE(refusedOnB.size(), 2); // the second layout edit came too soon after the first
        QCOMPARE(refusedOnA.size(), 0);

        // A's own edits reach B as before, and are not refused.
        p.wa->setLayout({{p.a->id(), {0, 0}}, {p.b->id(), {1920, 0}}});
        QTRY_COMPARE(p.wb->layout().offsets.value(p.b->id()), QPoint(1920, 0));
        QTest::qWait(200);
        QCOMPARE(refusedOnA.size(), 0);
    }

    // ws.refused is believed only from a peer that declared this device a guest, and a flood of
    // them makes one notice per kind per kRefusedReplyMs on the receiving side too.
    void aRefusalIsBelievedOnlyFromWhereItCanComeFrom()
    {
        Pair p;
        QVERIFY(p.start());
        QSignalSpy refusedOnA(p.wa.get(), &Workspace::editRefused);
        QSignalSpy refusedOnB(p.wb.get(), &Workspace::editRefused);
        QCborMap refusal;
        refusal.insert(QStringLiteral("t"), QStringLiteral("ws.refused"));
        refusal.insert(QStringLiteral("k"), QStringLiteral("layout"));
        refusal.insert(QStringLiteral("r"), QStringLiteral("guest"));
        // B trusts A: a refusal from B means nothing.
        for (int i = 0; i < 3; ++i)
            QVERIFY(p.b->sendMessage(p.a->id(), refusal));
        // A made B a guest: B believes it, once.
        p.a->setTrust(p.b->id(), TrustLevel::Guest);
        QTRY_COMPARE(p.b->declaredTrust(p.a->id()), std::optional<TrustLevel>(TrustLevel::Guest));
        for (int i = 0; i < 5; ++i)
            QVERIFY(p.a->sendMessage(p.b->id(), refusal));
        refusal.insert(QStringLiteral("k"), QStringLiteral("master"));
        for (int i = 0; i < 5; ++i)
            QVERIFY(p.a->sendMessage(p.b->id(), refusal));
        QTRY_COMPARE(refusedOnB.size(), 2);
        QTest::qWait(300);
        QCOMPARE(refusedOnB.size(), 2);
        QCOMPARE(refusedOnB.at(0).at(1).toString(), QString("layout"));
        QCOMPARE(refusedOnB.at(1).at(1).toString(), QString("master"));
        QCOMPARE(refusedOnA.size(), 0);
    }

    // A guest of this device must not become its master (nor rewrite its layout) through a third
    // device that trusts the guest and relays its claim.
    void aGuestCannotBecomeMasterThroughAThirdDevice()
    {
        QTemporaryDir dirA, dirB, dirC;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto c = test::makeNode(dirC.path());
        Workspace wa(a.get(), dirA.path());
        Workspace wb(b.get(), dirB.path());
        Workspace wc(c.get(), dirC.path());
        // A pairs B as a guest; C pairs B normally; A and C are paired normally.
        QVERIFY(test::pairNodes(*a, *b, TrustLevel::Guest));
        QVERIFY(test::pairNodes(*c, *b));
        QVERIFY(test::pairNodes(*a, *c));
        QVERIFY(a->isGuest(b->id()));

        wa.setLayout({{a->id(), {0, 0}}, {c->id(), {1920, 0}}});
        const auto mine = wa.layout().offsets;

        // B shares and arranges the screens: C adopts and relays both to A.
        wb.setSharing(true);
        wb.setLayout({{a->id(), {0, 0}}, {b->id(), {-1920, 0}}});
        QTRY_COMPARE(wc.masterId(), b->id());         // C, which trusts B, does adopt it
        QTest::qWait(500);                            // let any relay reach A
        QVERIFY2(wa.masterId().isEmpty(), wa.masterId().toHex());  // but A never accepts a guest master
        QCOMPARE(wa.layout().offsets, mine);          // nor a layout a guest authored
    }

    void aLocalEditOutranksTheVersionsAGuestWrote()
    {
        // A pairs G as a guest, B in full: B takes G's registers, A drops them. A's next edit must
        // still win on B (last writer wins is strict on the version), or the two would keep
        // different layouts and masters for good.
        QTemporaryDir dirA, dirB, dirG;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto g = test::makeNode(dirG.path());
        Workspace wa(a.get(), dirA.path());
        Workspace wb(b.get(), dirB.path());
        Workspace wg(g.get(), dirG.path());
        QVERIFY(test::pairNodes(*a, *g, TrustLevel::Guest));
        QVERIFY(test::pairNodes(*b, *g));
        QVERIFY(test::pairNodes(*a, *b));

        // G arranges twice (version 2) and shares, stops and shares again (version 3).
        wg.setLayout({{g->id(), {0, 0}}, {a->id(), {1920, 0}}});
        wg.setLayout({{g->id(), {0, 0}}, {a->id(), {-1920, 0}}});
        wg.setSharing(true);
        wg.setSharing(false);
        wg.setSharing(true);
        QTRY_COMPARE(wb.layout().stamp.author, g->id());
        QTRY_COMPARE(wb.masterId(), g->id());
        QTest::qWait(500); // relays reach A
        QVERIFY(wa.layout().stamp.author.isEmpty());
        QVERIFY(wa.masterId().isEmpty());

        // A's edits are stamped above what it refused: B adopts them.
        const QMap<QByteArray, QPoint> mine = {{a->id(), {0, 0}}, {b->id(), {1920, 0}}};
        wa.setLayout(mine);
        wa.setSharing(true);
        QTRY_COMPARE(wb.layout().stamp.author, a->id());
        QCOMPARE(wb.layout().offsets, mine);
        QTRY_COMPARE(wb.masterId(), a->id());
        QCOMPARE(wa.masterId(), a->id());
    }

    void viewPlansStayLocalAndFollowThePairing()
    {
        Pair p;
        p.wa->setLocalScreens(twoScreens("A"));
        p.wb->setLocalScreens(twoScreens("B"));
        QVERIFY(p.start());
        QTRY_COMPARE(p.wa->screens(p.b->id()).size(), 2);
        QSignalSpy changed(p.wa.get(), &Workspace::changed);
        QSignalSpy toB(p.b.get(), &Node::messageReceived);
        const Workspace::ViewPlan plan = {{"B1", "A2"}, {"B2", Workspace::kHiddenScreen}};
        // changed() also follows every peersChanged(): only the calls here are counted.
        qsizetype seen = changed.size();
        p.wa->setViewPlan(p.b->id(), plan);
        QCOMPARE(p.wa->viewPlan(p.b->id()), plan);
        QCOMPARE(changed.size(), seen + 1);
        seen = changed.size();
        p.wa->setViewPlan(p.b->id(), plan); // unchanged: nothing to tell
        QCOMPARE(changed.size(), seen);

        // Nothing of it reaches B (it names A's monitors): not on its own, nor with the layout.
        p.wa->setLayout({{p.a->id(), {0, 0}}, {p.b->id(), {0, 1080}}});
        QTRY_COMPARE(p.wb->layout().offsets.value(p.b->id()), QPoint(0, 1080));
        QVERIFY(!toB.isEmpty());
        for (const QList<QVariant> &args : toB) {
            const QCborMap m = args.at(1).value<QCborMap>();
            QVERIFY(!QCborValue(m).toCbor().contains("viewPlan"));
            if (m.value(QStringLiteral("t")).toString() != QLatin1String("layout"))
                continue;
            for (auto it = m.constBegin(); it != m.constEnd(); ++it)
                QVERIFY2(QStringList({"t", "v", "a", "p", "rv"}).contains(it.key().toString()), qPrintable(it.key().toString()));
        }
        QVERIFY(p.wb->viewPlan(p.a->id()).isEmpty());

        // Not for this device itself, a stranger, or with too many entries; an empty one removes.
        p.wa->setViewPlan(p.a->id(), plan);
        QVERIFY(p.wa->viewPlan(p.a->id()).isEmpty());
        p.wa->setViewPlan("nobody", plan);
        QVERIFY(p.wa->viewPlan("nobody").isEmpty());
        Workspace::ViewPlan huge;
        for (int i = 0; i <= Workspace::kMaxViewPlanEntries; ++i)
            huge.insert(QStringLiteral("s%1").arg(i), QStringLiteral("A1"));
        seen = changed.size();
        p.wa->setViewPlan(p.b->id(), huge);
        QCOMPARE(p.wa->viewPlan(p.b->id()), plan);
        QCOMPARE(changed.size(), seen);
        p.wa->setViewPlan(p.b->id(), {});
        QVERIFY(p.wa->viewPlan(p.b->id()).isEmpty());
        QCOMPARE(changed.size(), seen + 1);

        // Persisted: a fresh Workspace reads it back.
        p.wa->setViewPlan(p.b->id(), plan);
        {
            Workspace again(p.a.get(), p.dirA.path());
            QCOMPARE(again.viewPlan(p.b->id()), plan);
        }

        // Dropped, also from disk, once B is no longer paired.
        p.a->unpair(p.b->id());
        QTRY_VERIFY(p.wa->viewPlan(p.b->id()).isEmpty());
        Workspace afterwards(p.a.get(), p.dirA.path());
        QVERIFY(afterwards.viewPlan(p.b->id()).isEmpty());
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
