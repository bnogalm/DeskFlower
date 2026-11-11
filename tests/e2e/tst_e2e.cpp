// End-to-end tests: real deskflower-service processes driven over their IPC (e2eharness.h).
//
//   tst_e2e cli     the command line of deskflower-service (no blocking dialogs)
//   tst_e2e direct  two devices on loopback (a third joins for the group test)
//   tst_e2e nat     the same, with B behind a simulated home router (NatRouter)
//
// Without a mode all three run. The end of each service's log is part of the failure message; with
// DF_E2E_KEEP=1 the work folder (whole logs, received files) stays when a test fails.
//
// Behaviour that is documented as intended but implemented on another branch is checked with
// E2E_EXPECT(feature, ...): while the feature is listed in kPendingFeatures a failure is reported
// as a skip with the reason, a success as a pass with a reminder to drop it from the list.

#include "e2eharness.h"

#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace df::e2e;

namespace {

struct PendingFeature {
    const char *name;
    const char *reason;
};
// Intended behaviour (README) that this tree does not have yet. None now: trusted-paste-ungated
// (Device and Group paste without paste evidence) came with the clipboard work.
const std::vector<PendingFeature> kPendingFeatures;

const PendingFeature *pendingFeature(const char *name)
{
    for (const PendingFeature &f : kPendingFeatures) {
        if (qstrcmp(f.name, name) == 0)
            return &f;
    }
    return nullptr;
}

} // namespace

#define E2E_VERIFY(cond, what)                                                                              \
    do {                                                                                                    \
        if (!(cond))                                                                                        \
            QVERIFY2(false, qPrintable(QStringLiteral("%1\n%2").arg(what, diagnostics())));                 \
    } while (false)

#define E2E_EXPECT(feature, cond, what)                                                                     \
    do {                                                                                                    \
        const bool ok_ = (cond);                                                                            \
        const PendingFeature *pending_ = pendingFeature(feature);                                           \
        if (!ok_ && pending_)                                                                               \
            QSKIP(qPrintable(QStringLiteral("pending feature '%1': %2 (%3)").arg(feature, pending_->reason, what))); \
        if (ok_ && pending_)                                                                                \
            qWarning("pending feature '%s' works now: remove it from kPendingFeatures", feature);           \
        E2E_VERIFY(ok_, what);                                                                              \
    } while (false)

// --- deskflower-service command line ----------------------------------------------------------------

class ServiceCli : public QObject
{
    Q_OBJECT
private slots:
    void versionPrintsAndExits()
    {
        QProcess p;
        p.start(serviceExecutable(), {QStringLiteral("--version")});
        QVERIFY2(p.waitForFinished(10000), "deskflower-service --version did not exit");
        QCOMPARE(p.exitCode(), 0);
        const QByteArray out = p.readAllStandardOutput();
        QVERIFY2(out.startsWith("deskflower ") && out.trimmed().size() > 11, out.constData());
    }

    void versionDoesNotBlockWithoutConsole()
    {
#ifdef Q_OS_WIN
        // Should it regress, a message box would open on the user's desktop: opt-in only.
        if (qgetenv("DESKFLOWER_DESKTOP_TESTS") != "1")
            QSKIP("a regression shows a dialog on the desktop: set DESKFLOWER_DESKTOP_TESTS=1 (docs/TESTING.md)");
        // Started detached, with no console and no redirected handles (as a scheduler or a script
        // without a console does): Qt would show the version in a message box and wait for a click.
        QProcess p;
        p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= DETACHED_PROCESS;
            args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
            args->inheritHandles = false;
        });
        p.start(serviceExecutable(), {QStringLiteral("--version")});
        QVERIFY(p.waitForStarted(10000));
        const bool exited = p.waitForFinished(10000);
        if (!exited)
            p.kill();
        QVERIFY2(exited, "deskflower-service --version blocked (a message box?) without a console");
        QCOMPARE(p.exitCode(), 0);
#else
        QSKIP("Only Windows shows command-line messages in a dialog");
#endif
    }
};

// --- Two (or three) devices -------------------------------------------------------------------------

class EndToEnd : public QObject
{
    Q_OBJECT
public:
    explicit EndToEnd(bool nat) : m_nat(nat) {}

private:
    // B as A reaches it: directly, or through the router's port forward.
    QString bHost() const { return m_nat ? m_router->wanAddress().toString() : QStringLiteral("127.0.0.1"); }
    quint16 bPort() const { return m_nat ? m_router->wanPort() : m_b->dataPort; }

    QString diagnostics() const
    {
        QString out = QStringLiteral("(work folder: %1)").arg(m_work.path());
        for (Service *s : {m_a.get(), m_b.get(), m_c.get()}) {
            if (s)
                out += QLatin1Char('\n') + s->diagnostics();
        }
        return out;
    }

    bool startB(QString *error)
    {
        if (!m_b->start(m_bPort, error))
            return false;
        m_bPort = m_b->dataPort; // the same port after a restart: the router forwards to it
        if (m_router) {
            m_router->setInsidePort(m_b->dataPort);
            return m_router->forwardTcp(m_b->tcpPort, error);
        }
        return true;
    }

    // A device outside the router (A, C): B reaches its file streams through the router.
    bool startOutside(Service &s, quint16 port, QString *error)
    {
        return s.start(port, error) && (!m_router || m_router->masqueradeTcp(s.tcpPort, error));
    }

