// Keyboard diagnostics (KeyDiagnostics) between two paired nodes over loopback. Built only with
// -DDESKFLOWER_KEY_DIAGNOSTICS=ON (the helper's key injections: tst_keyinjection.cpp).

#include "inputcontroller.h"
#include "keydiagnostics.h"
#include "testutil.h"
#include "workspace.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
bool anyLineContains(const QStringList &lines, const QString &text)
{
    for (const QString &l : lines) {
        if (l.contains(text))
            return true;
    }
    return false;
}

struct Pair {
    QTemporaryDir dirA, dirB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<KeyDiagnostics> da, db;

    bool setUp(std::optional<TrustLevel> levelOnB = std::nullopt, bool diagOnB = true)
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        da = std::make_unique<KeyDiagnostics>(a.get());
        if (diagOnB)
            db = std::make_unique<KeyDiagnostics>(b.get());
        return test::pairNodes(*a, *b, std::nullopt, levelOnB);
    }
};
} // namespace

class TestKeyDiag : public QObject
{
    Q_OBJECT
private slots:
    void keyNames()
    {
        QCOMPARE(KeyDiagnostics::keyName(0xE05B), QStringLiteral("LWin"));
        QCOMPARE(KeyDiagnostics::keyName(0x20), QStringLiteral("D"));
        QVERIFY(KeyDiagnostics::keyName(0x1E).isEmpty());
        const QCborMap on = KeyDiagnostics::requestMessage(true, 5000);
        QCOMPARE(on.value(QStringLiteral("t")).toString(), QStringLiteral("diag.key"));
        QCOMPARE(on.value(QStringLiteral("on")).toBool(), true);
        QCOMPARE(on.value(QStringLiteral("ms")).toInteger(), 5000);
        QVERIFY(!KeyDiagnostics::requestMessage(false, 0).contains(QStringLiteral("ms")));
    }

    // Started on A: B records too, for A, and its lines reach A with B's name; keys forwarded and
    // received are described with the modifiers held; stopping on A stops B.
    void startingAsksTheConnectedDevices()
    {
        Pair p;
        QVERIFY(p.setUp());
        QSignalSpy activeB(p.db.get(), &KeyDiagnostics::activeChanged);
        p.da->start();
        QVERIFY(p.da->isActive());
        QVERIFY(p.da->requestedBy().isEmpty());
        QCOMPARE(p.da->asked(), QList<QByteArray>{p.b->id()});
        QVERIFY(p.da->remainingMs() > KeyDiagnostics::kDurationMs - 5000);
        QTRY_VERIFY(p.db->isActive());
        QCOMPARE(p.db->requestedBy(), p.a->id());
        QCOMPARE(activeB.size(), 1);

        p.db->addLine(QStringLiteral("helper: inject sc=0xE05B down"));
        QTRY_VERIFY(anyLineContains(p.da->lines(), QStringLiteral("helper: inject sc=0xE05B down")));
        QVERIFY(anyLineContains(p.da->lines(), QLatin1Char('[') + p.b->name() + QLatin1Char(']')));

        p.da->noteKey(p.b->id(), 0xE05B, true, true);
        p.da->noteKey(p.b->id(), 0x20, true, true);
        QVERIFY(anyLineContains(p.da->lines(), QStringLiteral("sent in.key sc=0xE05B (LWin) down")));
        QVERIFY(anyLineContains(p.da->lines(), QStringLiteral("sc=0x20 (D) down with LWin")));
        p.db->noteKey(p.a->id(), 0xE05B, false, false);
        QVERIFY(anyLineContains(p.db->lines(), QStringLiteral("received in.key sc=0xE05B (LWin) up")));

        p.da->stop();
        QVERIFY(!p.da->isActive());
        QTRY_VERIFY(!p.db->isActive());
        QVERIFY(anyLineContains(p.db->lines(), QStringLiteral("stopped by")));
        // B's last line, on the way when A stopped, still reaches A.
        const auto bEndedOnA = [&] {
            for (const QString &l : p.da->lines()) {
                if (l.contains(QLatin1Char('[') + p.b->name() + QStringLiteral("] ")) && l.contains(QStringLiteral("ended on ")))
                    return true;
            }
            return false;
        };
        QTRY_VERIFY(bEndedOnA());

        // Nothing is recorded once it stopped.
        const int lines = p.da->lines().size();
        p.da->addLine(QStringLiteral("late"));
        p.da->noteKey(p.b->id(), 0x20, true, true);
        QCOMPARE(p.da->lines().size(), lines);
    }