    // `asker` pairs with `answerer` by PIN (the asker types the PIN the answerer shows). Levels are
    // what each side makes of the other ("device", "group", "guest").
    bool pair(Service &asker, Service &answerer, const QString &host, quint16 port, const QString &askerLevel,
              const QString &answererLevel, QString *why, QString *suggested = nullptr)
    {
        answerer.agent.discard(QStringLiteral("pairPin"));
        asker.agent.discard(QStringLiteral("pairPrompt"));
        asker.agent.discard(QStringLiteral("pairResult"));
        asker.agent.send(QStringLiteral("addPeer"), QCborMap{{QStringLiteral("host"), host}, {QStringLiteral("port"), port}});
        if (!asker.waitState([&](const QCborMap &s) { return !Service::peerIn(s, answerer.idHex).isEmpty(); }, 10000)) {
            *why = QStringLiteral("%1 does not see %2 after addPeer %3:%4").arg(asker.label(), answerer.label(), host).arg(port);
            return false;
        }
        asker.agent.send(QStringLiteral("pair"), QCborMap{{QStringLiteral("id"), answerer.idHex}, {QStringLiteral("trust"), askerLevel}});
        const auto pin = answerer.agent.waitEvent(QStringLiteral("pairPin"), 10000);
        if (!pin) {
            *why = QStringLiteral("%1 shows no PIN").arg(answerer.label());
            return false;
        }
        if (suggested)
            *suggested = pin->value(QStringLiteral("trust")).toString();
        answerer.agent.send(QStringLiteral("setPairTrust"),
                            QCborMap{{QStringLiteral("id"), asker.idHex}, {QStringLiteral("trust"), answererLevel}});
        if (!asker.agent.waitEvent(QStringLiteral("pairPrompt"), 10000)) {
            *why = QStringLiteral("%1 is not asked for the PIN").arg(asker.label());
            return false;
        }
        asker.agent.send(QStringLiteral("pin"), QCborMap{{QStringLiteral("id"), answerer.idHex},
                                                         {QStringLiteral("pin"), pin->value(QStringLiteral("pin"))}});
        const auto result = asker.agent.waitEvent(QStringLiteral("pairResult"), 15000);
        if (!result || !result->value(QStringLiteral("ok")).toBool()) {
            *why = QStringLiteral("pairing failed: %1")
                       .arg(result ? result->value(QStringLiteral("error")).toString() : QStringLiteral("no result"));
            return false;
        }
        if (!asker.waitConnected(answerer.idHex, 15000) || !answerer.waitConnected(asker.idHex, 15000)) {
            *why = QStringLiteral("%1 and %2 paired but did not connect").arg(asker.label(), answerer.label());
            return false;
        }
        return true;
    }

    // The shared cursor goes from A (the master) to B through A's right edge.
    bool crossToB()
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 8000) {
            // The fake helper pushes against the right edge; a crossing just after a return is
            // refused for a moment (return guard), so push again until it goes.
            m_a->helper.send(QStringLiteral("edge"), QCborMap{{QStringLiteral("s"), 1},
                                                              {QStringLiteral("x"), Service::kScreenWidth - 1},
                                                              {QStringLiteral("y"), Service::kScreenHeight / 2}});
            if (m_a->waitState([&](const QCborMap &) { return m_a->activeDevice() == m_b->idHex; }, 400))
                return true;
        }
        return false;
    }
    // Back to A through B's left edge (the master moves the cursor it routes there).
    bool returnToA()
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 8000) {
            m_a->helper.send(QStringLiteral("mm"), QCborMap{{QStringLiteral("x"), -2 * Service::kScreenWidth},
                                                            {QStringLiteral("y"), 0}});
            if (m_a->waitState([&](const QCborMap &) { return m_a->activeDevice() == m_a->idHex; }, 400))
                return true;
        }
        return false;
    }

    // A clipboard owner (the helper here, in console mode) received sealed content.
    std::optional<QCborMap> waitClipSet(Service &s, const std::function<bool(const QCborMap &)> &pred, int timeoutMs = 10000)
    {
        return s.helper.waitEvent(QStringLiteral("clipSet"), timeoutMs, pred);
    }

    // The user pressed a paste chord on the keyboard of `s` just before pasting, as the helper
    // reports it: local presses are seen there (localPressSupport, idempotent) and one was a paste
    // gesture (localPress). The helper and the paster use different connections, which the service
    // may read in any order: the paste request goes a moment later, as for a real user.
    static void reportActivity(Service &s)
    {
        s.helper.send(QStringLiteral("localPressSupport"), QCborMap{{QStringLiteral("on"), true}});
        s.helper.send(QStringLiteral("localPress"));
        QTest::qWait(150);
    }

    // Pastes one format on `s` the way its clipboard owner does it: evidence of the user's own
    // input there (localPress), then clipUnseal on a connection of its own.
    std::optional<QCborMap> unseal(Service &s, const QByteArray &kid, const QString &mime, bool withActivity)
    {
        if (withActivity)
            reportActivity(s);
        s.agent.discard(QStringLiteral("clipUnsealed"));
        s.agent.send(QStringLiteral("clipUnseal"), QCborMap{{QStringLiteral("kid"), hex(kid)}, {QStringLiteral("mime"), mime}});
        return s.agent.waitEvent(QStringLiteral("clipUnsealed"), 10000);
    }

    std::optional<QCborMap> fetch(Service &s, bool withActivity)
    {
        if (withActivity)
            reportActivity(s);
        s.agent.discard(QStringLiteral("clipFetched"));
        s.agent.send(QStringLiteral("clipFetch"));
        return s.agent.waitEvent(QStringLiteral("clipFetched"), 60000);
    }

    static QStringList stringList(const QCborValue &v)
    {
        QStringList out;
        for (const QCborValue &item : v.toArray())
            out << item.toString();
        return out;
    }

    // A tree with nested and empty folders, a folder holding only empty folders, Unicode names, a
    // larger binary file and a file next to the folders. Returns the top-level items.
    QStringList makeTree(const QString &root, quint32 seed)
    {
        const QString folder = QDir(root).filePath(QStringLiteral("Carpeta ñ 日本"));
        writeFile(folder + QStringLiteral("/sub/deeper/hoja €.txt"), QByteArrayLiteral("hoja"));
        writeFile(folder + QStringLiteral("/sub/data.bin"), randomBytes(300 * 1024, seed));
        writeFile(folder + QStringLiteral("/vacío.txt"), QByteArray());
        QDir().mkpath(folder + QStringLiteral("/vacía"));
        QDir().mkpath(folder + QStringLiteral("/sub/otra vacía"));
        const QString onlyFolders = QDir(root).filePath(QStringLiteral("Solo carpetas"));
        QDir().mkpath(onlyFolders + QStringLiteral("/a/b"));
        QDir().mkpath(onlyFolders + QStringLiteral("/c"));
        const QString single = QDir(root).filePath(QStringLiteral("suelto ü.txt"));
        writeFile(single, randomBytes(1234, seed + 1));
        return {QDir::toNativeSeparators(folder), QDir::toNativeSeparators(onlyFolders), QDir::toNativeSeparators(single)};
    }

    // Copies `items` on `from` (A with the cursor home, or B with the cursor there), carries the
    // clipboard to `to` with the cursor, pastes them there and compares the trees.
    void copyFilesAndPaste(Service &from, Service &to, const QStringList &items)
    {
        to.helper.discard(QStringLiteral("clipSet"));
        to.agent.discard(QStringLiteral("transfer"));
        from.helper.send(QStringLiteral("clip"), QCborMap{{QStringLiteral("f"), QCborMap()},
                                                          {QStringLiteral("files"), QCborArray::fromStringList(items)}});
        QTest::qWait(200);
        if (&from == m_a.get())
            E2E_VERIFY(crossToB(), QStringLiteral("the cursor did not cross to B"));
        else
            E2E_VERIFY(returnToA(), QStringLiteral("the cursor did not come back to A"));
        const auto set = waitClipSet(to, [](const QCborMap &m) {
            return m.value(QStringLiteral("pendingFiles")).toInteger() > 0;
        });
        E2E_VERIFY(set, QStringLiteral("%1 got no clipboard with files").arg(to.label()));
        // pendingFiles counts files and folders; the list has a folder as [path, -1].
        const QMap<QString, QString> want = snapshot(items);
        const QCborArray listed = set->value(QStringLiteral("pendingList")).toArray();
        QSet<QString> listedDirs;
        for (const QCborValue &v : listed) {
            if (v.toArray().at(1).toInteger() < 0)
                listedDirs.insert(v.toArray().at(0).toString());
        }
        E2E_VERIFY(set->value(QStringLiteral("pendingFiles")).toInteger() == want.size(),
                   QStringLiteral("pendingFiles %1, expected %2 (files and folders)")
                       .arg(set->value(QStringLiteral("pendingFiles")).toInteger())
                       .arg(want.size()));
        E2E_VERIFY(listedDirs.contains(QStringLiteral("Solo carpetas/a/b")) && listedDirs.contains(QStringLiteral("Carpeta ñ 日本/vacía")),
                   QStringLiteral("empty folders missing from the list: %1").arg(QStringList(listedDirs.values()).join(QStringLiteral(", "))));

        const auto fetched = fetch(to, true);
        E2E_VERIFY(fetched && fetched->value(QStringLiteral("ok")).toBool(),
                   QStringLiteral("clipFetch failed: %1").arg(fetched ? fetched->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        const QStringList got = stringList(fetched->value(QStringLiteral("files")));
        const QMap<QString, QString> have = snapshot(got);
        E2E_VERIFY(have == want, QStringLiteral("the pasted tree differs:\n%1").arg(snapshotDiff(want, have)));
        for (const QString &p : got) {
            E2E_VERIFY(QDir::fromNativeSeparators(p).startsWith(to.receivedFolder()),
                       QStringLiteral("pasted outside the received folder: %1").arg(p));
        }
        // The transfer is shown to the user: progress, then done (kind "clipboard").
        const auto done = to.agent.waitEvent(QStringLiteral("transfer"), 5000, [](const QCborMap &m) {
            return m.value(QStringLiteral("state")).toString() != QLatin1String("running");
        });
        E2E_VERIFY(done && done->value(QStringLiteral("state")).toString() == QLatin1String("done")
                       && done->value(QStringLiteral("kind")).toString() == QLatin1String("clipboard")
                       && done->value(QStringLiteral("peer")).toString() == from.idHex,
                   QStringLiteral("no finished clipboard transfer event"));
    }

private slots:
    void initTestCase()
    {
        if (m_nat && !NatRouter::available())
            QSKIP("127.0.0.2/127.0.0.3 cannot be bound here (macOS: add them as lo0 aliases)");
        QVERIFY2(QFileInfo::exists(serviceExecutable()), qPrintable(serviceExecutable() + QStringLiteral(" not found")));
        QVERIFY(m_work.isValid());
        m_a = std::make_unique<Service>(QStringLiteral("A"), m_work.path());
        m_b = std::make_unique<Service>(QStringLiteral("B"), m_work.path());
        QString error;
        // B keeps its port across restarts: the router (NAT run) forwards to it, and A finds it
        // there again.
        m_bPort = freePort();
        QVERIFY2(m_a->start(freePort(), &error), qPrintable(error + diagnostics()));
        QVERIFY2(startB(&error), qPrintable(error + diagnostics()));
        if (m_nat) {
            m_router = std::make_unique<NatRouter>();
            QVERIFY2(m_router->start(m_b->dataPort, &error), qPrintable(error));
            QVERIFY2(m_router->forwardTcp(m_b->tcpPort, &error) && m_router->masqueradeTcp(m_a->tcpPort, &error),
                     qPrintable(error));
        }
    }

    void cleanupTestCase()
    {
        for (Service *s : {m_c.get(), m_b.get(), m_a.get()}) {
            if (s)
                s->stop();
        }
    }

    void init()
    {
        // Each test starts with the cursor home, whatever the previous one left.
        if (m_crossing && m_a->isRunning() && m_a->activeDevice(true) != m_a->idHex)
            returnToA();
    }

    void cleanup()
    {
        // The end of every log is in the failure message; the whole folder only on request.
        if (QTest::currentTestFailed() && qEnvironmentVariable("DF_E2E_KEEP") == QLatin1String("1"))
            m_work.setAutoRemove(false);
    }

    void pairingByPin()
    {
        QString why, suggested;
        E2E_VERIFY(pair(*m_a, *m_b, bHost(), bPort(), QStringLiteral("device"), QStringLiteral("device"), &why, &suggested), why);
        // The PIN dialog offers a level (a new device: "device", never a silent upgrade).
        E2E_VERIFY(suggested == QLatin1String("device"), QStringLiteral("PIN dialog suggests '%1'").arg(suggested));
        const QCborMap ab = m_a->peer(m_b->idHex), ba = m_b->peer(m_a->idHex);
        E2E_VERIFY(ab.value(QStringLiteral("trusted")).toBool() && ba.value(QStringLiteral("trusted")).toBool(),
                   QStringLiteral("not trusted on both sides"));
        E2E_VERIFY(ab.value(QStringLiteral("effectiveTrust")).toString() == QLatin1String("device")
                       && ba.value(QStringLiteral("effectiveTrust")).toString() == QLatin1String("device"),
                   QStringLiteral("effective trust %1 / %2").arg(ab.value(QStringLiteral("effectiveTrust")).toString(),
                                                                  ba.value(QStringLiteral("effectiveTrust")).toString()));
        if (m_nat) {
            // B sees A at the router's LAN address (masquerade), A sees B at the port forward.
            E2E_VERIFY(ba.value(QStringLiteral("address")).toString() == m_router->lanAddress().toString(),
                       QStringLiteral("B sees A at %1").arg(ba.value(QStringLiteral("address")).toString()));
            E2E_VERIFY(ab.value(QStringLiteral("address")).toString() == m_router->wanAddress().toString()
                           && ab.value(QStringLiteral("port")).toInteger() == m_router->wanPort(),
                       QStringLiteral("A sees B at %1:%2").arg(ab.value(QStringLiteral("address")).toString()).arg(ab.value(QStringLiteral("port")).toInteger()));
        }
        m_paired = true;
    }

    void cursorCrossesAndReturns()
    {
        if (!m_paired)
            QSKIP("needs pairingByPin");
        m_a->agent.send(QStringLiteral("setSharing"), QCborMap{{QStringLiteral("on"), true}});
        m_a->agent.send(QStringLiteral("setLayout"),
                        QCborMap{{QStringLiteral("packs"),
                                  QCborArray{QCborMap{{QStringLiteral("id"), m_a->idHex}, {QStringLiteral("x"), 0}, {QStringLiteral("y"), 0}},
                                             QCborMap{{QStringLiteral("id"), m_b->idHex}, {QStringLiteral("x"), Service::kScreenWidth}, {QStringLiteral("y"), 0}}}}});
        auto knowsBoth = [&](Service &s) {
            return s.waitState([&](const QCborMap &st) {
                const QCborMap ws = st.value(QStringLiteral("workspace")).toMap();
                int withScreens = 0;
                for (const QCborValue &d : ws.value(QStringLiteral("devices")).toArray())
                    withScreens += d.toMap().value(QStringLiteral("screens")).toArray().isEmpty() ? 0 : 1;
                return ws.value(QStringLiteral("master")).toString() == m_a->idHex && withScreens == 2;
            }, 10000).has_value();
        };
        E2E_VERIFY(knowsBoth(*m_a) && knowsBoth(*m_b), QStringLiteral("the workspace (master, screens) did not replicate"));

        m_a->helper.discard(QStringLiteral("capture"));
        E2E_VERIFY(crossToB(), QStringLiteral("the cursor did not cross to B"));
        // A's helper captures the keyboard and mouse while the cursor is away.
        const auto capture = m_a->helper.waitEvent(QStringLiteral("capture"), 3000, [](const QCborMap &m) {
            return m.value(QStringLiteral("on")).toBool();
        });
        E2E_VERIFY(capture, QStringLiteral("A's helper was not told to capture"));
        E2E_VERIFY(returnToA(), QStringLiteral("the cursor did not come back to A"));
        m_crossing = true;
    }

    void textClipboardTravelsWithTheCursor()
    {
        if (!m_crossing)
            QSKIP("needs cursorCrossesAndReturns");
        // Copied on A (the cursor is home), pasted on B.
        const QByteArray text = "hola desde A — ñ 日本";
        m_b->helper.discard(QStringLiteral("clipSet"));
        m_a->helper.send(QStringLiteral("clip"),
                         QCborMap{{QStringLiteral("f"), QCborMap{{QStringLiteral("text/plain"), text},
                                                                 {QStringLiteral("text/html"), QByteArray("<b>hola</b>")}}}});
        QTest::qWait(200);
        E2E_VERIFY(crossToB(), QStringLiteral("the cursor did not cross to B"));
        const auto set = waitClipSet(*m_b, [](const QCborMap &m) { return m.contains(QStringLiteral("sealed")); });
        E2E_VERIFY(set, QStringLiteral("B's clipboard owner got no sealed clipboard"));
        // Only names and sizes travel to the owner; the plaintext comes with the paste.
        QMap<QString, qint64> formats;
        for (const QCborValue &v : set->value(QStringLiteral("sealed")).toArray())
            formats.insert(v.toArray().at(0).toString(), v.toArray().at(1).toInteger());
        // (An adopted format reports its ciphertext size: the plaintext size is not known there.)
        E2E_VERIFY(formats.value(QStringLiteral("text/plain")) >= text.size() && formats.contains(QStringLiteral("text/html")),
                   QStringLiteral("sealed formats: %1").arg(QStringList(formats.keys()).join(QLatin1Char(','))));
        const QByteArray kid = set->value(QStringLiteral("kid")).toByteArray();
        const auto plain = unseal(*m_b, kid, QStringLiteral("text/plain"), true);
        E2E_VERIFY(plain && plain->value(QStringLiteral("ok")).toBool(),
                   QStringLiteral("paste refused: %1").arg(plain ? plain->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        E2E_VERIFY(plain->value(QStringLiteral("data")).toByteArray() == text, QStringLiteral("pasted text differs"));
        const auto html = unseal(*m_b, kid, QStringLiteral("text/html"), false); // the same paste (latch)
        E2E_VERIFY(html && html->value(QStringLiteral("data")).toByteArray() == "<b>hola</b>", QStringLiteral("pasted HTML differs"));

        // Copied on B (the cursor is there), pasted on A after the cursor comes back.
        const QByteArray back = "y de vuelta desde B";
        m_a->helper.discard(QStringLiteral("clipSet"));
        m_b->helper.send(QStringLiteral("clip"), QCborMap{{QStringLiteral("f"), QCborMap{{QStringLiteral("text/plain"), back}}}});
        QTest::qWait(200);
        E2E_VERIFY(returnToA(), QStringLiteral("the cursor did not come back to A"));
        const auto setA = waitClipSet(*m_a, [](const QCborMap &m) { return m.contains(QStringLiteral("sealed")); });
        E2E_VERIFY(setA, QStringLiteral("A's clipboard owner got no sealed clipboard from B"));
        const auto plainA = unseal(*m_a, setA->value(QStringLiteral("kid")).toByteArray(), QStringLiteral("text/plain"), true);
        E2E_VERIFY(plainA && plainA->value(QStringLiteral("ok")).toBool() && plainA->value(QStringLiteral("data")).toByteArray() == back,
                   QStringLiteral("paste on A failed: %1").arg(plainA ? plainA->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        m_textKid = setA->value(QStringLiteral("kid")).toByteArray();
    }

    void trustedDevicePastesWithoutPasteEvidence()
    {
        if (m_textKid.isEmpty())
            QSKIP("needs textClipboardTravelsWithTheCursor");
        // A fresh copy on A (text and a file) travels to B; B pastes it with no input of its own
        // shortly before (a paste from the context menu, or any reader): for a trusted device the
        // content simply decrypts (user decision; guests still need the evidence). The file is
        // fetched only once a press is reported (0.1.30: file data moves only for a paste).
        const QByteArray text = "sin evidencia";
        const QString file = QDir(m_a->home()).filePath(QStringLiteral("ungated/nota.txt"));
        writeFile(file, QByteArrayLiteral("nota"));
        m_b->helper.discard(QStringLiteral("clipSet"));
        m_a->helper.send(QStringLiteral("clip"), QCborMap{{QStringLiteral("f"), QCborMap{{QStringLiteral("text/plain"), text}}},
                                                          {QStringLiteral("files"), QCborArray{QDir::toNativeSeparators(file)}}});
        QTest::qWait(200);
        E2E_VERIFY(crossToB(), QStringLiteral("the cursor did not cross to B"));
        const auto set = waitClipSet(*m_b, [](const QCborMap &m) { return m.contains(QStringLiteral("sealed")); });
        E2E_VERIFY(set, QStringLiteral("B got no clipboard"));
        QTest::qWait(2500); // past every activity window of this tree
        const auto plain = unseal(*m_b, set->value(QStringLiteral("kid")).toByteArray(), QStringLiteral("text/plain"), false);
        const bool pasted = plain && plain->value(QStringLiteral("ok")).toBool() && plain->value(QStringLiteral("data")).toByteArray() == text;
        // Copied files move only for a paste, on a trusted device too (0.1.30): a read with no key
        // or click just before is refused quietly, and the paste that follows gets them.
        const auto refused = fetch(*m_b, false);
        const bool quiet = refused && !refused->value(QStringLiteral("ok")).toBool()
                           && refused->value(QStringLiteral("code")).toString() == QLatin1String("noPaste");
        const auto fetched = fetch(*m_b, true);
        const bool gotFile = fetched && fetched->value(QStringLiteral("ok")).toBool()
                             && snapshot(stringList(fetched->value(QStringLiteral("files")))) == snapshot(file);
        E2E_VERIFY(returnToA(), QStringLiteral("the cursor did not come back to A"));
        E2E_VERIFY(pasted,
                   QStringLiteral("paste refused: %1").arg(plain ? plain->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        E2E_VERIFY(quiet, QStringLiteral("a file read with no paste was not refused as such: %1")
                              .arg(refused ? refused->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        E2E_VERIFY(gotFile,
                   QStringLiteral("file paste refused: %1").arg(fetched ? fetched->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
    }

    void filesAndFoldersByClipboard()
    {
        if (!m_crossing)
            QSKIP("needs cursorCrossesAndReturns");
        // A -> B, then B -> A.
        copyFilesAndPaste(*m_a, *m_b, makeTree(QDir(m_a->home()).filePath(QStringLiteral("copy")), 1));
        if (QTest::currentTestFailed())
            return;
        copyFilesAndPaste(*m_b, *m_a, makeTree(QDir(m_b->home()).filePath(QStringLiteral("copy")), 2));
    }

    void dragFromAToB()
    {
        if (!m_crossing)
            QSKIP("needs cursorCrossesAndReturns");
        const QStringList items = makeTree(QDir(m_a->home()).filePath(QStringLiteral("drag")), 3);
        const QMap<QString, QString> want = snapshot(items);
        bool asked = false;
        m_a->agent.onRequest(QStringLiteral("dragCatch"), [this, items, &asked](const QCborMap &r) {
            asked = true;
            m_a->agent.reply(r, QCborMap{{QStringLiteral("paths"), QCborArray::fromStringList(items)}});
        });
        QStringList recycled;
        m_a->agent.onRequest(QStringLiteral("recycle"), [this, &recycled](const QCborMap &r) {
            recycled = stringList(r.value(QStringLiteral("paths")));
            m_a->agent.reply(r, QCborMap{{QStringLiteral("ok"), true}});
        });
        auto cleanupHandlers = qScopeGuard([this] {
            m_a->agent.onRequest(QStringLiteral("dragCatch"), {});
            m_a->agent.onRequest(QStringLiteral("recycle"), {});
        });
        m_b->agent.discard(QStringLiteral("dragHover"));
        // The button is held at the edge: the master asks A's agent what is dragged there (pushed
        // again while the return guard of the previous test still holds).
        QElapsedTimer pushing;
        pushing.start();
        while (!asked && pushing.elapsed() < 5000) {
            m_a->helper.send(QStringLiteral("edge"), QCborMap{{QStringLiteral("s"), 1}, {QStringLiteral("x"), Service::kScreenWidth - 1},
                                                              {QStringLiteral("y"), 300}, {QStringLiteral("lb"), true}});
            QTest::qWaitFor([&] { return asked; }, 400);
        }
        E2E_VERIFY(asked, QStringLiteral("A's agent was not asked what is dragged"));
        const auto hover = m_b->agent.waitEvent(QStringLiteral("dragHover"), 10000);
        E2E_VERIFY(hover, QStringLiteral("B's agent was not asked to run the drag"));
        E2E_VERIFY(!hover->value(QStringLiteral("own")).toBool() && hover->value(QStringLiteral("n")).toInteger() == want.size(),
                   QStringLiteral("dragHover n=%1, expected %2").arg(hover->value(QStringLiteral("n")).toInteger()).arg(want.size()));
        int dirs = 0;
        for (const QCborValue &v : hover->value(QStringLiteral("files")).toArray())
            dirs += v.toArray().at(1).toInteger() < 0 ? 1 : 0;
        const auto wantDirs = std::count(want.cbegin(), want.cend(), QStringLiteral("dir"));
        E2E_VERIFY(dirs == wantDirs, QStringLiteral("dragHover lists %1 folders, expected %2").arg(dirs).arg(wantDirs));
        // Released on B: the drop target reads the files (dragFetch), then the drag ends there.
        m_b->agent.discard(QStringLiteral("dragHoverEnd"));
        m_a->helper.send(QStringLiteral("mb"), QCborMap{{QStringLiteral("b"), 1}, {QStringLiteral("d"), false}});
        const auto end = m_b->agent.waitEvent(QStringLiteral("dragHoverEnd"), 5000);
        E2E_VERIFY(end && end->value(QStringLiteral("drop")).toBool(), QStringLiteral("B's drag did not end with a drop"));
        m_b->agent.send(QStringLiteral("dragFetch"), QCborMap{{QStringLiteral("o"), hover->value(QStringLiteral("o"))}});
        const auto fetched = m_b->agent.waitEvent(QStringLiteral("dragFetched"), 60000);
        E2E_VERIFY(fetched && fetched->value(QStringLiteral("ok")).toBool(),
                   QStringLiteral("dragFetch failed: %1").arg(fetched ? fetched->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        const QMap<QString, QString> have = snapshot(stringList(fetched->value(QStringLiteral("files"))));
        E2E_VERIFY(have == want, QStringLiteral("the dropped tree differs:\n%1").arg(snapshotDiff(want, have)));
        m_b->agent.reply(*hover, QCborMap{{QStringLiteral("dropped"), true}, {QStringLiteral("confirmed"), true}});
        // Taken on B: A moves the originals to the Recycle Bin (move semantics).
        E2E_VERIFY(QTest::qWaitFor([&] { return !recycled.isEmpty(); }, 10000), QStringLiteral("A did not recycle the originals"));
        QStringList wantRecycled = items;
        std::sort(wantRecycled.begin(), wantRecycled.end());
        std::sort(recycled.begin(), recycled.end());
        for (QString &p : recycled)
            p = QDir::toNativeSeparators(p);
        E2E_VERIFY(recycled == wantRecycled, QStringLiteral("recycled: %1").arg(recycled.join(QStringLiteral(", "))));
        E2E_VERIFY(m_a->activeDevice(true) == m_b->idHex, QStringLiteral("the cursor is not on B after the drop"));
        E2E_VERIFY(returnToA(), QStringLiteral("the cursor did not come back to A"));
    }

    void dragFromBToA()
    {
        if (!m_crossing)
            QSKIP("needs cursorCrossesAndReturns");
        const QStringList items = makeTree(QDir(m_b->home()).filePath(QStringLiteral("drag")), 4);
        const QMap<QString, QString> want = snapshot(items);
        m_b->agent.onRequest(QStringLiteral("dragCatch"), [this, items](const QCborMap &r) {
            m_b->agent.reply(r, QCborMap{{QStringLiteral("paths"), QCborArray::fromStringList(items)}});
        });
        QStringList recycled;
        m_b->agent.onRequest(QStringLiteral("recycle"), [this, &recycled](const QCborMap &r) {
            recycled = stringList(r.value(QStringLiteral("paths")));
            m_b->agent.reply(r, QCborMap{{QStringLiteral("ok"), true}});
        });
        auto cleanupHandlers = qScopeGuard([this] {
            m_b->agent.onRequest(QStringLiteral("dragCatch"), {});
            m_b->agent.onRequest(QStringLiteral("recycle"), {});
        });
        E2E_VERIFY(crossToB(), QStringLiteral("the cursor did not cross to B"));
        // The button goes down on B, and the cursor comes back to A with it held.
        m_a->helper.discard(QStringLiteral("dragWatch"));
        m_a->agent.discard(QStringLiteral("dragHover"));
        m_a->helper.send(QStringLiteral("mb"), QCborMap{{QStringLiteral("b"), 1}, {QStringLiteral("d"), true}});
        QTest::qWait(100);
        m_a->helper.send(QStringLiteral("mm"), QCborMap{{QStringLiteral("x"), -2 * Service::kScreenWidth}, {QStringLiteral("y"), 0}});
        const auto hover = m_a->agent.waitEvent(QStringLiteral("dragHover"), 10000);
        E2E_VERIFY(hover, QStringLiteral("A's agent was not asked to run the drag"));
        E2E_VERIFY(hover->value(QStringLiteral("n")).toInteger() == want.size(),
                   QStringLiteral("dragHover n=%1, expected %2").arg(hover->value(QStringLiteral("n")).toInteger()).arg(want.size()));
        // A's own helper reports the release (the input is local again).
        const auto watch = m_a->helper.waitEvent(QStringLiteral("dragWatch"), 3000, [](const QCborMap &m) {
            return m.value(QStringLiteral("on")).toBool();
        });
        E2E_VERIFY(watch, QStringLiteral("A's helper was not told to watch the release"));
        m_a->agent.discard(QStringLiteral("dragHoverEnd"));
        m_a->helper.send(QStringLiteral("dragRelease"), QCborMap{{QStringLiteral("x"), 800}, {QStringLiteral("y"), 400}});
        const auto end = m_a->agent.waitEvent(QStringLiteral("dragHoverEnd"), 5000);
        E2E_VERIFY(end && end->value(QStringLiteral("drop")).toBool(), QStringLiteral("A's drag did not end with a drop"));
        m_a->agent.send(QStringLiteral("dragFetch"), QCborMap{{QStringLiteral("o"), hover->value(QStringLiteral("o"))}});
        const auto fetched = m_a->agent.waitEvent(QStringLiteral("dragFetched"), 60000);
        E2E_VERIFY(fetched && fetched->value(QStringLiteral("ok")).toBool(),
                   QStringLiteral("dragFetch failed: %1").arg(fetched ? fetched->value(QStringLiteral("error")).toString() : QStringLiteral("no answer")));
        const QMap<QString, QString> have = snapshot(stringList(fetched->value(QStringLiteral("files"))));
        E2E_VERIFY(have == want, QStringLiteral("the dropped tree differs:\n%1").arg(snapshotDiff(want, have)));
        m_a->agent.reply(*hover, QCborMap{{QStringLiteral("dropped"), true}, {QStringLiteral("confirmed"), true}});
        E2E_VERIFY(QTest::qWaitFor([&] { return !recycled.isEmpty(); }, 10000), QStringLiteral("B did not recycle the originals"));
        E2E_VERIFY(m_a->activeDevice(true) == m_a->idHex, QStringLiteral("the cursor is not home after the drop"));
    }

    void routerRestartKeepsTheSession()
    {
        if (!m_nat)
            QSKIP("NAT run only");
        if (!m_paired)
            QSKIP("needs pairingByPin");
        // Every mapping is gone (router restart). B either follows A to the new mapping A's next
        // datagram makes, or does not need to: from a peer's hello (0.1.22) B knows A's own
        // address, and on loopback that one is always reachable, so B usually talks to A directly
        // by now (the hairpin case: Node probes it). Either way the session survives. Following a
        // mapping itself is covered in tst_node (reconnectsAfterRestartsAndNewMappings).
        const QCborMap seen = m_b->peer(m_a->idHex);
        const QString before = QStringLiteral("%1:%2").arg(seen.value(QStringLiteral("address")).toString())
                                   .arg(seen.value(QStringLiteral("port")).toInteger());
        const bool direct = seen.value(QStringLiteral("address")).toString() != m_router->lanAddress().toString();
        m_router->reset();
        if (direct) {
            // Nothing to follow: traffic still passes (the cursor crosses and comes back).
            E2E_VERIFY(m_b->peer(m_a->idHex).value(QStringLiteral("connected")).toBool(),
                       QStringLiteral("B lost A (direct at %1) when the router restarted").arg(before));
            if (m_crossing)
                E2E_VERIFY(crossToB() && returnToA(),
                           QStringLiteral("the cursor does not cross after the router restarted (B talks to A at %1)").arg(before));
        } else {
            E2E_VERIFY(m_b->waitState([&](const QCborMap &s) {
                           const QCborMap p = Service::peerIn(s, m_a->idHex);
                           const QString now = QStringLiteral("%1:%2").arg(p.value(QStringLiteral("address")).toString())
                                                   .arg(p.value(QStringLiteral("port")).toInteger());
                           return p.value(QStringLiteral("connected")).toBool() && now != before;
                       }, 15000),
                       QStringLiteral("B did not follow A to its new mapping"));
        }
        E2E_VERIFY(m_a->waitConnected(m_b->idHex, 5000), QStringLiteral("A lost B"));
    }

    void restartAndReconnect()
    {
        if (!m_paired)
            QSKIP("needs pairingByPin");
        QString error;
        // B restarts (on its port): A finds it again where it was.
        m_b->stop();
        E2E_VERIFY(m_a->waitState([&](const QCborMap &s) {
                       return !Service::peerIn(s, m_b->idHex).value(QStringLiteral("connected")).toBool();
                   }, 15000),
                   QStringLiteral("A still shows B connected"));
        E2E_VERIFY(startB(&error), error);
        E2E_VERIFY(m_a->waitConnected(m_b->idHex, 20000) && m_b->waitConnected(m_a->idHex, 5000),
                   QStringLiteral("no reconnection after B restarted"));
        // A restarts on another port: it goes back to B's last address itself.
        m_a->stop();
        E2E_VERIFY(startOutside(*m_a, 0, &error), error);
        E2E_VERIFY(m_a->waitConnected(m_b->idHex, 20000),
                   QStringLiteral("A does not see B connected after A restarted: %1").arg(QString::fromUtf8(QCborValue(m_a->peer(m_b->idHex)).toDiagnosticNotation().toUtf8())));
        E2E_VERIFY(m_b->waitConnected(m_a->idHex, 10000),
                   QStringLiteral("B does not see A connected after A restarted: %1").arg(QCborValue(m_b->peer(m_a->idHex)).toDiagnosticNotation()));
        // The pairing and the workspace survived: the cursor still crosses.
        if (m_crossing)
            E2E_VERIFY(crossToB() && returnToA(), QStringLiteral("the cursor does not cross after the restarts"));
    }

    void groupIntroducesAThirdDevice()
    {
        if (!m_paired)
            QSKIP("needs pairingByPin");
        // A and B become a group; C joins A's group by PIN and is introduced to B: B and C trust
        // each other as group members and connect, with no PIN of their own.
        m_a->agent.send(QStringLiteral("setTrust"), QCborMap{{QStringLiteral("id"), m_b->idHex}, {QStringLiteral("trust"), QStringLiteral("group")}});
        m_b->agent.send(QStringLiteral("setTrust"), QCborMap{{QStringLiteral("id"), m_a->idHex}, {QStringLiteral("trust"), QStringLiteral("group")}});
        auto groupWith = [](Service &s, const QString &peer) {
            return s.waitState([&](const QCborMap &st) {
                const QCborMap p = Service::peerIn(st, peer);
                return p.value(QStringLiteral("connected")).toBool()
                       && p.value(QStringLiteral("effectiveTrust")).toString() == QLatin1String("group");
            }, 20000).has_value();
        };
        E2E_VERIFY(groupWith(*m_a, m_b->idHex) && groupWith(*m_b, m_a->idHex), QStringLiteral("A and B did not become a group"));
        m_c = std::make_unique<Service>(QStringLiteral("C"), m_work.path());
        QString error;
        E2E_VERIFY(startOutside(*m_c, 0, &error), error);
        m_b->agent.discard(QStringLiteral("pairPin"));
        E2E_VERIFY(pair(*m_c, *m_a, QStringLiteral("127.0.0.1"), m_a->dataPort, QStringLiteral("group"), QStringLiteral("group"), &error), error);
        E2E_VERIFY(groupWith(*m_b, m_c->idHex) && groupWith(*m_c, m_b->idHex), QStringLiteral("B and C were not introduced"));
        E2E_VERIFY(!m_b->agent.waitEvent(QStringLiteral("pairPin"), 0), QStringLiteral("B was shown a PIN for C"));
    }

private:
    const bool m_nat;
    QTemporaryDir m_work{QDir::tempPath() + QStringLiteral("/dfe2e-XXXXXX")};
    std::unique_ptr<Service> m_a, m_b, m_c;
    std::unique_ptr<NatRouter> m_router;
    quint16 m_bPort = 0;
    bool m_paired = false;
    bool m_crossing = false;
    QByteArray m_textKid;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // First argument: which part (cli, direct, nat); the rest goes to QtTest.
    QStringList modes{QStringLiteral("cli"), QStringLiteral("direct"), QStringLiteral("nat")};
    QList<char *> args{argv[0]};
    int first = 1;
    if (argc > 1 && modes.contains(QString::fromLocal8Bit(argv[1]))) {
        modes = {QString::fromLocal8Bit(argv[1])};
        first = 2;
    }
    for (int i = first; i < argc; ++i)
        args.append(argv[i]);
    int rc = 0;
    for (const QString &mode : std::as_const(modes)) {
        if (mode == QLatin1String("cli")) {
            ServiceCli t;
            rc |= QTest::qExec(&t, int(args.size()), args.data());
        } else {
            EndToEnd t(mode == QLatin1String("nat"));
            rc |= QTest::qExec(&t, int(args.size()), args.data());
        }
    }
    return rc;
}

#include "tst_e2e.moc"