    // The window ends by itself on both sides; a peer cannot ask for longer than here.
    void itEndsWhenTheTimeIsUp()
    {
        Pair p;
        QVERIFY(p.setUp());
        p.da->setDurationMs(600);
        p.db->setDurationMs(400);
        QSignalSpy activeA(p.da.get(), &KeyDiagnostics::activeChanged);
        p.da->start();
        QTRY_VERIFY(p.db->isActive());
        QVERIFY(p.db->remainingMs() <= 400);
        QTRY_VERIFY_WITH_TIMEOUT(!p.da->isActive() && !p.db->isActive(), 5000);
        QCOMPARE(activeA.size(), 2);
        QCOMPARE(activeA.last().at(0).toBool(), false);
        QVERIFY(anyLineContains(p.da->lines(), QStringLiteral("time is up")));
        QCOMPARE(p.da->remainingMs(), 0);
    }

    // A device that declared the other a guest neither records for it nor asks it.
    void guestsAreNeitherAskedNorObeyed()
    {
        Pair p;
        QVERIFY(p.setUp(TrustLevel::Guest)); // B holds A as a guest
        QTRY_VERIFY(p.a->declaredTrust(p.b->id()).has_value());
        p.da->start();
        QVERIFY(p.da->asked().isEmpty()); // B declared A a guest: not asked
        QVERIFY(anyLineContains(p.da->lines(), QStringLiteral("no other device")));
        // Even sent anyway, B refuses it.
        p.a->sendMessage(p.b->id(), KeyDiagnostics::requestMessage(true, 5000));
        QTest::qWait(300);
        QVERIFY(!p.db->isActive());
        p.da->stop();

        // B asks A: A may record for B (A trusts B), but B does not ask a guest.
        p.db->start();
        QVERIFY(p.db->asked().isEmpty());
        p.db->stop();
    }

    // One run at a time; only the device that asked stops it; lines from a device not asked, or
    // other diag messages, are ignored.
    void oneRunAtATime()
    {
        Pair p;
        QVERIFY(p.setUp());
        p.db->start(); // B's own run
        QTRY_VERIFY(p.da->isActive()); // A records for B
        QCOMPARE(p.da->requestedBy(), p.b->id());
        // A's own start replaces it here (B still runs its own).
        p.da->start();
        QVERIFY(p.da->requestedBy().isEmpty());
        QTest::qWait(200);
        QVERIFY(p.db->isActive());
        QVERIFY(p.db->requestedBy().isEmpty()); // B's run, not taken over
        // A stopping does not stop B's own run (B did not run for A).
        p.da->stop();
        QTest::qWait(300);
        QVERIFY(p.db->isActive());

        // Unknown diag messages and malformed requests change nothing.
        QCborMap other;
        other.insert(QStringLiteral("t"), QStringLiteral("diag.other"));
        p.a->sendMessage(p.b->id(), other);
        QCborMap bad;
        bad.insert(QStringLiteral("t"), QStringLiteral("diag.key"));
        bad.insert(QStringLiteral("on"), QStringLiteral("yes"));
        p.a->sendMessage(p.b->id(), bad);
        QTest::qWait(300);
        QVERIFY(p.db->isActive());
        QVERIFY(p.db->requestedBy().isEmpty());
        p.db->stop();
    }

    // A device without diagnostics (an older version: only its own message types reach its
    // higher layers) is asked like any other and simply records nothing; the run here goes on.
    void anOlderPeerIgnoresTheRequest()
    {
        Pair p;
        QVERIFY(p.setUp(std::nullopt, false));
        // What B runs instead: the layers that take peer messages.
        Workspace wb(p.b.get(), p.dirB.path());
        InputController ib(p.b.get(), &wb);
        QSignalSpy toHelperB(&ib, &InputController::toSession);
        p.da->setDurationMs(500);
        p.da->start();
        QCOMPARE(p.da->asked().size(), 1);
        QTest::qWait(200);
        QVERIFY(p.a->isConnected(p.b->id()));
        QTRY_VERIFY_WITH_TIMEOUT(!p.da->isActive(), 5000);
        QVERIFY(p.a->isConnected(p.b->id()));
        QVERIFY(!anyLineContains(p.da->lines(), QLatin1Char('[') + p.b->name() + QLatin1Char(']')));
        QCOMPARE(toHelperB.size(), 0);
    }

    // Lines are one line each and bounded.
    void linesAreBounded()
    {
        QTemporaryDir dir;
        auto n = test::makeNode(dir.path());
        KeyDiagnostics d(n.get());
        d.start();
        d.addLine(QString(5000, QLatin1Char('x')) + QStringLiteral("\nsecond"));
        const QString last = d.lines().last();
        QVERIFY(!last.contains(QLatin1Char('\n')));
        QVERIFY(last.size() < KeyDiagnostics::kMaxLineLength + 20);
        d.addLine(QStringLiteral("helper: where: user SYSTEM"));
        for (int i = 0; i < KeyDiagnostics::kMaxLines + 10; ++i)
            d.addLine(QStringLiteral("l%1").arg(i));
        const QStringList lines = d.lines();
        QCOMPARE(lines.size(), KeyDiagnostics::kMaxLines);
        // The header (where it runs) is never dropped; a line says how many others were.
        QVERIFY(lines.at(0).contains(QStringLiteral("started on")));
        QVERIFY(anyLineContains(d.header(), QStringLiteral("helper: where: user")));
        QVERIFY(anyLineContains(lines, QStringLiteral("older lines dropped")));
        QVERIFY(lines.last().endsWith(QStringLiteral("l%1").arg(KeyDiagnostics::kMaxLines + 9)));
        QVERIFY(!anyLineContains(lines, QStringLiteral(" l0")));
        d.stop();
    }

    // A key held down (autorepeat) is one line down and one up, with the repeats counted; the
    // modifiers held are still shown with the next key.
    void autorepeatIsCounted()
    {
        QTemporaryDir dir;
        auto n = test::makeNode(dir.path());
        KeyDiagnostics d(n.get());
        d.start();
        const QByteArray peer("peer-b");
        const int before = d.lines().size();
        d.noteKey(peer, 0xE05B, true, true);
        for (int i = 0; i < 30; ++i)
            d.noteKey(peer, 0xE05B, true, true);
        d.noteKey(peer, 0x20, true, true);
        d.noteKey(peer, 0x20, true, true);
        d.noteKey(peer, 0x20, false, true);
        d.noteKey(peer, 0xE05B, false, true);
        const QStringList lines = d.lines();
        QCOMPARE(lines.size(), before + 4);
        QVERIFY(anyLineContains(lines, QStringLiteral("sc=0x20 (D) down with LWin")));
        QVERIFY(anyLineContains(lines, QStringLiteral("sc=0x20 (D) up (after 1 repeats)")));
        QVERIFY(anyLineContains(lines, QStringLiteral("sc=0xE05B (LWin) up (after 30 repeats)")));
        // Pressed again: a new line.
        d.noteKey(peer, 0x20, true, true);
        QCOMPARE(d.lines().size(), before + 5);
        // And a key not taken by the receiving side says why.
        d.noteIgnoredKey(peer, 0x20, true, QStringLiteral("not the master here"));
        QVERIFY(anyLineContains(d.lines(), QStringLiteral("ignored in.key sc=0x20 (D) down")));
        d.stop();
    }
};

QTEST_MAIN(TestKeyDiag)
#include "tst_keydiag.moc"
