#include "clipboardsync.h"
#include "clock.h"
#include "crypto.h"
#include "filetransfer.h"
#include "testutil.h"
#include "wire.h"

#include <algorithm>
#include <QCborArray>
#include <QCborMap>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>
#include <optional>
#include <thread>

using namespace df;

namespace {
ClipboardData text(const QString &s) { return {{QStringLiteral("text/plain"), s.toUtf8()}}; }

// One device: node, file transfer and clipboard sync with its own downloads folder, where it
// believes the shared cursor is, and (as the master) its record of the paste chords it forwarded,
// which the real InputController keeps.
struct Device {
    QTemporaryDir config;
    QTemporaryDir downloads;
    std::unique_ptr<Node> node;
    std::unique_ptr<FileTransfer> transfer;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<ClipboardSync> clip;
    std::unique_ptr<QSignalSpy> applied;
    std::unique_ptr<QSignalSpy> received;
    std::unique_ptr<QSignalSpy> notices;
    bool cursorHere = true;
    QByteArray master;
    // Master side (fake InputController): the device a paste chord was forwarded to.
    QByteArray pasteOrderedDevice;
    // Guest side: a press from the master reached this device just now.
    bool inputArrivedFlag = false;
    // Paste evidence for file data (InputController::lastPasteGestureAt): by default "now", a
    // gesture is always fresh; a test sets -1 (never) or a time of its own.
    std::function<qint64()> pasteGestureAt = [] { return nowMs(); };
    // Gestures before the last one (pasteGestureAt), as InputController keeps them.
    std::vector<qint64> earlierGestures;
    bool localPressSupported = true;

    Device()
    {
        node = test::makeNode(config.path());
        transfer = std::make_unique<FileTransfer>(node.get());
        const QString dl = downloads.path();
        settings = std::make_unique<Settings>(config.path());
        ClipboardSync::Presence presence;
        presence.master = [this] { return master; };
        presence.cursorArrived = [this] { return cursorHere; };
        presence.cursorLeftRecently = [this](qint64) { return cursorHere; };
        presence.pasteOrderedOn = [this](const QByteArray &d, qint64) {
            return !pasteOrderedDevice.isEmpty() && d == pasteOrderedDevice;
        };
        presence.inputArrived = [this](qint64) { return inputArrivedFlag; };
        presence.lastPasteGesture = [this] { return pasteGestureAt(); };
        presence.localPressSupported = [this] { return localPressSupported; };
        presence.pasteGestureBetween = [this](qint64 from, qint64 to) {
            const auto within = [&](qint64 g) { return g >= 0 && g >= from && (to < 0 || g <= to); };
            return within(pasteGestureAt()) || std::any_of(earlierGestures.begin(), earlierGestures.end(), within);
        };
        clip = std::make_unique<ClipboardSync>(node.get(), transfer.get(), settings.get(), presence,
                                               [dl] { return dl; });
        applied = std::make_unique<QSignalSpy>(clip.get(), &ClipboardSync::applyLocally);
        received = std::make_unique<QSignalSpy>(node.get(), &Node::messageReceived);
        notices = std::make_unique<QSignalSpy>(clip.get(), &ClipboardSync::notice);
    }
    QByteArray id() const { return node->id(); }

    QByteArray lastKid() const { return applied->isEmpty() ? QByteArray() : applied->last().at(0).toByteArray(); }
    QVariantMap lastFormats() const { return applied->isEmpty() ? QVariantMap() : applied->last().at(1).toMap(); }
    QStringList lastMimes() const { return lastFormats().keys(); }
    QStringList lastFiles() const { return applied->isEmpty() ? QStringList() : applied->last().at(2).toStringList(); }
    int lastPending() const { return applied->isEmpty() ? 0 : applied->last().at(3).toInt(); }
    bool lastGated() const { return !applied->isEmpty() && applied->last().at(4).toBool(); }

    // The plaintext a paste gets for one format (clipUnseal) of the clipboard last applied (or of
    // `kid`), or empty on refusal.
    QByteArray unseal(const QString &mime, QString *err = nullptr, const QByteArray &kid = {})
    {
        bool done = false;
        QByteArray out;
        QString e;
        clip->unseal(kid.isEmpty() ? lastKid() : kid, mime, [&](bool ok, const QByteArray &plain, const QString &error) {
            done = true;
            if (ok)
                out = plain;
            else
                e = error;
        });
        QTest::qWaitFor([&] { return done; }, 8000);
        if (err)
            *err = e;
        return out;
    }

    // What a paste gets: the files transferred on demand (of the clipboard `kid`, when given), and
    // why not (code).
    QStringList paste(QString *err = nullptr, int timeoutMs = 15000, ClipboardSync::FetchError *code = nullptr,
                      const QByteArray &kid = {})
    {
        bool done = false;
        QStringList files;
        QString e;
        ClipboardSync::FetchError c = ClipboardSync::FetchError::Other;
        clip->fetchFiles(
            [&](bool ok, const QStringList &f, const QString &error, ClipboardSync::FetchError why) {
                done = true;
                c = why;
                if (ok)
                    files = f;
                else
                    e = error;
            },
            kid);
        QTest::qWaitFor([&] { return done; }, timeoutMs);
        if (err)
            *err = e;
        if (code)
            *code = c;
        return files;
    }
    // No paste gesture at this device (ever), or one at `at` (nowMs), or always a fresh one.
    void noGesture()
    {
        pasteGestureAt = [] { return qint64(-1); };
    }
    void gestureAt(qint64 at)
    {
        pasteGestureAt = [at] { return at; };
    }
    void freshGesture()
    {
        pasteGestureAt = [] { return nowMs(); };
    }

    int countType(const QString &t) const
    {
        int n = 0;
        for (const auto &args : *received) {
            if (args.at(1).value<QCborMap>().value(QStringLiteral("t")).toString() == t)
                ++n;
        }
        return n;
    }
    bool sawType(const QString &t) const { return countType(t) > 0; }
    // The last message of type `t` received.
    QCborMap lastOfType(const QString &t) const
    {
        for (qsizetype i = received->size() - 1; i >= 0; --i) {
            const QCborMap m = received->at(i).at(1).value<QCborMap>();
            if (m.value(QStringLiteral("t")).toString() == t)
                return m;
        }
        return {};
    }
    // The master's answer to the unseal request `rid` (sent by hand), once it arrives.
    std::optional<QCborMap> unsealAnswer(qint64 rid) const
    {
        for (const auto &args : *received) {
            const QCborMap m = args.at(1).value<QCborMap>();
            if (m.value(QStringLiteral("t")).toString() == QLatin1String("clip.unsealed")
                && m.value(QStringLiteral("rid")).toInteger() == rid)
                return m;
        }
        return std::nullopt;
    }
    // Makes `peer` believe this device runs `version` (the hello is a control message any node
    // may send; only its own hello is ever genuine).
    // `protocol`: the transfer protocol it claims too (this one's by default, so that the
    // clipboard's version checks are tested apart; none: what versions before 0.1.31 send).
    void claimVersionTo(const Device &peer, const QString &version,
                        std::optional<int> protocol = wire::kProtocolVersion)
    {
        QCborMap hello;
        hello.insert(QStringLiteral("t"), QStringLiteral("hello"));
        hello.insert(QStringLiteral("n"), node->name());
        hello.insert(QStringLiteral("tp"), node->tcpPort());
        hello.insert(QStringLiteral("v"), version);
        hello.insert(QStringLiteral("tl"), QStringLiteral("device"));
        if (protocol)
            hello.insert(QStringLiteral("pv"), *protocol);
        node->sendMessage(peer.id(), hello);
    }
    quint64 lastAnnouncedFileOffer() const
    {
        return quint64(lastOfType(QStringLiteral("clip")).value(QStringLiteral("fo")).toInteger());
    }
};

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// `master` shares its keyboard and mouse with the others; the cursor starts on it.
void shareFrom(Device &master, std::initializer_list<Device *> others)
{
    master.master = master.id();
    master.cursorHere = true;
    for (Device *d : others) {
        d->master = master.id();
        d->cursorHere = false;
    }
}

// The master moves the cursor from `from` to `to` (as InputController reports it).
void moveCursor(Device &master, Device &from, Device &to)
{
    from.cursorHere = true; // just left: within the hand-off grace
    to.cursorHere = true;
    master.clip->cursorMoved(from.id(), to.id());
}

// An offer of `dirCount` folders under "tree" and `fileCount` files in them, the files pointing
// at a path that need not exist (only listed, never read).
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

// `from` publishes `offer` and announces it to `to` by hand, as a 0.1.30 publisher announces a
// copy too large to list (no fl): counts from the offer unless given, sp when `shortPaths`, and
// `to` let in when `letIn`. Waits until `to` applied it; returns its kid.
QByteArray announceFiles(Device &from, Device &to, const Offer &offer, qint64 stamp, bool shortPaths = true,
                         bool letIn = true, int fileCount = -1, int dirCount = -1)
{
    const quint64 offerId = from.transfer->publish(offer);
    if (letIn)
        from.transfer->allowPeer(offerId, to.id(), true);
    const QByteArray kid = crypto::randomBytes(16);
    QCborMap clip;
    clip.insert(QStringLiteral("t"), QStringLiteral("clip"));
    clip.insert(QStringLiteral("v"), stamp);
    clip.insert(QStringLiteral("a"), from.id());
    clip.insert(QStringLiteral("kid"), kid);
    clip.insert(QStringLiteral("sf"), QCborMap());
    clip.insert(QStringLiteral("big"), QCborMap());
    clip.insert(QStringLiteral("fb"), offer.fileBytes());
    clip.insert(QStringLiteral("fc"), fileCount >= 0 ? fileCount : int(offer.files.size()));
    clip.insert(QStringLiteral("dc"), dirCount >= 0 ? dirCount : int(offer.dirs.size()));
    clip.insert(QStringLiteral("o"), 0);
    clip.insert(QStringLiteral("fo"), qint64(offerId));
    if (letIn)
        clip.insert(QStringLiteral("fa"), true);
    if (shortPaths)
        clip.insert(QStringLiteral("sp"), true);
    const qsizetype before = to.applied->size();
    if (!from.node->sendMessage(to.id(), clip))
        return {};
    if (!QTest::qWaitFor([&] { return to.applied->size() > before && to.lastKid() == kid; }, 10000))
        return {};
    return kid;
}

// What listFiles answers, waiting for it.
struct ListAnswer {
    bool answered = false;
    bool ok = false;
    QList<ClipboardSync::ListedFile> items;
    QString error;
    ClipboardSync::ListCallback callback()
    {
        return [this](bool o, const QList<ClipboardSync::ListedFile> &l, const QString &e) {
            answered = true;
            ok = o;
            items = l;
            error = e;
        };
    }
};

// What fetchFiles answers, waiting for it.
struct FilesAnswer {
    bool answered = false;
    bool ok = false;
    QStringList files;
    QString error;
    ClipboardSync::FetchError code = ClipboardSync::FetchError::Other;
    ClipboardSync::FilesCallback callback()
    {
        return [this](bool o, const QStringList &f, const QString &e, ClipboardSync::FetchError c) {
            answered = true;
            ok = o;
            files = f;
            error = e;
            code = c;
        };
    }
};

// `from` copies one small file, which reaches `to` (a trusted device): waits until it is applied
// there and returns its kid.
QByteArray copyOneFile(Device &from, Device &to, const QString &path, const QByteArray &content = "data")
{
    writeFile(path, content);
    const qsizetype before = to.applied->size();
    from.clip->localChanged({}, {path});
    if (!QTest::qWaitFor([&] { return to.applied->size() > before && to.lastPending() > 0; }, 15000))
        return {};
    return to.lastKid();
}

// `owner` treats `guest` as a guest, and the guest has heard so (its hello declares it).
bool markGuest(Device &owner, Device &guest)
{
    owner.node->setTrust(guest.id(), TrustLevel::Guest);
    return QTest::qWaitFor([&] { return guest.node->declaredTrust(owner.id()) == TrustLevel::Guest; }, 5000);
}
} // namespace

class TestClipboard : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<df::ClipboardData>("df::ClipboardData");
        QVERIFY(crypto::init());
    }

    // --- Trusted devices -------------------------------------------------------------------

    // A copy reaches every trusted device at once, sealed and with its key: the paste there just
    // decrypts, with no activity, chord, master or round trip of any kind.
    void trustedPasteWithoutAnyActivityIsGranted()
    {
        Device a, b, c;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*a.node, *c.node));
        shareFrom(a, {&b, &c});

        ClipboardData rich = text("hello");
        rich.insert(QStringLiteral("text/html"), "<b>hello</b>");
        rich.insert(QStringLiteral("application/x-ignored"), "dropped");
        a.clip->localChanged(rich);
        QTRY_COMPARE(b.applied->size(), 1); // no crossing needed
        QTRY_COMPARE(c.applied->size(), 1);
        QVERIFY(!b.lastKid().isEmpty());
        QVERIFY(b.lastMimes().contains(QStringLiteral("text/plain")));
        QVERIFY(b.lastMimes().contains(QStringLiteral("text/html")));
        QVERIFY(!b.lastMimes().contains(QStringLiteral("application/x-ignored")));
        QVERIFY(!b.lastGated());

        // No input of any kind on B, no chord at the master.
        b.inputArrivedFlag = false;
        a.pasteOrderedDevice.clear();
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("hello"));
        QCOMPARE(b.unseal(QStringLiteral("text/html")), QByteArray("<b>hello</b>"));
        QCOMPARE(c.unseal(QStringLiteral("text/plain")), QByteArray("hello"));
        // Read again much later (a clipboard manager, a second paste): still served.
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("hello"));
        QTest::qWait(100);
        QVERIFY(!a.sawType(QStringLiteral("clip.unseal"))); // nobody had to ask

        // The master pastes what a slave copied, just as freely.
        b.clip->localChanged(text("from B"));
        QTRY_COMPARE(a.applied->size(), 1);
        QCOMPARE(a.unseal(QStringLiteral("text/plain")), QByteArray("from B"));
    }

    // Two devices used each with its own keyboard, nobody sharing: the clipboard still flows both
    // ways (the 0.1.19 behaviour the hand-off-only rule had lost).
    void trustedDevicesShareWithoutAMasterOrACrossing()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(a.master.isEmpty() && b.master.isEmpty());
        a.cursorHere = false;
        b.cursorHere = false;

        a.clip->localChanged(text("copied on A"));
        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("copied on A"));

        b.clip->localChanged(text("copied on B"));
        QTRY_COMPARE(a.applied->size(), 1);
        QCOMPARE(a.unseal(QStringLiteral("text/plain")), QByteArray("copied on B"));
        QVERIFY(a.notices->isEmpty());
        QVERIFY(b.notices->isEmpty());
    }

    // A device that connects later gets the current clipboard once its hello arrived.
    void deviceConnectingLaterGetsTheClipboard()
    {
        Device a, b;
        a.clip->localChanged(text("before pairing"));
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("before pairing"));
    }

    // Last writer wins on a logical clock: a copy made after seeing another one wins even if the
    // clock of the device it was made on is behind; a stamp absurdly far ahead is refused, and the
    // user told.
    void logicalClockSurvivesSkewedClocks()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->setWallClock([] { return QDateTime::currentMSecsSinceEpoch() + 120000; }); // A is 2 min ahead

        a.clip->localChanged(text("from the fast clock"));
        QTRY_COMPARE(b.applied->size(), 1);
        // B copies right after: with wall clocks its stamp would be 2 minutes older and lose.
        b.clip->localChanged(text("copied later"));
        QTRY_COMPARE(a.applied->size(), 1);
        QCOMPARE(a.unseal(QStringLiteral("text/plain")), QByteArray("copied later"));

        // A stamp more than a day ahead is not taken.
        QCborMap clip = b.lastOfType(QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), QDateTime::currentMSecsSinceEpoch() + 2 * ClipboardSync::kMaxClockAheadMs);
        clip.insert(QStringLiteral("kid"), crypto::randomBytes(16));
        QVERIFY(a.node->sendMessage(b.id(), clip));
        QTRY_VERIFY(!b.notices->isEmpty());
        QCOMPARE(b.applied->size(), 1);
    }

    // An older clipboard never replaces a newer one.
    void olderClipboardDoesNotWin()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged(text("new"));
        QTRY_COMPARE(b.applied->size(), 1);
        QCborMap old = b.lastOfType(QStringLiteral("clip"));
        old.insert(QStringLiteral("v"), old.value(QStringLiteral("v")).toInteger() - 1000);
        old.insert(QStringLiteral("kid"), crypto::randomBytes(16));
        QVERIFY(a.node->sendMessage(b.id(), old));
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 1);
    }

    // The owner of the local clipboard (the agent) that reconnects gets an adopted clipboard put
    // back; for one copied here there is nothing to put back (the copying application owns it).
    void clipboardOwnerReconnectingGetsReapply()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged(text("adopted on B"));
        QTRY_COMPARE(b.applied->size(), 1);
        const QByteArray kid = b.lastKid();

        QVERIFY(b.clip->reapply());
        QCOMPARE(b.applied->size(), 2);
        QCOMPARE(b.lastKid(), kid);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("adopted on B"));

        QVERIFY(!a.clip->reapply()); // A's own copy
        QCOMPARE(a.applied->size(), 0);
        Device fresh; // after a restart: nothing at all, so the owner drops what it still holds
        QVERIFY(!fresh.clip->reapply());

        // B's user copies something that is not shared (too large): the adopted clipboard must not
        // be put back over it.
        b.clip->localChanged({{QStringLiteral("image/png"), QByteArray(ClipboardSync::kMaxFormatBytes + 1, 'x')}});
        QVERIFY(!b.clip->reapply());
        QCOMPARE(b.applied->size(), 2);
        // A newer clipboard from A is put back again.
        a.clip->localChanged(text("newer"));
        QTRY_COMPARE(b.applied->size(), 3);
        QVERIFY(b.clip->reapply());
    }

    // A stale clipboard object (an older id: an application kept it, or the service restarted)
    // renders nothing and says why.
    void staleClipboardIsNoticed()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged(text("one"));
        QTRY_COMPARE(b.applied->size(), 1);
        const QByteArray oldKid = b.lastKid();
        a.clip->localChanged(text("two"));
        QTRY_COMPARE(b.applied->size(), 2);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("two"));
        QString err;
        QCOMPARE(b.unseal(QStringLiteral("text/plain"), &err, oldKid), QByteArray());
        QCOMPARE(err, QString("The clipboard changed"));
        QCOMPARE(b.notices->size(), 1);
    }

    void largeImageTravelsOverTcp()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QByteArray image(3 * 1024 * 1024, Qt::Uninitialized);
        for (int i = 0; i < image.size(); ++i)
            image[i] = char(i * 31);
        ClipboardData data = text("caption");
        data.insert(QStringLiteral("image/png"), image);
        a.clip->localChanged(data);
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("caption"));
        QCOMPARE(b.unseal(QStringLiteral("image/png")), image);
    }

    // Copied files and folders (nested, and empty ones) are announced only, and transferred when
    // the user pastes, straight from the device they were copied on, through FileTransfer.
    void copiedFoldersArePastedEndToEnd()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("notes.txt"), "remember the milk");
        writeFile(source.filePath("project/src/main.cpp"), "int main() {}");
        writeFile(source.filePath("project/README.md"), "# project");
        QVERIFY(QDir().mkpath(source.filePath("project/empty/deeper")));

        a.clip->localChanged({}, {source.filePath("notes.txt"), source.filePath("project")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QVERIFY(b.lastFiles().isEmpty());
        // Three files; "project", "project/src", "project/empty", "project/empty/deeper".
        QCOMPARE(b.lastPending(), 7);
        QCOMPARE(b.clip->pendingFileList().size(), 7);
        QVERIFY(!QDir(b.downloads.filePath("Clipboard")).exists() || QDir(b.downloads.filePath("Clipboard")).isEmpty());

        // The paste is the grant: no activity, no chord.
        QSignalSpy progress(b.transfer.get(), &FileTransfer::transferFinished);
        const QStringList files = b.paste();
        QCOMPARE(files.size(), 2);
        for (const QString &f : files)
            QVERIFY2(f.startsWith(QDir::cleanPath(b.downloads.path() + "/Clipboard/")), qPrintable(f));
        const QString root = QFileInfo(files.first()).absolutePath();
        QCOMPARE(readFile(root + "/notes.txt"), QByteArray("remember the milk"));
        QCOMPARE(readFile(root + "/project/src/main.cpp"), QByteArray("int main() {}"));
        QCOMPARE(readFile(root + "/project/README.md"), QByteArray("# project"));
        QVERIFY(QFileInfo(root + "/project/empty/deeper").isDir());
        QVERIFY(!a.sawType(QStringLiteral("clip.unseal")));
        QCOMPARE(progress.size(), 1); // a transfer the user sees ("clipboard")
        QCOMPARE(progress.first().at(2).toString(), QString("clipboard"));
    }

    // A device of another transfer protocol (0.1.30: none advertised) cannot send files or large
    // formats here: what came inline is applied, the files and the large image are not offered (a
    // paste would fail), the user is told which device to update (once), and nothing is fetched.
    void filesFromIncompatiblePeerAreNotOffered()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.claimVersionTo(b, QStringLiteral("0.1.30"), std::nullopt);
        QTRY_COMPARE(b.node->peerProtocol(a.id()), 2);
        QSignalSpy started(b.transfer.get(), &FileTransfer::transferProgress);
        int accepted = 0;
        connect(a.node.get(), &Node::streamAccepted, this, [&](SecureStream *) { ++accepted; });
        QTemporaryDir source;
        writeFile(source.filePath("notes.txt"), "remember the milk");
        QByteArray image(3 * 1024 * 1024, Qt::Uninitialized); // too large to travel inline
        for (int i = 0; i < image.size(); ++i)
            image[i] = char(i * 7);
        ClipboardData data = text("caption");
        data.insert(QStringLiteral("image/png"), image);
        a.clip->localChanged(data, {source.filePath("notes.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.lastPending(), 0);
        QVERIFY(b.clip->pendingFileList().isEmpty());
        QVERIFY(b.lastMimes().contains(QStringLiteral("text/plain")));
        QVERIFY(!b.lastMimes().contains(QStringLiteral("image/png")));
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("caption"));
        QCOMPARE(b.notices->size(), 1);
        const QString notice = b.notices->first().at(0).toString();
        QVERIFY2(notice.startsWith(QStringLiteral("Files copied on %1 cannot be pasted here: %1 uses an older transfer "
                                                  "protocol (version 2)")
                                       .arg(a.node->name())),
                 qPrintable(notice));
        QVERIFY(b.notices->first().at(1).toBool()); // a problem
        QTest::qWait(300);
        QCOMPARE(accepted, 0); // nothing was fetched
        QVERIFY(started.isEmpty());

        // Another copy from it: the notice is not repeated at once.
        a.clip->localChanged(text("again"), {source.filePath("notes.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 2, 15000);
        QCOMPARE(b.lastPending(), 0);
        QCOMPARE(b.notices->size(), 1);

        // Updated: its files are offered again.
        a.claimVersionTo(b, QStringLiteral("0.1.31"));
        QTRY_COMPARE(b.node->peerProtocol(a.id()), wire::kProtocolVersion);
        a.clip->localChanged(text("now"), {source.filePath("notes.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 3, 15000);
        QCOMPARE(b.lastPending(), 1);
        const QStringList files = b.paste();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), QByteArray("remember the milk"));
    }

    // A source tree with more than 10,000 files (the bound before 0.1.27) is offered and pasted:
    // walked on a worker thread, announced by count (too many to list), transferred on paste.
    void largeFolderCopyIsPasted()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        constexpr int kDirs = 101, kPerDir = 100; // 10,100 files
        for (int d = 0; d < kDirs; ++d) {
            const QString dir = source.filePath(QStringLiteral("tree/d%1").arg(d));
            QVERIFY(QDir().mkpath(dir));
            for (int f = 0; f < kPerDir; ++f)
                writeFile(dir + QStringLiteral("/f%1.txt").arg(f), QByteArray::number(d * kPerDir + f));
        }
        QSignalSpy filesNotices(a.clip.get(), &ClipboardSync::filesNotice);
        a.clip->localChanged({}, {source.filePath("tree")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 60000);
        QVERIFY(!filesNotices.isEmpty());
        QVERIFY(!filesNotices.last().at(1).toBool()); // offered, not refused
        QCOMPARE(b.lastPending(), kDirs * kPerDir + kDirs + 1); // the files, d0..d100 and "tree"
        QVERIFY(b.clip->pendingFileList().isEmpty()); // over kMaxListedFiles: count only

        // 10,100 small files: slow where an antivirus scans each new file.
        QString error;
        const QStringList files = b.paste(&error, 120000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first() + "/d100/f99.txt"), QByteArray::number(10099));
        QCOMPARE(QDir(files.first() + "/d42").entryList(QDir::Files).size(), kPerDir);
    }

    // A copy of folders alone (an empty one, one holding only empty folders) is a copy too.
    void folderOnlyCopyIsAdoptedAndPasted()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        QVERIFY(QDir().mkpath(source.filePath("empty")));
        QVERIFY(QDir().mkpath(source.filePath("tree/one/two")));
        QVERIFY(QDir().mkpath(source.filePath("tree/three")));
        QSignalSpy filesNotices(a.clip.get(), &ClipboardSync::filesNotice);
        a.clip->localChanged({}, {source.filePath("empty"), source.filePath("tree")});
        QTRY_VERIFY(!filesNotices.isEmpty()); // once the folders are listed (on a worker thread)
        QVERIFY(!filesNotices.last().at(1).toBool()); // "copied folders", not a problem

        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.lastPending(), 5); // empty, tree, tree/one, tree/one/two, tree/three
        const QList<ClipboardSync::ListedFile> listed = b.clip->pendingFileList();
        QCOMPARE(listed.size(), 5);
        for (const ClipboardSync::ListedFile &f : listed)
            QVERIFY2(f.dir, qPrintable(f.path));

        const QStringList tops = b.paste();
        QCOMPARE(tops.size(), 2);
        const QString root = QFileInfo(tops.first()).absolutePath();
        QVERIFY(QFileInfo(root + "/empty").isDir());
        QVERIFY(QFileInfo(root + "/tree/one/two").isDir());
        QVERIFY(QFileInfo(root + "/tree/three").isDir());
    }

    // A paste whose files are gone at the source fails visibly (a notice), not silently.
    void filesGoneAtTheSourceFailVisibly()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("f.txt"), "x");
        a.clip->localChanged({}, {source.filePath("f.txt")});
        QTRY_COMPARE(b.applied->size(), 1);
        a.clip->reset(); // A forgets its offers (another user signed in there)
        QSignalSpy filesNotices(b.clip.get(), &ClipboardSync::filesNotice);
        QString err;
        QVERIFY(b.paste(&err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(!filesNotices.isEmpty());
        QVERIFY(filesNotices.last().at(1).toBool());
    }

    // --- The paste rule for file data --------------------------------------------------------

    // A trusted device reading copied files with no key or click just before (a clipboard history,
    // a file manager looking at what arrived) gets nothing: no data is asked of the publisher, the
    // refusal says why (NoPaste) after the grace, the user is offered the files once (filesWaiting)
    // and not bothered with a notice. A paste then gets them.
    void trustedFilePasteWithoutEvidenceIsRefusedQuietly()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("f.txt"), "fresh");
        QVERIFY(!kid.isEmpty());
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        QSignalSpy waiting(b.clip.get(), &ClipboardSync::filesWaiting);
        QSignalSpy filesNotices(b.clip.get(), &ClipboardSync::filesNotice);
        b.noGesture();

        QElapsedTimer elapsed;
        elapsed.start();
        QString err;
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(b.paste(&err, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);
        QVERIFY(!err.isEmpty());
        QVERIFY2(elapsed.elapsed() >= ClipboardSync::kEvidenceGraceMs - 50, qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(waiting.size(), 1);
        QCOMPARE(waiting.first().at(0).toByteArray(), kid);
        QCOMPARE(waiting.first().at(1).toInt(), 1);
        QVERIFY(!waiting.first().at(2).toString().isEmpty());
        QCOMPARE(b.notices->size(), 0);
        QCOMPARE(filesNotices.size(), 0);
        // Asked again: refused again, but the files are offered only once per clipboard.
        QVERIFY(b.paste(&err, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);
        QCOMPARE(waiting.size(), 1);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 0); // nothing asked of the publisher
        QVERIFY(!QDir(b.downloads.filePath("Clipboard")).exists());

        b.freshGesture();
        const QStringList files = b.paste(&err, 15000, &code);
        QVERIFY2(files.size() == 1, qPrintable(err));
        QCOMPARE(code, ClipboardSync::FetchError::None);
        QCOMPARE(readFile(files.first()), QByteArray("fresh"));
        QCOMPARE(streams.size(), 1);
        // The files are here: the offer is over (count 0).
        QCOMPARE(waiting.size(), 2);
        QCOMPARE(waiting.last().at(0).toByteArray(), kid);
        QCOMPARE(waiting.last().at(1).toInt(), 0);
    }

    // A press made before the clipboard was applied here (the click that brought the cursor, say)
    // did not paste it.
    void evidenceBeforeTheClipboardArrivedDoesNotCount()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.gestureAt(nowMs());
        QTest::qWait(30);
        QTemporaryDir source;
        QVERIFY(!copyOneFile(a, b, source.filePath("f.txt")).isEmpty());
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(b.paste(nullptr, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);

        b.gestureAt(nowMs()); // pressed now, after the apply
        QCOMPARE(b.paste(nullptr, 15000, &code).size(), 1);
        QCOMPARE(code, ClipboardSync::FetchError::None);
    }

    // A file manager reads the list at the paste, then creates every folder before it reads the
    // first file's contents: a streaming request that says how long ago its reader read the list
    // (listedAgo) passes with a press made around that read, however long ago, within
    // kDescriptorReferenceMs; a press long before that read, or a list read too long ago, does not.
    void pressAroundTheListReadCountsForAStreamingPaste()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("f.txt"), "tree");
        QVERIFY(!kid.isEmpty());
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        QTest::qWait(30);
        const qint64 pressed = nowMs();
        b.gestureAt(pressed); // the paste's Ctrl+V, then the folders take a while
        QTest::qWait(int(ClipboardSync::kChordWindowMs) + 300);

        auto ask = [&](qint64 listedAgo) {
            auto answer = std::make_shared<FilesAnswer>();
            b.clip->fetchFiles(answer->callback(), kid, {}, true, listedAgo);
            [[maybe_unused]] const bool answered = QTest::qWaitFor([&] { return answer->answered; }, 15000);
            return answer;
        };
        // No list read said: the press is too old for a request made now.
        QCOMPARE(ask(-1)->code, ClipboardSync::FetchError::NoPaste);
        // A list read just now: the press came long before it.
        QTest::qWait(int(ClipboardSync::kNoPasteMemoryMs) + 50);
        QCOMPARE(ask(10)->code, ClipboardSync::FetchError::NoPaste);
        // A list read beyond kDescriptorReferenceMs does not count at all.
        QTest::qWait(int(ClipboardSync::kNoPasteMemoryMs) + 50);
        QCOMPARE(ask(ClipboardSync::kDescriptorReferenceMs + 1)->code, ClipboardSync::FetchError::NoPaste);
        QCOMPARE(streams.size(), 0);
        // The list read right after the press: the paste.
        QTest::qWait(int(ClipboardSync::kNoPasteMemoryMs) + 50);
        const auto got = ask(nowMs() - pressed - 50);
        QVERIFY2(got->ok, qPrintable(got->error));
        QCOMPARE(got->code, ClipboardSync::FetchError::None);
        QCOMPARE(got->files.size(), 1);
        QCOMPARE(readFile(got->files.first()), QByteArray("tree"));
    }

    // The user clicks elsewhere while the file manager creates the folders of a large paste: the
    // press of the paste, around the list read, still counts although it is not the last one.
    void laterClickDoesNotHideThePasteAroundTheListRead()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("f.txt"), "tree");
        QVERIFY(!kid.isEmpty());
        QTest::qWait(30);
        const qint64 pressed = nowMs(); // the click on Paste; the list is read right after it
        QTest::qWait(int(ClipboardSync::kChordWindowMs) + 300);
        b.earlierGestures = {pressed};
        b.gestureAt(nowMs()); // a click on another window while the folders are created
        QTest::qWait(int(ClipboardSync::kChordWindowMs) + 300);

        auto answer = std::make_shared<FilesAnswer>();
        b.clip->fetchFiles(answer->callback(), kid, {}, true, nowMs() - pressed - 50);
        QVERIFY(QTest::qWaitFor([&] { return answer->answered; }, 15000));
        QVERIFY2(answer->ok, qPrintable(answer->error));
        QCOMPARE(answer->code, ClipboardSync::FetchError::None);
        QCOMPARE(answer->files.size(), 1);
        QCOMPARE(readFile(answer->files.first()), QByteArray("tree"));
    }

    // Applying the same clipboard again (its owner reconnected, the gate re-told) puts a new object
    // on the local clipboard, which clipboard readers read again: a press made before it does not
    // let such a read move the files. A press after it does.
    void reapplyRestartsEvidence()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("f.txt"));
        QVERIFY(!kid.isEmpty());
        QTest::qWait(30);
        b.gestureAt(nowMs()); // a click after the apply, then the owner reconnects
        QTest::qWait(30);
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        QVERIFY(b.clip->reapply());
        QCOMPARE(b.lastKid(), kid);
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(b.paste(nullptr, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);
        QTest::qWait(100);
        QCOMPARE(streams.size(), 0);

        QString err;
        b.freshGesture();
        QVERIFY2(b.paste(&err, 15000, &code).size() == 1, qPrintable(err));
        QCOMPARE(code, ClipboardSync::FetchError::None);
    }

    // A reader asking for the files again and again with nothing pressed (once per item, once per
    // format) is refused at once after the first refusal, not held for the grace each time; a
    // press then still pastes.
    void repeatedRequestsWithoutAPressAreRefusedAtOnce()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        QVERIFY(!copyOneFile(a, b, source.filePath("f.txt"), "burst").isEmpty());
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        b.noGesture();
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 10; ++i) {
            ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
            QVERIFY(b.paste(nullptr, 5000, &code).isEmpty());
            QCOMPARE(code, ClipboardSync::FetchError::NoPaste);
        }
        QVERIFY2(elapsed.elapsed() < 3 * ClipboardSync::kEvidenceGraceMs, qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(streams.size(), 0);

        b.freshGesture();
        QString err;
        const QStringList files = b.paste(&err, 15000);
        QVERIFY2(files.size() == 1, qPrintable(err));
        QCOMPARE(readFile(files.first()), QByteArray("burst"));
    }

    // The press of the paste reaches the service a moment after the paste's own request (the
    // helper's localPress and the request use different connections): within the grace it
    // counts, and the transfer starts as soon as it comes.
    void lateLocalPressWithinGraceIsAccepted()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        QVERIFY(!copyOneFile(a, b, source.filePath("f.txt"), "late").isEmpty());
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        b.noGesture();
        FilesAnswer answer;
        b.clip->fetchFiles(answer.callback());
        QVERIFY(!answer.answered);
        QTest::qWait(100);
        QVERIFY(!answer.answered);
        QCOMPARE(streams.size(), 0);
        b.freshGesture();
        b.clip->pasteGestureNoted();
        QTRY_COMPARE(streams.size(), 1); // asked at once, not at the end of the grace
        QTRY_VERIFY_WITH_TIMEOUT(answer.answered, 15000);
        QVERIFY2(answer.ok, qPrintable(answer.error));
        QCOMPARE(answer.code, ClipboardSync::FetchError::None);
        QCOMPARE(readFile(answer.files.value(0)), QByteArray("late"));

        // A press that came without a word (no pasteGestureNoted) is still seen when the grace ends.
        QVERIFY(!copyOneFile(a, b, source.filePath("g.txt"), "later").isEmpty());
        b.noGesture();
        FilesAnswer quiet;
        b.clip->fetchFiles(quiet.callback());
        QTest::qWait(50);
        b.freshGesture();
        QTRY_VERIFY_WITH_TIMEOUT(quiet.answered, 15000);
        QVERIFY2(quiet.ok, qPrintable(quiet.error));
    }

    // A request that comes while one waits for its press joins it: one transfer, both answered
    // alike, refused alike.
    void secondRequestDuringGraceStartsOneFetch()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        QVERIFY(!copyOneFile(a, b, source.filePath("f.txt")).isEmpty());
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        QSignalSpy finished(b.transfer.get(), &FileTransfer::transferFinished);
        QSignalSpy waiting(b.clip.get(), &ClipboardSync::filesWaiting);
        b.noGesture();

        FilesAnswer first, second;
        b.clip->fetchFiles(first.callback());
        b.clip->fetchFiles(second.callback());
        QTRY_VERIFY(first.answered && second.answered);
        QCOMPARE(first.code, ClipboardSync::FetchError::NoPaste);
        QCOMPARE(second.code, ClipboardSync::FetchError::NoPaste);
        QCOMPARE(waiting.size(), 1);

        // A while later (past the refusal's memory, which would refuse at once): the press comes
        // during the grace.
        QTest::qWait(ClipboardSync::kNoPasteMemoryMs + 100);
        FilesAnswer third, fourth;
        b.clip->fetchFiles(third.callback());
        QTest::qWait(50);
        b.clip->fetchFiles(fourth.callback());
        b.freshGesture();
        b.clip->pasteGestureNoted();
        QTRY_VERIFY_WITH_TIMEOUT(third.answered && fourth.answered, 15000);
        QVERIFY2(third.ok, qPrintable(third.error));
        QVERIFY(fourth.ok);
        QCOMPARE(third.files, fourth.files);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(streams.size(), 1);
    }

    // The master pasting files copied on another device needs the press too.
    void masterPastingAnotherDevicesFilesNeedsEvidence()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        QTemporaryDir source;
        QVERIFY(!copyOneFile(b, a, source.filePath("from-b.txt"), "slave").isEmpty());
        QSignalSpy streams(b.node.get(), &Node::streamAccepted);
        a.noGesture();
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(a.paste(nullptr, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 0);
        QVERIFY(!b.sawType(QStringLiteral("clip.allow")));

        a.freshGesture();
        const QStringList files = a.paste(nullptr, 15000, &code);
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), QByteArray("slave"));
    }

    // A clipboard object that is not the current clipboard any more pastes nothing, moves nothing
    // and says why.
    void staleKidIsRefused()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray oldKid = copyOneFile(a, b, source.filePath("one.txt"), "one");
        QVERIFY(!oldKid.isEmpty());
        const QByteArray kid = copyOneFile(a, b, source.filePath("two.txt"), "two");
        QVERIFY(!kid.isEmpty() && kid != oldKid);
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        QString err;
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        // Read with nothing pressed (a clipboard monitor): refused quietly.
        b.noGesture();
        QVERIFY(b.paste(&err, 5000, &code, oldKid).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::Changed);
        QCOMPARE(b.notices->size(), 0);
        // A paste: the user hears why nothing came.
        b.freshGesture();
        QVERIFY(b.paste(&err, 5000, &code, oldKid).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::Changed);
        QCOMPARE(err, QString("The clipboard changed"));
        QCOMPARE(b.notices->size(), 1);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 0);

        const QStringList files = b.paste(&err, 15000, &code, kid);
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), QByteArray("two"));
    }

    // The user cancelling the transfer of a paste is no problem to report: the paste hears so
    // (Cancelled), and no problem notice goes out.
    void userCancelIsNotAProblemNotice()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.transfer->setWriteDelayForTests(50);
        QTemporaryDir source;
        QVERIFY(!copyOneFile(a, b, source.filePath("slow.bin"), QByteArray(4 * 1024 * 1024, 's')).isEmpty());
        QSignalSpy progress(b.transfer.get(), &FileTransfer::transferProgress);
        QSignalSpy filesNotices(b.clip.get(), &ClipboardSync::filesNotice);
        FilesAnswer answer;
        b.clip->fetchFiles(answer.callback());
        QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty(), 20000);
        QVERIFY(!answer.answered);
        b.transfer->cancel(progress.first().at(0).toULongLong());
        QTRY_VERIFY(answer.answered);
        QVERIFY(!answer.ok);
        QCOMPARE(answer.code, ClipboardSync::FetchError::Cancelled);
        for (const auto &n : std::as_const(filesNotices))
            QVERIFY2(!n.at(1).toBool(), qPrintable(n.at(0).toString()));
        QCOMPARE(b.notices->size(), 0);
    }

    // A file manager reads the list, then the contents: when the list was just given, the window
    // counts from when it was asked for (a large list may take the network a while).
    void pasteRightAfterAListCountsFromTheList()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        QVERIFY(!copyOneFile(a, b, source.filePath("one.txt")).isEmpty());
        // Without a list: a press 2.3 s before the request is too old.
        QTest::qWait(30);
        b.gestureAt(nowMs());
        QTest::qWait(2300);
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(b.paste(nullptr, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);

        // With the list asked 1.5 s after the press and the contents 0.8 s later: it counts.
        const QByteArray kid = copyOneFile(a, b, source.filePath("two.txt"));
        QVERIFY(!kid.isEmpty());
        QTest::qWait(30);
        b.gestureAt(nowMs());
        QTest::qWait(1500);
        ListAnswer list;
        b.clip->listFiles(kid, list.callback());
        QVERIFY(list.answered && list.ok); // listed in the announcement: answered at once
        QTest::qWait(800);
        QString err;
        QVERIFY2(b.paste(&err, 15000, &code).size() == 1, qPrintable(err));
        QCOMPARE(code, ClipboardSync::FetchError::None);
    }

    void oversizedFormatIsNotShared()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged({{QStringLiteral("image/png"), QByteArray(ClipboardSync::kMaxFormatBytes + 1, 'x')}});
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
        QCOMPARE(a.notices->size(), 1);
    }

    void disabledDeviceNeitherSendsNorReceives()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        b.clip->setEnabled(false);
        a.clip->localChanged(text("secret"));
        moveCursor(a, a, b);
        b.clip->localChanged(text("other secret"));
        moveCursor(a, b, a);
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);
        QCOMPARE(b.applied->size(), 0);

        Settings reloadedSettings(b.config.path());
        ClipboardSync reloaded(b.node.get(), b.transfer.get(), &reloadedSettings, {}, [] { return QString(); });
        QVERIFY(!reloaded.isEnabled());
    }

    // Turning sharing back on sends the current clipboard to the trusted devices that connected
    // while it was off (they were sent nothing then).
    void enablingSharingSendsToConnectedTrustedDevices()
    {
        Device a, b;
        a.clip->localChanged(text("copied before"));
        a.clip->setEnabled(false);
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
        a.clip->setEnabled(true);
        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("copied before"));
    }

    // A paste that fails here is told, never an empty paste in silence.
    void pasteOfAMissingFormatIsNoticed()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged(text("only text"));
        QTRY_COMPARE(b.applied->size(), 1);
        QString err;
        QCOMPARE(b.unseal(QStringLiteral("image/png"), &err), QByteArray());
        QCOMPARE(err, QString("No such clipboard format"));
        QCOMPARE(b.notices->size(), 1);
        QVERIFY(b.notices->last().at(1).toBool());
    }

    // A key sent along that does not open every format (here a large one fetched later) is not
    // kept: the paste asks the master instead of failing on the format the key does not open.
    void keyThatDoesNotOpenEveryFormatIsDropped()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        a.clip->localChanged(text("hello"));
        QTRY_COMPARE(b.applied->size(), 1);
        // Forge the announcement again, newer, with a key that opens no format.
        QCborMap clip = b.lastOfType(QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), clip.value(QStringLiteral("v")).toInteger() + 1);
        clip.insert(QStringLiteral("k"), crypto::randomBytes(crypto::kKeySize));
        QVERIFY(a.node->sendMessage(b.id(), clip));
        QTRY_COMPARE(b.applied->size(), 2);
        // The master (A) holds the right key and grants B, a trusted device, at once.
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("hello"));
        QVERIFY(a.sawType(QStringLiteral("clip.unseal")));
    }

    // --- Guests ------------------------------------------------------------------------------

    // A guest gets the clipboard only when the shared cursor brings it, sealed and without the
    // key; it reads it only with a paste chord the master forwarded to it. A read with no input
    // at all (a clipboard manager on arrival) costs nothing; with input but no chord the master
    // refuses, and the user is told.
    void guestPasteNeedsTheChord()
    {
        Device a, g;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&g});

        a.clip->localChanged(text("password"));
        QTest::qWait(200);
        QCOMPARE(g.applied->size(), 0); // a guest is not sent every copy

        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QVERIFY(g.lastGated());
        QVERIFY(!g.lastOfType(QStringLiteral("clip")).contains(QStringLiteral("k"))); // no key

        // No input on G: refused here, the master is not even asked, and nobody is bothered.
        g.inputArrivedFlag = false;
        QString err;
        QCOMPARE(g.unseal(QStringLiteral("text/plain"), &err), QByteArray());
        QVERIFY(!err.isEmpty());
        QTest::qWait(100);
        QVERIFY(!a.sawType(QStringLiteral("clip.unseal")));
        QCOMPARE(g.notices->size(), 0);

        // Input but no chord (a context-menu paste): the master refuses, G's user is told.
        g.inputArrivedFlag = true;
        QCOMPARE(g.unseal(QStringLiteral("text/plain"), &err), QByteArray());
        QCOMPARE(err, QString("No paste on that device"));
        QCOMPARE(g.notices->size(), 1);
        QVERIFY(g.notices->last().at(1).toBool());

        // The chord.
        a.pasteOrderedDevice = g.id();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("password"));
    }

    // Whether reads need the grant is told again when it changes after the content arrived: a
    // guest's object is not left ungated, nor one that no longer needs the grant left gated.
    void gateFollowsTheMaster()
    {
        Device a, g;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&g});
        a.clip->localChanged(text("password"));
        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QVERIFY(g.lastGated());
        const QByteArray kid = g.lastKid();

        g.master.clear(); // nobody shares a keyboard any more
        g.clip->masterChanged();
        QCOMPARE(g.applied->size(), 2);
        QVERIFY(!g.lastGated());
        QCOMPARE(g.lastKid(), kid);
        g.clip->masterChanged(); // nothing changed: nothing told
        QCOMPARE(g.applied->size(), 2);

        g.master = a.id();
        g.clip->masterChanged();
        QCOMPARE(g.applied->size(), 3);
        QVERIFY(g.lastGated());
    }

    // A guest's granted plaintext serves only while the grant latches.
    void guestPlaintextExpiresWithTheLatch()
    {
        Device a, g;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&g});
        a.clip->localChanged(text("token"));
        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        g.inputArrivedFlag = true;
        a.pasteOrderedDevice = g.id();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("token"));

        g.inputArrivedFlag = false;
        a.pasteOrderedDevice.clear();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("token")); // within the latch

        QTest::qWait(int(ClipboardSync::kGrantLatchMs) + 200);
        const int asked = a.countType(QStringLiteral("clip.unseal"));
        QString err;
        QCOMPARE(g.unseal(QStringLiteral("text/plain"), &err), QByteArray());
        QVERIFY(!err.isEmpty());
        QCOMPARE(a.countType(QStringLiteral("clip.unseal")), asked); // not even asked

        g.inputArrivedFlag = true;
        a.pasteOrderedDevice = g.id();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("token"));
    }

    // A guest's file paste is gated the same way, and it learns the count, not the names.
    void guestFilePasteNeedsTheChord()
    {
        Device a, g;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&g});
        QTemporaryDir source;
        writeFile(source.filePath("secret.txt"), "classified");
        a.clip->localChanged({}, {source.filePath("secret.txt")});
        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QCOMPARE(g.lastPending(), 1);
        QVERIFY(g.clip->pendingFileList().isEmpty());

        // An answer that names no request (rid 0) is no grant: nothing is transferred.
        QSignalSpy transfers(g.transfer.get(), &FileTransfer::transferFinished);
        QCborMap stray;
        stray.insert(QStringLiteral("t"), QStringLiteral("clip.unsealed"));
        stray.insert(QStringLiteral("ok"), true);
        stray.insert(QStringLiteral("k"), crypto::randomBytes(crypto::kKeySize));
        QVERIFY(a.node->sendMessage(g.id(), stray));
        QTest::qWait(300);
        QCOMPARE(transfers.size(), 0);

        // No key or click at G just before (a reader that is no paste): no transfer, no request,
        // and nobody is bothered.
        g.noGesture();
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(g.paste(nullptr, 15000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);
        QTest::qWait(100);
        QVERIFY(!a.sawType(QStringLiteral("clip.unseal")));
        QCOMPARE(g.notices->size(), 0);

        g.freshGesture(); // a click, but no chord at the master: refused there, and told
        g.inputArrivedFlag = true;
        QVERIFY(g.paste(nullptr, 15000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::Other);
        QVERIFY(a.sawType(QStringLiteral("clip.unseal")));
        QCOMPARE(g.notices->size(), 1);

        a.pasteOrderedDevice = g.id();
        const QStringList files = g.paste();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(QFileInfo(files.first()).absolutePath() + "/secret.txt"), QByteArray("classified"));
    }

    // A guest is granted only the clipboard the cursor brought it: one it relayed earlier (still
    // in the master's escrow) cannot be unsealed on the strength of a paste of the current one.
    void guestIsGrantedOnlyTheClipboardItHolds()
    {
        Device a, g, b;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*g.node, *b.node));
        QVERIFY(markGuest(a, g));
        QVERIFY(markGuest(b, g));
        shareFrom(a, {&g, &b});

        // A password copied on A passes through G to B.
        a.clip->localChanged(text("password"));
        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        const QByteArray relayed = g.lastKid();
        moveCursor(a, g, b);

        // Something harmless copied on B comes to G, where the user pastes it.
        b.clip->localChanged(text("harmless"));
        QTRY_VERIFY(a.sawType(QStringLiteral("clip.key")));
        moveCursor(a, b, g);
        QTRY_COMPARE(g.applied->size(), 2);
        g.inputArrivedFlag = true;
        a.pasteOrderedDevice = g.id();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("harmless"));

        // With that grant in hand, G asks for the password's key: refused. The current one: given.
        auto ask = [&](qint64 rid, const QByteArray &kid) {
            QCborMap m;
            m.insert(QStringLiteral("t"), QStringLiteral("clip.unseal"));
            m.insert(QStringLiteral("rid"), rid);
            m.insert(QStringLiteral("kid"), kid);
            m.insert(QStringLiteral("files"), false);
            QVERIFY(g.node->sendMessage(a.id(), m));
        };
        ask(777, relayed);
        QTRY_VERIFY(g.unsealAnswer(777).has_value());
        QCOMPARE(g.unsealAnswer(777)->value(QStringLiteral("ok")).toBool(), false);
        ask(778, g.lastKid());
        QTRY_VERIFY(g.unsealAnswer(778).has_value());
        QCOMPARE(g.unsealAnswer(778)->value(QStringLiteral("ok")).toBool(), true);
        QVERIFY(!g.unsealAnswer(778)->value(QStringLiteral("keep")).toBool()); // a guest keeps nothing
    }

    // A grant for one clipboard does not open another device's copied files: the publisher lets a
    // device in only for a paste of its own current clipboard, whatever offer the request names.
    void grantForOneClipboardDoesNotOpenAnothersFiles()
    {
        Device a, p, g;
        QVERIFY(test::pairNodes(*a.node, *p.node));
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(test::pairNodes(*p.node, *g.node));
        QVERIFY(markGuest(a, g));
        QVERIFY(markGuest(p, g));
        shareFrom(a, {&p, &g});
        QTemporaryDir source;
        writeFile(source.filePath("secret.txt"), "classified");

        // P copies files; the cursor passes through G (which learns the offer) to the master.
        p.clip->localChanged({}, {source.filePath("secret.txt")});
        QTRY_VERIFY(a.sawType(QStringLiteral("clip.key")));
        moveCursor(a, p, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QCOMPARE(g.lastPending(), 1);
        const quint64 secretOffer = g.lastAnnouncedFileOffer();
        QVERIFY(secretOffer != 0);
        moveCursor(a, g, a);
        QTest::qWait(100);

        // The master copies text and brings it to G, where the user pastes it: a legitimate grant.
        a.clip->localChanged(text("harmless"));
        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 2);
        g.inputArrivedFlag = true;
        a.pasteOrderedDevice = g.id();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("harmless"));

        // A modified client on G names P's files in a files request for the text's clipboard.
        QCborMap m;
        m.insert(QStringLiteral("t"), QStringLiteral("clip.unseal"));
        m.insert(QStringLiteral("rid"), 55);
        m.insert(QStringLiteral("kid"), g.lastKid());
        m.insert(QStringLiteral("files"), true);
        m.insert(QStringLiteral("p"), p.id());
        m.insert(QStringLiteral("o"), qint64(secretOffer));
        QVERIFY(g.node->sendMessage(a.id(), m));
        QTRY_VERIFY(g.unsealAnswer(55).has_value());
        QCOMPARE(g.unsealAnswer(55)->value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(g.unsealAnswer(55)->value(QStringLiteral("r")).toString(), QString("Not allowed"));

        // And P never let G in.
        FetchResult result;
        bool done = false;
        QTemporaryDir target;
        g.transfer->fetch(p.id(), secretOffer, target.path(), true, 1 << 20, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("No such offer"));
    }

    // The author's key and the guest's request race on different channels; the master holds the
    // request briefly so the paste still succeeds.
    void keyArrivingAfterTheRequestIsHonoured()
    {
        Device a, b, g;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(test::pairNodes(*b.node, *g.node));
        QVERIFY(markGuest(a, g));
        QVERIFY(markGuest(b, g));
        shareFrom(a, {&b, &g});
        moveCursor(a, a, b);
        b.clip->localChanged(text("raced"));
        moveCursor(a, b, g);
        QTRY_COMPARE(g.applied->size(), 1);
        g.inputArrivedFlag = true;
        a.pasteOrderedDevice = g.id();
        QTRY_VERIFY(g.unseal(QStringLiteral("text/plain")) == QByteArray("raced"));
    }

    // The key of a clipboard copied before sharing moved follows the master: the author escrows it
    // again with the new one, which can then grant a guest's paste.
    void keyFollowsTheMaster()
    {
        Device a, b, m2, g;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*b.node, *m2.node));
        QVERIFY(test::pairNodes(*m2.node, *g.node));
        QVERIFY(test::pairNodes(*b.node, *g.node));
        QVERIFY(markGuest(m2, g));
        QVERIFY(markGuest(b, g));
        shareFrom(a, {&b});
        b.clip->localChanged(text("from B"));
        QTRY_VERIFY(a.sawType(QStringLiteral("clip.key")));

        shareFrom(m2, {&a, &b, &g});
        for (Device *d : {&a, &b, &m2, &g})
            d->clip->masterChanged();
        QTRY_VERIFY(m2.sawType(QStringLiteral("clip.key")));

        moveCursor(m2, b, g);
        QTRY_COMPARE(g.applied->size(), 1);
        g.inputArrivedFlag = true;
        m2.pasteOrderedDevice = g.id();
        QCOMPARE(g.unseal(QStringLiteral("text/plain")), QByteArray("from B"));
    }

    // Content that came without the key needs the master: unreachable, the paste fails at once;
    // an older one, it fails at once naming the device to update.
    void contentWithoutTheKeyNeedsAReachableMaster()
    {
        Device a, b, c;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*a.node, *c.node));
        QVERIFY(test::pairNodes(*b.node, *c.node));
        QVERIFY(markGuest(b, c)); // B gives C its clipboard without the key
        shareFrom(a, {&b, &c});
        b.clip->localChanged(text("stuck"));
        moveCursor(a, b, c);
        QTRY_COMPARE(c.applied->size(), 1);
        QVERIFY(!c.lastGated()); // the master trusts C: no evidence needed, only the key

        // The master trusts C: granted at once, with no chord.
        QCOMPARE(c.unseal(QStringLiteral("text/plain")), QByteArray("stuck"));

        // A newer copy on B, and a master that is not reachable.
        b.clip->localChanged(text("stuck again"));
        moveCursor(a, b, c);
        QTRY_COMPARE(c.applied->size(), 2);
        c.master = QByteArray(16, 'z');
        QString err;
        QCOMPARE(c.unseal(QStringLiteral("text/plain"), &err), QByteArray());
        QVERIFY(!err.isEmpty());

        // A master that claims an older version.
        c.master = a.id();
        a.claimVersionTo(c, QStringLiteral("0.1.19"));
        QTest::qWait(200);
        QElapsedTimer elapsed;
        elapsed.start();
        QCOMPARE(c.unseal(QStringLiteral("text/plain"), &err), QByteArray());
        QVERIFY(elapsed.elapsed() < 1000);
        QVERIFY2(err.startsWith(QStringLiteral("Update DeskFlower on")), qPrintable(err));
        QVERIFY(!c.notices->isEmpty());
    }

    // A guest replaces a clipboard only through a hand-off the master announced.
    void guestSendsNothingUnlessTheCursorBringsIt()
    {
        Device m, b, g;
        QVERIFY(test::pairNodes(*m.node, *b.node));
        QVERIFY(test::pairNodes(*m.node, *g.node));
        QVERIFY(test::pairNodes(*b.node, *g.node));
        b.node->setTrust(g.id(), TrustLevel::Guest);
        shareFrom(m, {&b, &g});

        g.clip->localChanged(text("from the guest")); // G sends it to B (G trusts B)
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
        QCOMPARE(b.notices->size(), 0); // a copy sent to everyone: refused quietly

        moveCursor(m, g, b); // now the cursor brings it
        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("from the guest"));
    }

    void masterFlaggedAsGuestIsIgnored()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(b, {&a}); // B is the master, A a slave
        a.node->setTrust(b.id(), TrustLevel::Guest);
        a.cursorHere = true;
        b.clip->localChanged(text("from the ex-master"));
        b.clip->cursorMoved(b.id(), a.id());
        QTest::qWait(400);
        QCOMPARE(a.applied->size(), 0); // a guest master is not obeyed
    }

    // Only the master orchestrates hand-offs between devices that are guests to each other.
    void noHandOffWithoutTheMaster()
    {
        Device a, b, c;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*b.node, *c.node));
        QVERIFY(test::pairNodes(*a.node, *c.node));
        b.node->setTrust(c.id(), TrustLevel::Guest);
        c.node->setTrust(b.id(), TrustLevel::Guest);
        shareFrom(a, {&b, &c});
        b.clip->localChanged(text("secret on B"));
        c.clip->localChanged(text("text on C"));
        const int bBefore = b.applied->size(); // A's copies, if any
        const int cBefore = c.applied->size();
        b.cursorHere = true;
        c.cursorHere = true;
        c.clip->cursorMoved(b.id(), c.id());
        c.clip->cursorMoved(c.id(), b.id());
        QTest::qWait(300);
        QCOMPARE(c.applied->size(), cBefore);
        QCOMPARE(b.applied->size(), bBefore);
    }

    // The escrow is bounded, and a guest's key goes first: copying on a guest cannot push the
    // keys of trusted devices out.
    void guestKeyDoesNotDisplaceTrustedKeys()
    {
        Device a, b, c, d, g;
        for (Device *x : {&b, &c, &d, &g})
            QVERIFY(test::pairNodes(*a.node, *x->node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&b, &c, &d, &g});
        b.clip->localChanged(text("b"));
        QTRY_COMPARE(a.countType(QStringLiteral("clip.key")), 1);
        const QByteArray bKid = a.lastOfType(QStringLiteral("clip.key")).value(QStringLiteral("kid")).toByteArray();
        c.clip->localChanged(text("c"));
        d.clip->localChanged(text("d"));
        QTRY_COMPARE(a.countType(QStringLiteral("clip.key")), 3);
        a.clip->localChanged(text("a")); // the master's own: the fourth
        g.clip->localChanged(text("g")); // a fifth, from the guest
        QTRY_COMPARE(a.countType(QStringLiteral("clip.key")), 4);
        const QByteArray gKid = a.lastOfType(QStringLiteral("clip.key")).value(QStringLiteral("kid")).toByteArray();

        auto ask = [&](qint64 rid, const QByteArray &kid) {
            QCborMap m;
            m.insert(QStringLiteral("t"), QStringLiteral("clip.unseal"));
            m.insert(QStringLiteral("rid"), rid);
            m.insert(QStringLiteral("kid"), kid);
            m.insert(QStringLiteral("files"), false);
            QVERIFY(d.node->sendMessage(a.id(), m));
        };
        ask(1, bKid); // the oldest, still there
        QTRY_VERIFY(d.unsealAnswer(1).has_value());
        QCOMPARE(d.unsealAnswer(1)->value(QStringLiteral("ok")).toBool(), true);
        ask(2, gKid); // the guest's went
        QTRY_VERIFY_WITH_TIMEOUT(d.unsealAnswer(2).has_value(), 3000);
        QCOMPARE(d.unsealAnswer(2)->value(QStringLiteral("ok")).toBool(), false);
    }

    // Requests for keys nobody sent are held a moment, a bounded number of them, and all answered.
    void heldUnsealsAreBounded()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        const int n = ClipboardSync::kMaxHeldUnseals + 4;
        for (int i = 1; i <= n; ++i) {
            QCborMap m;
            m.insert(QStringLiteral("t"), QStringLiteral("clip.unseal"));
            m.insert(QStringLiteral("rid"), i);
            m.insert(QStringLiteral("kid"), crypto::randomBytes(16));
            QVERIFY(b.node->sendMessage(a.id(), m));
        }
        // The ones over the bound are refused at once, the rest once the wait is over.
        QTRY_VERIFY_WITH_TIMEOUT(b.countType(QStringLiteral("clip.unsealed")) >= 4, ClipboardSync::kKeyWaitMs - 100);
        QTRY_COMPARE(b.countType(QStringLiteral("clip.unsealed")), n);
        for (int i = 1; i <= n; ++i)
            QCOMPARE(b.unsealAnswer(i)->value(QStringLiteral("ok")).toBool(), false);
    }

    // --- Older peers -------------------------------------------------------------------------

    // A peer from before sealing is sent the clipboard as 0.1.19 understands it (plaintext, one
    // offer with the large formats and the files) at a hand-off: degraded, not dropped.
    void olderPeerGetsThePlaintextFormat()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        b.claimVersionTo(a, QStringLiteral("0.1.19"));
        QTest::qWait(200);
        QByteArray image(200 * 1024, Qt::Uninitialized);
        for (int i = 0; i < image.size(); ++i)
            image[i] = char(i * 7 + 3);
        QTemporaryDir source;
        writeFile(source.filePath("doc.txt"), "for the old one");
        ClipboardData data = text("plain for 0.1.19");
        data.insert(QStringLiteral("image/png"), image);
        a.clip->localChanged(data, {source.filePath("doc.txt")});
        QTest::qWait(200);
        QCOMPARE(b.applied->size(), 0); // not sent every copy

        moveCursor(a, a, b);
        QTRY_VERIFY(b.sawType(QStringLiteral("clip")));
        const QCborMap clip = b.lastOfType(QStringLiteral("clip"));
        QVERIFY(!clip.contains(QStringLiteral("kid")));
        QVERIFY(!clip.contains(QStringLiteral("k")));
        QCOMPARE(clip.value(QStringLiteral("f")).toMap().value(QStringLiteral("text/plain")).toByteArray(),
                 QByteArray("plain for 0.1.19"));
        QCOMPARE(clip.value(QStringLiteral("big")).toMap().value(QStringLiteral("image/png")).toInteger(), image.size());
        QCOMPARE(clip.value(QStringLiteral("fc")).toInteger(), 1);
        QVERIFY(clip.value(QStringLiteral("o")).toInteger() != 0);

        // B (really a new node, which still understands that format) takes all of it.
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("plain for 0.1.19"));
        QCOMPARE(b.unseal(QStringLiteral("image/png")), image);
        const QStringList files = b.paste();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), QByteArray("for the old one"));
        QVERIFY(a.notices->isEmpty());
    }

    // What cannot be delivered to an older peer is said: to an older guest (it knows no grant),
    // once per while.
    void undeliverableToAnOlderPeerIsNoticed()
    {
        Device a, g;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&g});
        g.claimVersionTo(a, QStringLiteral("0.1.19"));
        QTest::qWait(200);
        a.clip->localChanged(text("for g"));
        moveCursor(a, a, g);
        QTRY_COMPARE(a.notices->size(), 1);
        QVERIFY(a.notices->first().at(1).toBool());
        QVERIFY2(a.notices->first().at(0).toString().contains(g.node->name()),
                 qPrintable(a.notices->first().at(0).toString()));
        moveCursor(a, g, a);
        moveCursor(a, a, g);
        QTest::qWait(200);
        QCOMPARE(a.notices->size(), 1); // not repeated at every crossing
        QCOMPARE(g.applied->size(), 0);
    }

    // A peer older than kFoldersVersion (0.1.19, no sealing) gets the files' list without folder
    // entries, and no folder count; it is not sent every copy either.
    void olderPeerIsListedTheFilesOnly()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        b.claimVersionTo(a, QStringLiteral("0.1.19"));
        QTest::qWait(200);
        QTemporaryDir source;
        writeFile(source.filePath("dir/a.txt"), "a");
        QVERIFY(QDir().mkpath(source.filePath("dir/empty")));
        a.clip->localChanged({}, {source.filePath("dir")});
        QTest::qWait(200);
        QVERIFY(!b.sawType(QStringLiteral("clip")));
        moveCursor(a, a, b);
        QTRY_VERIFY(b.sawType(QStringLiteral("clip")));
        const QCborMap clip = b.lastOfType(QStringLiteral("clip"));
        QVERIFY(!clip.contains(QStringLiteral("dc")));
        QCOMPARE(clip.value(QStringLiteral("fc")).toInteger(), 1);
        const QCborArray fl = clip.value(QStringLiteral("fl")).toArray();
        QCOMPARE(fl.size(), 1);
        QCOMPARE(fl.at(0).toArray().at(0).toString(), QString("dir/a.txt"));
    }

    // A plaintext clipboard from an older peer (no id) is still understood: it is re-sealed locally
    // and pasteable without a round trip.
    void legacyPlaintextClipIsStillAccepted()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        b.cursorHere = true;

        QCborMap clip;
        clip.insert(QStringLiteral("t"), QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), 5);
        clip.insert(QStringLiteral("a"), a.id());
        QCborMap formats;
        formats.insert(QStringLiteral("text/plain"), QByteArray("legacy"));
        clip.insert(QStringLiteral("f"), formats);
        QVERIFY(a.node->sendMessage(b.id(), clip));

        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("legacy"));
    }

    // Plaintext from an older peer, re-sealed by the device that received it, is escrowed with the
    // master under that device's identity, and handed on with its key to trusted devices.
    void legacyPlaintextIsEscrowedByTheDeviceThatSealedIt()
    {
        Device m, a, b;
        QVERIFY(test::pairNodes(*m.node, *a.node));
        QVERIFY(test::pairNodes(*m.node, *b.node));
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(m, {&a, &b});

        moveCursor(m, a, b);
        QCborMap clip;
        clip.insert(QStringLiteral("t"), QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), 5);
        clip.insert(QStringLiteral("a"), a.id());
        QCborMap formats;
        formats.insert(QStringLiteral("text/plain"), QByteArray("legacy"));
        clip.insert(QStringLiteral("f"), formats);
        QVERIFY(a.node->sendMessage(b.id(), clip));
        QTRY_COMPARE(b.applied->size(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("legacy"));
        QTRY_VERIFY(m.sawType(QStringLiteral("clip.key")));

        moveCursor(m, b, m);
        QTRY_COMPARE(m.applied->size(), 1);
        QCOMPARE(m.unseal(QStringLiteral("text/plain")), QByteArray("legacy"));
    }

    // An older peer announces large formats and copied files in one open offer: both are taken
    // (the formats fetched and re-sealed here, the files transferred at the paste).
    void legacyClipWithLargeFormatsAndFilesIsStillTaken()
    {
        Device m, x, b;
        QVERIFY(test::pairNodes(*m.node, *x.node));
        QVERIFY(test::pairNodes(*m.node, *b.node));
        QVERIFY(test::pairNodes(*x.node, *b.node));
        shareFrom(m, {&x, &b});
        x.claimVersionTo(m, QStringLiteral("0.1.19"));
        QTest::qWait(200);

        QByteArray image(200 * 1024, Qt::Uninitialized);
        for (int i = 0; i < image.size(); ++i)
            image[i] = char(i * 13 + 1);
        QTemporaryDir source;
        writeFile(source.filePath("doc.txt"), "old");
        Offer offer; // as an older peer publishes it: blobs and files together, open to any peer
        offer.blobs.insert(QStringLiteral("image/png"), image);
        QString error;
        QVERIFY2(Offer::addPaths(&offer, {source.filePath("doc.txt")}, 1 << 20, 10, &error), qPrintable(error));
        const quint64 offerId = x.transfer->publish(offer);

        moveCursor(m, x, b);
        QCborMap clip;
        clip.insert(QStringLiteral("t"), QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), 5);
        clip.insert(QStringLiteral("a"), x.id());
        QCborMap formats;
        formats.insert(QStringLiteral("text/plain"), QByteArray("caption"));
        clip.insert(QStringLiteral("f"), formats);
        QCborMap big;
        big.insert(QStringLiteral("image/png"), qint64(image.size()));
        clip.insert(QStringLiteral("big"), big);
        clip.insert(QStringLiteral("fc"), 1);
        clip.insert(QStringLiteral("fb"), 3);
        clip.insert(QStringLiteral("fl"), QCborArray{QCborArray{QStringLiteral("doc.txt"), 3}});
        clip.insert(QStringLiteral("o"), qint64(offerId));
        QVERIFY(x.node->sendMessage(b.id(), clip));

        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QVERIFY(b.lastMimes().contains(QStringLiteral("text/plain")));
        QVERIFY(b.lastMimes().contains(QStringLiteral("image/png")));
        QCOMPARE(b.lastPending(), 1);
        QCOMPARE(b.unseal(QStringLiteral("text/plain")), QByteArray("caption"));
        QCOMPARE(b.unseal(QStringLiteral("image/png")), image);
        QTRY_VERIFY(m.sawType(QStringLiteral("clip.key")));

        const QStringList files = b.paste();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), QByteArray("old"));
    }

    // Only copied files from an older peer (no plaintext, so no kid here): pasted after a press,
    // refused without one.
    void legacyFilesOnlyClipPastesWithAPress()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        a.claimVersionTo(b, QStringLiteral("0.1.19"));
        QTest::qWait(200);
        QTemporaryDir source;
        writeFile(source.filePath("doc.txt"), "old");
        Offer offer; // as an older peer publishes it: open to any peer
        QString error;
        QVERIFY2(Offer::addPaths(&offer, {source.filePath("doc.txt")}, 1 << 20, 10, &error), qPrintable(error));
        const quint64 offerId = a.transfer->publish(offer);

        b.cursorHere = true;
        QCborMap clip;
        clip.insert(QStringLiteral("t"), QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), 5);
        clip.insert(QStringLiteral("a"), a.id());
        clip.insert(QStringLiteral("fc"), 1);
        clip.insert(QStringLiteral("fb"), 3);
        clip.insert(QStringLiteral("fl"), QCborArray{QCborArray{QStringLiteral("doc.txt"), 3}});
        clip.insert(QStringLiteral("o"), qint64(offerId));
        QVERIFY(a.node->sendMessage(b.id(), clip));
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.lastPending(), 1);

        b.noGesture();
        ClipboardSync::FetchError code = ClipboardSync::FetchError::None;
        QVERIFY(b.paste(nullptr, 5000, &code).isEmpty());
        QCOMPARE(code, ClipboardSync::FetchError::NoPaste);

        b.freshGesture();
        const QStringList files = b.paste(&error, 15000, &code);
        QVERIFY2(files.size() == 1, qPrintable(error));
        QCOMPARE(code, ClipboardSync::FetchError::None);
        QCOMPARE(readFile(files.first()), QByteArray("old"));
    }

    // A hostile announcement cannot inflate what the receiver fetches or keeps.
    void hostileAnnouncementsAreBounded()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        b.cursorHere = true;
        QSignalSpy filesNotices(b.clip.get(), &ClipboardSync::filesNotice);

        Offer offer;
        offer.gated = true;
        offer.blobs.insert(QStringLiteral("image/png"), QByteArray(100, 'p'));
        const quint64 offerId = a.transfer->publish(offer);
        a.transfer->allowPeer(offerId, b.id(), true);
        QCborMap big;
        for (int i = 0; i < 200; ++i)
            big.insert(QStringLiteral("x/invented-%1").arg(i), qint64(1) << 40);
        big.insert(QStringLiteral("image/png"), qint64(1) << 62);
        QCborMap sealedInline;
        sealedInline.insert(QStringLiteral("text/plain"), QByteArray("sealed-bytes"));
        QCborMap clip;
        clip.insert(QStringLiteral("t"), QStringLiteral("clip"));
        clip.insert(QStringLiteral("v"), 5);
        clip.insert(QStringLiteral("a"), a.id());
        clip.insert(QStringLiteral("kid"), crypto::randomBytes(16));
        clip.insert(QStringLiteral("k"), crypto::randomBytes(crypto::kKeySize)); // a key that opens nothing
        clip.insert(QStringLiteral("o"), qint64(offerId));
        clip.insert(QStringLiteral("sf"), sealedInline);
        clip.insert(QStringLiteral("big"), big);
        clip.insert(QStringLiteral("fc"), 3);
        clip.insert(QStringLiteral("fb"), qint64(1) << 50);
        QVERIFY(a.node->sendMessage(b.id(), clip));

        QTRY_VERIFY_WITH_TIMEOUT(!b.applied->isEmpty(), 15000);
        QVERIFY(b.lastMimes().contains(QStringLiteral("text/plain")));
        QVERIFY(b.lastMimes().contains(QStringLiteral("image/png")));
        QCOMPARE(b.lastPending(), 0);
        QCOMPARE(filesNotices.size(), 1);
        QVERIFY(filesNotices.first().at(1).toBool());
    }

    // --- Listing copied files (sp, listFiles) ------------------------------------------------

    // A copy whose paths are all short is announced with sp to trusted devices (which may then
    // paste it before its contents arrive), never to a guest (nor its file names).
    void shortPathsAreAnnouncedButNotToGuests()
    {
        Device a, b, g;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&b, &g});
        QTemporaryDir source;
        writeFile(source.filePath("docs/a.txt"), "a");
        writeFile(source.filePath("docs/sub/b.txt"), "b");
        a.clip->localChanged({}, {source.filePath("docs")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.lastOfType(QStringLiteral("clip")).value(QStringLiteral("sp")).toBool(), true);
        QVERIFY(b.lastOfType(QStringLiteral("clip")).contains(QStringLiteral("fl")));
        QVERIFY(b.clip->filesStreamable());

        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QCOMPARE(g.lastPending(), 4);
        const QCborMap toGuest = g.lastOfType(QStringLiteral("clip"));
        QVERIFY(!toGuest.contains(QStringLiteral("sp")));
        QVERIFY(!toGuest.contains(QStringLiteral("fl")));
        QVERIFY(!g.clip->filesStreamable());
    }

    // No sp without files, nor for a copy with a path of 260 UTF-16 units or more: such a copy is
    // listed (short list) all the same, but listing it for a streaming paste is refused.
    void shortPathsAbsentWhenNotKnownShort()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged(text("no files"));
        QTRY_COMPARE(b.applied->size(), 1);
        QVERIFY(!b.lastOfType(QStringLiteral("clip")).contains(QStringLiteral("sp")));
        QVERIFY(!b.clip->filesStreamable());

        QTemporaryDir source;
        const QString top(200, QLatin1Char('t'));
        const QString sub(70, QLatin1Char('s'));
        const QString longFile = source.filePath(top + "/" + sub + "/f.txt"); // relative: 277 units
        if (!QDir().mkpath(QFileInfo(longFile).absolutePath()))
            QSKIP("This file system does not take paths this long");
        writeFile(longFile, "deep");
        if (!QFileInfo::exists(longFile))
            QSKIP("This file system does not take paths this long");
        a.clip->localChanged({}, {source.filePath(top)});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 2, 15000);
        QCOMPARE(b.lastPending(), 3);
        QVERIFY(!b.lastOfType(QStringLiteral("clip")).contains(QStringLiteral("sp")));
        QCOMPARE(b.clip->pendingFileList().size(), 3);
        // Known from the announcement's list: not streamable from the first apply.
        QVERIFY(!b.clip->filesStreamable());
        ListAnswer answer;
        b.clip->listFiles(b.lastKid(), answer.callback());
        QVERIFY(answer.answered);
        QVERIFY(!answer.ok);
        QCOMPARE(b.applied->size(), 2); // already applied as pasted once transferred
        QVERIFY(!b.clip->filesStreamable());

        // This device's own copy is never listed (nor re-applied).
        const qsizetype appliedOnA = a.applied->size();
        ListAnswer own;
        a.clip->listFiles(b.lastKid(), own.callback()); // the kid of a's copy
        QVERIFY(own.answered);
        QVERIFY(!own.ok);
        QCOMPARE(a.applied->size(), appliedOnA);
    }

    // A receiver that predates sp (0.1.22-0.1.29) is sent it like any key it ignores, and its
    // paste is the paste it always was: today's get, the whole copy.
    void receiverFromBeforeShortPathsStillPastes()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.claimVersionTo(a, QStringLiteral("0.1.22"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                for (const PeerView &p : a.node->peers()) {
                    if (p.id == b.id())
                        return p.version == QLatin1String("0.1.22");
                }
                return false;
            },
            5000));
        QTemporaryDir source;
        writeFile(source.filePath("notes.txt"), "remember the milk");
        a.clip->localChanged({}, {source.filePath("notes.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.lastOfType(QStringLiteral("clip")).value(QStringLiteral("sp")).toBool(), true);
        QString err;
        const QStringList files = b.paste(&err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), QByteArray("remember the milk"));
    }

    // A copy too large to list in its announcement (19,300 entries) is listed from its publisher
    // once per clipboard: requests meanwhile join that one list fetch, later ones are answered
    // from it, and a newer clipboard lists again. A list carries no contents.
    void listFilesIsCachedPerStamp()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        const QByteArray kid = announceFiles(a, b, syntheticCopy(300, 19000), 5);
        QVERIFY(!kid.isEmpty());
        QCOMPARE(b.lastPending(), 19300);
        QVERIFY(b.clip->pendingFileList().isEmpty());
        QVERIFY(b.clip->filesStreamable()); // sp, and within kMaxVirtualEntries

        ListAnswer first, second;
        b.clip->listFiles(kid, first.callback());
        b.clip->listFiles(kid, second.callback());
        QVERIFY(!first.answered && !second.answered);
        QTRY_VERIFY_WITH_TIMEOUT(first.answered && second.answered, 30000);
        QVERIFY2(first.ok, qPrintable(first.error));
        QVERIFY(second.ok);
        QCOMPARE(first.items.size(), 19300);
        QCOMPARE(listedDirCount(first.items), 300);
        QCOMPARE(first.items.first().path, QString("tree"));
        QCOMPARE(second.items.size(), 19300);
        QCOMPARE(streams.size(), 1);

        ListAnswer third; // a second descriptor read: no network at all
        b.clip->listFiles(kid, third.callback());
        QVERIFY(third.answered);
        QVERIFY(third.ok);
        QCOMPARE(third.items.size(), 19300);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 1);
        QCOMPARE(b.clip->pendingFileList().size(), 0); // the announcement's list stays empty
        QTRY_COMPARE_WITH_TIMEOUT(a.transfer->liveUploadsForTests(), 0, 10000);
        QVERIFY(!QDir(b.downloads.filePath("Clipboard")).exists()); // nothing received

        // A newer clipboard: the old kid is stale, the new one listed afresh.
        const QByteArray kid2 = announceFiles(a, b, syntheticCopy(3, 5), 6);
        QVERIFY(!kid2.isEmpty());
        ListAnswer stale, fresh;
        b.clip->listFiles(kid, stale.callback());
        QVERIFY(stale.answered);
        QVERIFY(!stale.ok);
        QCOMPARE(stale.error, QString("The clipboard changed"));
        b.clip->listFiles(kid2, fresh.callback());
        QTRY_VERIFY_WITH_TIMEOUT(fresh.answered, 15000);
        QVERIFY2(fresh.ok, qPrintable(fresh.error));
        QCOMPARE(fresh.items.size(), 8);
        QCOMPARE(streams.size(), 2);

        // A clipboard replaced while its list is on its way: the waiter hears so, quietly.
        const QByteArray kid3 = announceFiles(a, b, syntheticCopy(300, 19000), 7);
        QVERIFY(!kid3.isEmpty());
        ListAnswer replaced;
        b.clip->listFiles(kid3, replaced.callback());
        b.clip->localChanged(text("copied here"));
        QVERIFY(replaced.answered);
        QVERIFY(!replaced.ok);
        QCOMPARE(replaced.error, QString("The clipboard changed"));
        QCOMPARE(b.notices->size(), 0);
    }

    // No list for a device the publisher did not let in (a guest's copy, or fa missing): a
    // guest's paste goes through the master's grant, after which the files are transferred.
    void listFilesRefusedWhenNotLetIn()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        const QByteArray kid = announceFiles(a, b, syntheticCopy(10, 100), 5, true, false);
        QVERIFY(!kid.isEmpty());
        QVERIFY(!b.clip->filesStreamable());
        ListAnswer answer;
        b.clip->listFiles(kid, answer.callback());
        QVERIFY(answer.answered);
        QVERIFY(!answer.ok);
        QVERIFY(!answer.error.isEmpty());
        QTest::qWait(200);
        QCOMPARE(streams.size(), 0);

        Device m, g;
        QVERIFY(test::pairNodes(*m.node, *g.node));
        QVERIFY(markGuest(m, g));
        shareFrom(m, {&g});
        QTemporaryDir source;
        writeFile(source.filePath("secret.txt"), "classified");
        m.clip->localChanged({}, {source.filePath("secret.txt")});
        moveCursor(m, m, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QSignalSpy guestStreams(m.node.get(), &Node::streamAccepted);
        ListAnswer guest;
        g.clip->listFiles(g.lastKid(), guest.callback());
        QVERIFY(guest.answered);
        QVERIFY(!guest.ok);
        QTest::qWait(200);
        QCOMPARE(guestStreams.size(), 0);
    }

    // A publisher whose list breaks what it announced (sp with a long path, or other counts) gets
    // no streaming paste: the list is refused and the clipboard applied again, to be pasted once
    // transferred. Later requests are refused without asking again.
    void listViolationFallsBackToDeferred()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        Offer lying = syntheticCopy(2, 1);
        lying.dirs.append(QStringLiteral("long/") + QString(255, QLatin1Char('x'))); // 260 units
        const QByteArray kid = announceFiles(a, b, lying, 5);
        QVERIFY(!kid.isEmpty());
        QVERIFY(b.clip->filesStreamable()); // as announced
        const qsizetype applied = b.applied->size();
        ListAnswer answer;
        b.clip->listFiles(kid, answer.callback());
        QTRY_VERIFY_WITH_TIMEOUT(answer.answered, 15000);
        QVERIFY(!answer.ok);
        QVERIFY2(answer.error.startsWith("A copied item's path is too long"), qPrintable(answer.error));
        QCOMPARE(b.applied->size(), applied + 1);
        QCOMPARE(b.lastKid(), kid);
        QCOMPARE(b.lastPending(), 4);
        QVERIFY(!b.clip->filesStreamable());
        ListAnswer again;
        b.clip->listFiles(kid, again.callback());
        QVERIFY(again.answered);
        QVERIFY(!again.ok);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 1);

        // Counts other than those announced.
        const QByteArray kid2 = announceFiles(a, b, syntheticCopy(3, 4), 6, true, true, 3, 3);
        QVERIFY(!kid2.isEmpty());
        ListAnswer mismatch;
        b.clip->listFiles(kid2, mismatch.callback());
        QTRY_VERIFY_WITH_TIMEOUT(mismatch.answered, 15000);
        QVERIFY(!mismatch.ok);
        QCOMPARE(mismatch.error, QString("The copied files are not those announced"));
        QVERIFY(!b.clip->filesStreamable());

        // Without sp an unlisted copy is not streamable to begin with, and not listed either: no
        // stream, no re-apply.
        const QByteArray kid3 = announceFiles(a, b, syntheticCopy(3, 4), 7, false);
        QVERIFY(!kid3.isEmpty());
        QVERIFY(!b.clip->filesStreamable());
        const qsizetype streamsBefore = streams.size();
        const qsizetype appliedBefore = b.applied->size();
        ListAnswer unlisted;
        b.clip->listFiles(kid3, unlisted.callback());
        QVERIFY(unlisted.answered);
        QVERIFY(!unlisted.ok);
        QTest::qWait(200);
        QCOMPARE(streams.size(), streamsBefore);
        QCOMPARE(b.applied->size(), appliedBefore);
    }

    // A manifest the transfer refuses as such (here two paths that differ only in case, on a file
    // system that ignores case) is a refused list like any other: the clipboard is applied again
    // without the streaming paste, and later requests do not ask the publisher again.
    void listRefusedByManifestFallsBackToDeferred()
    {
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSKIP("Paths that differ only in case are refused only where case is ignored");
#endif
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        Offer twins;
        twins.gated = true;
        twins.dirs.append(QStringLiteral("tree"));
        twins.files.append(Offer::File{QStringLiteral("/nowhere/x"), QStringLiteral("tree/a.txt"), 10});
        twins.files.append(Offer::File{QStringLiteral("/nowhere/x"), QStringLiteral("tree/A.txt"), 10});
        const QByteArray kid = announceFiles(a, b, twins, 5);
        QVERIFY(!kid.isEmpty());
        QVERIFY(b.clip->filesStreamable()); // as announced
        const qsizetype applied = b.applied->size();
        ListAnswer answer;
        b.clip->listFiles(kid, answer.callback());
        QTRY_VERIFY_WITH_TIMEOUT(answer.answered, 15000);
        QVERIFY(!answer.ok);
        QVERIFY2(answer.error.startsWith("Two copied items differ only in upper and lower case"),
                 qPrintable(answer.error));
        QCOMPARE(b.applied->size(), applied + 1);
        QCOMPARE(b.lastKid(), kid);
        QVERIFY(!b.clip->filesStreamable());
        ListAnswer again;
        b.clip->listFiles(kid, again.callback());
        QVERIFY(again.answered);
        QVERIFY(!again.ok);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 1);
    }

    // A ClipboardSync destroyed with a list on its way: no callback into it afterwards, and the
    // publisher's upload ends.
    void listInFlightAtDestruction()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        const QByteArray kid = announceFiles(a, b, syntheticCopy(300, 19000), 5);
        QVERIFY(!kid.isEmpty());
        ListAnswer answer;
        b.clip->listFiles(kid, answer.callback());
        QVERIFY(!answer.answered);
        b.applied.reset();
        b.notices.reset();
        b.clip.reset();
        QTest::qWait(500);
        QVERIFY(!answer.answered);
        QTRY_COMPARE_WITH_TIMEOUT(a.transfer->liveUploadsForTests(), 0, 10000);
    }

    // --- Streaming pastes, detached transfers, Receive -----------------------------------------

    // A streaming paste (fetchFiles with streaming) needs a paste like any other. With one it is
    // given the transfer's folder before the transfer ends, a second one joins the same transfer,
    // and one after the transfer ended is answered at once with the same folder (no transfer).
    // The folder stays pinned while any of them is there.
    void streamingPasteIsGivenItsFolderFirst()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("docs/a.txt"), "alpha");
        writeFile(source.filePath("docs/sub/b.txt"), "beta");
        a.clip->localChanged({}, {source.filePath("docs")});
        QTRY_VERIFY_WITH_TIMEOUT(b.lastPending() > 0, 15000);
        QVERIFY(b.clip->filesStreamable());
        const QByteArray kid = b.lastKid();
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);

        b.noGesture();
        FilesAnswer refused;
        QString refusedRoot;
        b.clip->fetchFiles(refused.callback(), kid, [&](const QString &r) { refusedRoot = r; }, true);
        QTRY_VERIFY(refused.answered);
        QCOMPARE(refused.code, ClipboardSync::FetchError::NoPaste);
        QVERIFY(refusedRoot.isEmpty());
        QVERIFY(b.clip->pinnedTargets().isEmpty());

        b.freshGesture();
        FilesAnswer first, second;
        QStringList roots;
        bool startedFirst = false;
        const quint64 id1 = b.clip->fetchFiles(
            first.callback(), kid,
            [&](const QString &r) {
                roots << r;
                startedFirst = !first.answered;
            },
            true);
        const quint64 id2 = b.clip->fetchFiles(second.callback(), kid, [&](const QString &r) { roots << r; }, true);
        QVERIFY(id1 != 0 && id2 != 0 && id1 != id2);
        QTRY_VERIFY_WITH_TIMEOUT(first.answered && second.answered, 15000);
        QVERIFY2(first.ok, qPrintable(first.error));
        QVERIFY(second.ok);
        QVERIFY(startedFirst);
        QCOMPARE(roots.size(), 2);
        QCOMPARE(roots.at(0), roots.at(1));
        QCOMPARE(streams.size(), 1);
        const QString root = roots.first();
        QCOMPARE(readFile(root + "/docs/a.txt"), QByteArray("alpha"));
        QCOMPARE(readFile(root + "/docs/sub/b.txt"), QByteArray("beta"));
        QCOMPARE(b.clip->pinnedTargets(), QStringList{root});

        FilesAnswer later;
        QString laterRoot;
        b.noGesture(); // nothing moves any more: no paste needed
        const quint64 id3 = b.clip->fetchFiles(later.callback(), kid, [&](const QString &r) { laterRoot = r; }, true);
        QVERIFY(later.answered);
        QVERIFY(later.ok);
        QCOMPARE(later.files, first.files);
        QVERIFY(id3 != 0); // it holds the pin
        QCOMPARE(laterRoot, root);
        FilesAnswer plain;
        QCOMPARE(b.clip->fetchFiles(plain.callback(), kid), quint64(0));
        QVERIFY(plain.answered && plain.ok);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 1);

        b.clip->dropFilesWaiter(id1);
        b.clip->dropFilesWaiter(id2);
        QCOMPARE(b.clip->pinnedTargets(), QStringList{root});
        b.clip->dropFilesWaiter(id3);
        QVERIFY(b.clip->pinnedTargets().isEmpty());
        QVERIFY(QFileInfo::exists(root)); // dropping a pin deletes nothing
    }

    // The last streaming waiter of a transfer going away (its connection closed: Explorer
    // cancelled) cancels the transfer: the publisher's upload ends and the folder goes, quietly. A
    // plain waiter going away cancels nothing: the files are kept for the next paste.
    void droppingTheLastStreamingWaiterCancelsItsTransfer()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.transfer->setWriteDelayForTests(50);
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("slow.bin"), QByteArray(4 * 1024 * 1024, 's'));
        QVERIFY(!kid.isEmpty());
        QSignalSpy finished(b.transfer.get(), &FileTransfer::transferFinished);
        QSignalSpy filesNotices(b.clip.get(), &ClipboardSync::filesNotice);
        FilesAnswer plain, streaming;
        QString root;
        const quint64 plainId = b.clip->fetchFiles(plain.callback(), kid);
        const quint64 streamId =
            b.clip->fetchFiles(streaming.callback(), kid, [&](const QString &r) { root = r; }, true);
        QVERIFY(plainId != 0 && streamId != 0);
        QTRY_VERIFY(!root.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + "/slow.bin"), 20000);
        b.clip->dropFilesWaiter(plainId);
        QTest::qWait(200);
        QCOMPARE(finished.size(), 0);
        b.clip->dropFilesWaiter(streamId);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(3).toString(), QString("cancelled"));
        QVERIFY(!plain.answered && !streaming.answered); // dropped: never called
        QTRY_COMPARE_WITH_TIMEOUT(a.transfer->liveUploadsForTests(), 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root), 20000);
        QVERIFY(b.clip->pinnedTargets().isEmpty());
        for (const auto &n : std::as_const(filesNotices))
            QVERIFY2(!n.at(1).toBool(), qPrintable(n.at(0).toString()));
        QCOMPARE(b.notices->size(), 0);
        QCOMPARE(b.lastPending(), 1); // still to be pasted

        // A plain request alone going away: the transfer goes on, and the next paste has them.
        FilesAnswer gone;
        const quint64 goneId = b.clip->fetchFiles(gone.callback(), kid);
        QVERIFY(goneId != 0);
        b.clip->dropFilesWaiter(goneId);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 30000);
        QCOMPARE(finished.last().at(3).toString(), QString("done"));
        QVERIFY(!gone.answered);
        b.noGesture();
        FilesAnswer next;
        b.clip->fetchFiles(next.callback(), kid);
        QVERIFY(next.answered);
        QVERIFY(next.ok);
        QCOMPARE(next.files.size(), 1);
        QCOMPARE(QFileInfo(next.files.first()).size(), 4 * 1024 * 1024);
    }

    // A newer clipboard cancels a plain transfer of the old one (Changed), but a transfer a
    // streaming paste reads goes on, detached: its waiter gets the files, and the newer clipboard
    // is not touched by it.
    void newerClipboardDetachesAStreamingTransferOnly()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.transfer->setWriteDelayForTests(50);
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("slow.bin"), QByteArray(4 * 1024 * 1024, 's'));
        QVERIFY(!kid.isEmpty());
        QSignalSpy progress(b.transfer.get(), &FileTransfer::transferProgress);
        FilesAnswer plain;
        b.clip->fetchFiles(plain.callback(), kid);
        QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty(), 20000);
        a.clip->localChanged(text("newer"));
        QTRY_VERIFY_WITH_TIMEOUT(plain.answered, 15000);
        QVERIFY(!plain.ok);
        QCOMPARE(plain.code, ClipboardSync::FetchError::Changed);

        const QByteArray kid2 = copyOneFile(a, b, source.filePath("slow2.bin"), QByteArray(4 * 1024 * 1024, 't'));
        QVERIFY(!kid2.isEmpty());
        FilesAnswer streaming;
        QString root;
        const quint64 id = b.clip->fetchFiles(streaming.callback(), kid2, [&](const QString &r) { root = r; }, true);
        QVERIFY(id != 0);
        QTRY_VERIFY(!root.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + "/slow2.bin"), 20000);
        const qsizetype applied = b.applied->size();
        a.clip->localChanged(text("newest"));
        QTRY_VERIFY_WITH_TIMEOUT(b.applied->size() > applied, 15000);
        QVERIFY(b.lastKid() != kid2);
        QCOMPARE(b.lastPending(), 0);
        QVERIFY(!streaming.answered);
        QTRY_VERIFY_WITH_TIMEOUT(streaming.answered, 30000);
        QVERIFY2(streaming.ok, qPrintable(streaming.error));
        QCOMPARE(streaming.files.size(), 1);
        QCOMPARE(QFileInfo(root + "/slow2.bin").size(), 4 * 1024 * 1024);
        QCOMPARE(b.clip->pinnedTargets(), QStringList{root});
        // The current clipboard (text) did not get the old files.
        QVERIFY(b.clip->currentFiles().isEmpty());
        FilesAnswer none;
        b.clip->fetchFiles(none.callback());
        QVERIFY(none.answered);
        QVERIFY(!none.ok);
        QCOMPARE(b.lastPending(), 0);
        b.clip->dropFilesWaiter(id);
        QVERIFY(b.clip->pinnedTargets().isEmpty());
    }

    // A user change (reset) cancels every transfer, the detached ones streaming pastes still read
    // included: they act for the previous user.
    void resetCancelsADetachedTransfer()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.transfer->setWriteDelayForTests(50);
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("slow.bin"), QByteArray(4 * 1024 * 1024, 's'));
        QVERIFY(!kid.isEmpty());
        FilesAnswer streaming;
        QString root;
        b.clip->fetchFiles(streaming.callback(), kid, [&](const QString &r) { root = r; }, true);
        QTRY_VERIFY(!root.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + "/slow.bin"), 20000);
        const qsizetype applied = b.applied->size();
        a.clip->localChanged(text("newer"));
        QTRY_VERIFY_WITH_TIMEOUT(b.applied->size() > applied, 15000);
        QTest::qWait(200);
        QVERIFY(!streaming.answered); // detached, still going

        b.clip->reset();
        QVERIFY(streaming.answered);
        QVERIFY(!streaming.ok);
        QCOMPARE(streaming.code, ClipboardSync::FetchError::Changed);
        QTRY_COMPARE_WITH_TIMEOUT(a.transfer->liveUploadsForTests(), 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root), 20000);
        QCOMPARE(b.notices->size(), 0);
    }

    // A detached transfer goes on for the streaming pastes reading it only: a plain request of the
    // replaced clipboard that had joined it is answered Changed at once, and once the last
    // streaming paste goes the transfer is cancelled (the plain one does not keep it alive).
    void detachedTransferEndsWithItsLastStreamingWaiter()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.transfer->setWriteDelayForTests(50);
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("slow.bin"), QByteArray(4 * 1024 * 1024, 's'));
        QVERIFY(!kid.isEmpty());
        QSignalSpy finished(b.transfer.get(), &FileTransfer::transferFinished);
        FilesAnswer plain, streaming;
        QString root;
        const quint64 streamId =
            b.clip->fetchFiles(streaming.callback(), kid, [&](const QString &r) { root = r; }, true);
        const quint64 plainId = b.clip->fetchFiles(plain.callback(), kid);
        QVERIFY(streamId != 0 && plainId != 0);
        QTRY_VERIFY(!root.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + "/slow.bin"), 20000);
        const qsizetype applied = b.applied->size();
        a.clip->localChanged(text("newer"));
        QTRY_VERIFY_WITH_TIMEOUT(b.applied->size() > applied, 15000);
        QVERIFY(plain.answered);
        QVERIFY(!plain.ok);
        QCOMPARE(plain.code, ClipboardSync::FetchError::Changed);
        QVERIFY(!streaming.answered); // detached, still going for it
        QCOMPARE(finished.size(), 0);

        b.clip->dropFilesWaiter(plainId); // its connection goes too: nothing changes
        b.clip->dropFilesWaiter(streamId);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(3).toString(), QString("cancelled"));
        QVERIFY(!streaming.answered); // dropped: never called
        QTRY_COMPARE_WITH_TIMEOUT(a.transfer->liveUploadsForTests(), 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root), 20000);
        QVERIFY(b.clip->pinnedTargets().isEmpty());
        QCOMPARE(b.notices->size(), 0);
    }

    // A guest's streaming paste whose requester goes while the master decides: the late grant
    // starts nothing (no transfer nobody waits for, no folder given, nobody told).
    void guestWaiterGoneBeforeTheGrantStartsNothing()
    {
        Device a, g;
        QVERIFY(test::pairNodes(*a.node, *g.node));
        QVERIFY(markGuest(a, g));
        shareFrom(a, {&g});
        QTemporaryDir source;
        writeFile(source.filePath("secret.txt"), "classified");
        a.clip->localChanged({}, {source.filePath("secret.txt")});
        moveCursor(a, a, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        QSignalSpy transfers(g.transfer.get(), &FileTransfer::transferFinished);
        QSignalSpy filesNotices(g.clip.get(), &ClipboardSync::filesNotice);
        g.freshGesture();
        g.inputArrivedFlag = true;
        a.pasteOrderedDevice = g.id();
        const int asked = g.countType(QStringLiteral("clip.unsealed"));
        FilesAnswer answer;
        bool started = false;
        const quint64 id =
            g.clip->fetchFiles(answer.callback(), g.lastKid(), [&](const QString &) { started = true; }, true);
        QVERIFY(id != 0);
        QVERIFY(!answer.answered);
        g.clip->dropFilesWaiter(id);
        QTRY_COMPARE(g.countType(QStringLiteral("clip.unsealed")), asked + 1); // granted, too late
        QTest::qWait(300);
        QCOMPARE(streams.size(), 0);
        QCOMPARE(transfers.size(), 0);
        QVERIFY(!started);
        QVERIFY(!answer.answered);
        QCOMPARE(filesNotices.size(), 0);
        QCOMPARE(g.notices->size(), 0);
        QCOMPARE(g.lastPending(), 1);
    }

    // The folder a streaming paste read stays while that paste is there (pinned), however many
    // transfers come after it; once the paste is gone, the next prune takes it.
    void pinnedFolderSurvivesThePruneOfLaterTransfers()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("one.txt"), "one");
        QVERIFY(!kid.isEmpty());
        FilesAnswer first;
        QString root;
        const quint64 id = b.clip->fetchFiles(first.callback(), kid, [&](const QString &r) { root = r; }, true);
        QTRY_VERIFY_WITH_TIMEOUT(first.answered, 15000);
        QVERIFY2(first.ok, qPrintable(first.error));
        const QDir clipboardDir(b.downloads.filePath("Clipboard"));
        auto received = [&] { return clipboardDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(); };
        for (int i = 0; i < ClipboardSync::kKeptDownloads + 1; ++i) {
            QVERIFY(!copyOneFile(a, b, source.filePath(QStringLiteral("n%1.txt").arg(i)), "n").isEmpty());
            QString err;
            QVERIFY2(!b.paste(&err).isEmpty(), qPrintable(err));
        }
        // Without the pin the first one would be the oldest of five, beyond kKeptDownloads.
        QTRY_COMPARE_WITH_TIMEOUT(received(), ClipboardSync::kKeptDownloads + 1, 10000);
        QVERIFY(QFileInfo::exists(root + "/one.txt"));

        b.clip->dropFilesWaiter(id);
        QVERIFY(!copyOneFile(a, b, source.filePath("last.txt"), "last").isEmpty());
        QString err;
        QVERIFY2(!b.paste(&err).isEmpty(), qPrintable(err));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(received(), ClipboardSync::kKeptDownloads, 10000);
    }

    // --- Move paste (the Windows agent's large pastes) -----------------------------------------

    // Another device's files, still to transfer, that this device was let into can be pasted by
    // moving them; a guest's never.
    void filesMovableOnlyForTrustedPendingFiles()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("one.txt"), "one");
        QVERIFY(!kid.isEmpty());
        QVERIFY(b.clip->filesMovable());
        QCOMPARE(b.clip->pendingBytes(), qint64(3));
        QVERIFY(!a.clip->filesMovable()); // its own copy
        QString err;
        QVERIFY2(!b.paste(&err).isEmpty(), qPrintable(err));
        QVERIFY(!b.clip->filesMovable()); // here already
        QCOMPARE(b.clip->pendingBytes(), qint64(0));

        Device m, g;
        QVERIFY(test::pairNodes(*m.node, *g.node));
        QVERIFY(markGuest(m, g));
        shareFrom(m, {&g});
        writeFile(source.filePath("secret.txt"), "classified");
        m.clip->localChanged({}, {source.filePath("secret.txt")});
        moveCursor(m, m, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QVERIFY(g.lastPending() > 0);
        QVERIFY(!g.clip->filesMovable());
    }

    // A move paste (fresh) of files transferred before transfers them again into a new folder,
    // with the paste rule of any transfer: no paste gesture, nothing moves. A paste that is not a
    // move paste is still given the files already here.
    void freshPasteTransfersAgainIntoANewFolder()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("docs/a.txt"), "alpha");
        writeFile(source.filePath("docs/sub/b.txt"), "beta");
        a.clip->localChanged({}, {source.filePath("docs")});
        QTRY_VERIFY_WITH_TIMEOUT(b.lastPending() > 0, 15000);
        const QByteArray kid = b.lastKid();
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);

        FilesAnswer first;
        QString root1;
        const quint64 id1 = b.clip->fetchFiles(first.callback(), kid, [&](const QString &r) { root1 = r; }, true, -1,
                                               true);
        QTRY_VERIFY_WITH_TIMEOUT(first.answered, 15000);
        QVERIFY2(first.ok, qPrintable(first.error));
        QCOMPARE(streams.size(), 1);
        QCOMPARE(readFile(root1 + "/docs/sub/b.txt"), QByteArray("beta"));

        b.noGesture();
        FilesAnswer refused;
        b.clip->fetchFiles(refused.callback(), kid, {}, true, -1, true);
        QTRY_VERIFY_WITH_TIMEOUT(refused.answered, 10000);
        QCOMPARE(refused.code, ClipboardSync::FetchError::NoPaste);
        QTest::qWait(200);
        QCOMPARE(streams.size(), 1);
        // Refused for want of a paste: the copy here is still the current one (nothing moved).
        QVERIFY(!b.clip->filesMovable());
        FilesAnswer kept;
        QCOMPARE(b.clip->fetchFiles(kept.callback(), kid), quint64(0));
        QVERIFY(kept.answered && kept.ok);
        QCOMPARE(QFileInfo(kept.files.first()).absolutePath(), QFileInfo(root1 + "/docs").absolutePath());
        QCOMPARE(streams.size(), 1);

        b.freshGesture();
        FilesAnswer second;
        QString root2;
        const quint64 id2 = b.clip->fetchFiles(second.callback(), kid, [&](const QString &r) { root2 = r; }, true, -1,
                                               true);
        QTRY_VERIFY_WITH_TIMEOUT(second.answered, 15000);
        QVERIFY2(second.ok, qPrintable(second.error));
        QCOMPARE(streams.size(), 2);
        QVERIFY(!root2.isEmpty() && root2 != root1);
        QCOMPARE(readFile(root2 + "/docs/a.txt"), QByteArray("alpha"));
        QVERIFY(QFileInfo::exists(root1 + "/docs/a.txt")); // the earlier copy is not touched
        QStringList pinned = b.clip->pinnedTargets();
        pinned.sort();
        QStringList expected{root1, root2};
        expected.sort();
        QCOMPARE(pinned, expected);

        // Not a move paste: given the files here, no transfer.
        b.noGesture();
        FilesAnswer plain;
        QCOMPARE(b.clip->fetchFiles(plain.callback(), kid), quint64(0));
        QVERIFY(plain.answered && plain.ok);
        QCOMPARE(QFileInfo(plain.files.first()).absolutePath(), QFileInfo(root2 + "/docs").absolutePath());
        QCOMPARE(streams.size(), 2);
        b.clip->dropFilesWaiter(id1);
        b.clip->dropFilesWaiter(id2);
    }

    // A move paste's request (fresh) is only for another device's files transferred for it: files
    // copied on this device are never given away to be moved.
    void freshRequestForLocalFilesIsRefused()
    {
        Device a;
        QTemporaryDir source;
        writeFile(source.filePath("mine/a.txt"), "alpha");
        a.clip->localChanged({}, {source.filePath("mine")});
        auto plainPaste = [&] { // a plain paste gets them, where they are (once their folder is walked)
            FilesAnswer plain;
            a.clip->fetchFiles(plain.callback(), {});
            return plain.answered && plain.ok;
        };
        QTRY_VERIFY_WITH_TIMEOUT(plainPaste(), 10000);
        FilesAnswer move;
        QString started;
        QCOMPARE(a.clip->fetchFiles(move.callback(), {}, [&](const QString &r) { started = r; }, true, -1, true),
                 quint64(0));
        QVERIFY(move.answered);
        QVERIFY(!move.ok);
        QVERIFY(started.isEmpty());
        QVERIFY(QFileInfo::exists(source.filePath("mine/a.txt")));
    }

    // clipConsumed for a request that is not pinned yet (still waiting for its paste's press)
    // ends that request: nothing of it is left waiting.
    void consumedBeforeItWasPinnedEndsTheRequest()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("docs/a.txt"), "alpha");
        a.clip->localChanged({}, {source.filePath("docs")});
        QTRY_VERIFY_WITH_TIMEOUT(b.lastPending() > 0, 15000);
        const QByteArray kid = b.lastKid();
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);
        b.noGesture();
        FilesAnswer answer;
        const quint64 id = b.clip->fetchFiles(answer.callback(), kid, {}, true, -1, true);
        QVERIFY(id != 0);
        QVERIFY(!answer.answered);
        b.clip->filesConsumed(id);
        QTest::qWait(ClipboardSync::kEvidenceGraceMs + 300);
        QVERIFY(!answer.answered); // gone, not refused later
        QCOMPARE(streams.size(), 0);
    }

    // Receive while a move paste waits for the same transfer: the move paste is given the files to
    // move, and the clipboard is not applied again under it.
    void receiveJoinedByAMovePasteDoesNotApplyAgain()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("docs/a.txt"), "alpha");
        a.clip->localChanged({}, {source.filePath("docs")});
        QTRY_VERIFY_WITH_TIMEOUT(b.lastPending() > 0, 15000);
        const QByteArray kid = b.lastKid();
        const qsizetype applied = b.applied->size();
        FilesAnswer got, move;
        QString root;
        b.clip->receiveFiles(kid, got.callback());
        const quint64 id = b.clip->fetchFiles(move.callback(), kid, [&](const QString &r) { root = r; }, true, -1, true);
        QTRY_VERIFY_WITH_TIMEOUT(got.answered && move.answered, 15000);
        QVERIFY2(got.ok, qPrintable(got.error));
        QVERIFY2(move.ok, qPrintable(move.error));
        QTest::qWait(200);
        QCOMPARE(b.applied->size(), applied);
        QCOMPARE(b.clip->pinnedTargets(), QStringList{root});
        b.clip->dropFilesWaiter(id);
    }

    // A move paste that took the files (clipConsumed): its folder goes (what the paste left in it
    // too) and the next paste, move paste or not, transfers again. A folder another paste still
    // reads is left alone.
    void consumedCopyIsRemovedAndTheNextPasteTransfersAgain()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source, elsewhere;
        writeFile(source.filePath("docs/a.txt"), "alpha");
        writeFile(source.filePath("docs/b.txt"), "beta");
        a.clip->localChanged({}, {source.filePath("docs")});
        QTRY_VERIFY_WITH_TIMEOUT(b.lastPending() > 0, 15000);
        const QByteArray kid = b.lastKid();
        const int pending = b.lastPending();
        QSignalSpy streams(a.node.get(), &Node::streamAccepted);

        FilesAnswer first;
        QString root;
        const quint64 id = b.clip->fetchFiles(first.callback(), kid, [&](const QString &r) { root = r; }, true, -1, true);
        QTRY_VERIFY_WITH_TIMEOUT(first.answered, 15000);
        QVERIFY2(first.ok, qPrintable(first.error));
        // Explorer moves the folder away but skips one file (a name conflict it was told to skip).
        QVERIFY(QDir().mkpath(elsewhere.filePath("docs")));
        QVERIFY(QFile::rename(root + "/docs/a.txt", elsewhere.filePath("docs/a.txt")));
        QVERIFY(QFileInfo::exists(root + "/docs/b.txt"));
        b.clip->filesConsumed(0);     // unknown: nothing
        b.clip->filesConsumed(id + 1000);
        QVERIFY(QFileInfo::exists(root));
        b.clip->filesConsumed(id);
        QVERIFY(b.clip->pinnedTargets().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root), 10000);
        QCOMPARE(readFile(elsewhere.filePath("docs/a.txt")), QByteArray("alpha")); // the pasted copy stays
        QVERIFY(b.clip->filesMovable()); // to transfer again

        // The next paste (not a move paste) transfers again: the copy here was taken.
        b.freshGesture();
        QString err;
        const QStringList again = b.paste(&err);
        QVERIFY2(!again.isEmpty(), qPrintable(err));
        QCOMPARE(streams.size(), 2);
        QCOMPARE(readFile(again.first() + "/b.txt"), QByteArray("beta"));

        // Two pastes reading the same folder: one consuming it leaves it to the other.
        FilesAnswer m1, m2;
        QString r1, r2;
        const quint64 mid1 = b.clip->fetchFiles(m1.callback(), kid, [&](const QString &r) { r1 = r; }, true, -1, true);
        QTRY_VERIFY_WITH_TIMEOUT(m1.answered && m1.ok, 15000);
        const quint64 mid2 = b.clip->fetchFiles(m2.callback(), kid, [&](const QString &r) { r2 = r; }, true);
        QVERIFY(m2.answered && m2.ok);
        QCOMPARE(r2, r1);
        b.clip->filesConsumed(mid1);
        QTest::qWait(300);
        QVERIFY(QFileInfo::exists(r1 + "/docs/a.txt"));
        QCOMPARE(b.clip->pinnedTargets(), QStringList{r1});
        QCOMPARE(b.lastPending(), pending); // never applied again by any of this
        b.clip->dropFilesWaiter(mid2);
    }

    // Receive: the user takes the files with no paste (after a reader was refused), only for the
    // current clipboard and only files this device was let into. A request waiting for a gesture
    // goes on with it; once here the clipboard is applied again with the local files. A guest's
    // clipboard is never received this way (the master's grant needs the chord).
    void receiveTakesTheFilesWithoutAPaste()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        const QByteArray kid = copyOneFile(a, b, source.filePath("notes.txt"), "remember the milk");
        QVERIFY(!kid.isEmpty());
        b.noGesture();

        FilesAnswer stale;
        b.clip->receiveFiles(crypto::randomBytes(16), stale.callback());
        QVERIFY(stale.answered);
        QVERIFY(!stale.ok);
        QCOMPARE(stale.code, ClipboardSync::FetchError::Changed);

        const qsizetype applied = b.applied->size();
        FilesAnswer reader, got;
        b.clip->fetchFiles(reader.callback(), kid); // waits for a gesture
        QVERIFY(!reader.answered);
        b.clip->receiveFiles(kid, got.callback());
        QTRY_VERIFY_WITH_TIMEOUT(got.answered && reader.answered, 15000);
        QVERIFY2(got.ok, qPrintable(got.error));
        QVERIFY(reader.ok);
        QCOMPARE(got.files.size(), 1);
        QCOMPARE(readFile(got.files.first()), QByteArray("remember the milk"));
        QCOMPARE(b.applied->size(), applied + 1);
        QCOMPARE(b.lastKid(), kid);
        QCOMPARE(b.lastPending(), 0);
        QCOMPARE(b.lastFiles(), got.files);

        Device m, g;
        QVERIFY(test::pairNodes(*m.node, *g.node));
        QVERIFY(markGuest(m, g));
        shareFrom(m, {&g});
        writeFile(source.filePath("secret.txt"), "classified");
        m.clip->localChanged({}, {source.filePath("secret.txt")});
        moveCursor(m, m, g);
        QTRY_COMPARE(g.applied->size(), 1);
        QSignalSpy guestStreams(m.node.get(), &Node::streamAccepted);
        FilesAnswer guest;
        g.clip->receiveFiles(g.lastKid(), guest.callback());
        QVERIFY(guest.answered);
        QVERIFY(!guest.ok);
        QTest::qWait(200);
        QCOMPARE(guestStreams.size(), 0);
        QVERIFY(!m.sawType(QStringLiteral("clip.unseal")));
    }

    // Pruning the received copies keeps the newest ones (by stamp, then by the transfer counter,
    // numerically) and never touches a folder a transfer still writes into or a paste still reads.
    void pruneSkipsActiveAndPinnedTargets()
    {
        QTemporaryDir received;
        const QDir dir(received.path());
        for (const char *name : {"50", "100-1", "100-2", "100-10", "200-1", "300-1"})
            QVERIFY(dir.mkpath(QString::fromLatin1(name)));
        QSet<QString> pinned{dir.filePath("100-1")};
        auto remaining = [&] {
            QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            names.sort();
            return names;
        };
        ClipboardSync::pruneReceived(dir.path(), 3, [&](const QString &path) { return pinned.contains(path); });
        QCOMPARE(remaining(), QStringList({"100-1", "100-10", "200-1", "300-1"}));

        // A transfer at work into "400-1" (writing slowly): left alone even when nothing is kept.
        QTemporaryDir dirA, dirB, source;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriteDelayForTests(50);
        writeFile(source.filePath("slow.bin"), QByteArray(4 * 1024 * 1024, 's'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("slow.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);
        const QString active = dir.filePath("400-1");
        const quint64 handle = fb.fetch(a->id(), id, active, true, 1 << 30, [](const FetchResult &) {});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(active).filePath("slow.bin")), 20000);
        QVERIFY(fb.isActiveTarget(active));
        const auto busy = fb.activeTargetCheck();
        ClipboardSync::pruneReceived(dir.path(), 0,
                                     [&](const QString &path) { return busy(path) || pinned.contains(path); });
        QCOMPARE(remaining(), QStringList({"100-1", "400-1"}));
        fb.cancel(handle);
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(active), 20000);
        QVERIFY(!QFileInfo::exists(active)); // the cancelled transfer removed what it wrote
    }

    // A paste's transfer only renames the older copies aside before it starts (retireReceived),
    // and deletes them afterwards (removeRetired): pinned and active folders are never touched,
    // what an earlier run left set aside goes too, and any prune deletes such leftovers.
    void deferredPruneRenamesThenDeletes()
    {
        QTemporaryDir received;
        const QDir dir(received.path());
        for (const char *name : {"50", "100-1", "100-2", "100-10", "200-1", "300-1"})
            QVERIFY(dir.mkpath(QString::fromLatin1(name)));
        // Contents, so the deletion has a tree to walk.
        for (int i = 0; i < 20; ++i) {
            QFile f(dir.filePath(QStringLiteral("100-2/sub/f%1.txt").arg(i)));
            QVERIFY(dir.mkpath(QStringLiteral("100-2/sub")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("old");
        }
        // Left set aside by an earlier run (a crash before its deletion).
        QVERIFY(dir.mkpath(QStringLiteral(".removing-10-1-abc/inner")));
        const QSet<QString> pinned{dir.filePath("100-1")};
        const auto skip = [&](const QString &path) { return pinned.contains(path); };
        auto visible = [&] {
            QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
            names.sort();
            return names;
        };

        const QStringList retired = ClipboardSync::retireReceived(dir.path(), 2, skip);
        // "50", "100-2" and "100-10" are renamed aside (the pinned "100-1" is neither counted nor
        // touched), plus the leftover.
        QCOMPARE(retired.size(), 4);
        QStringList names = visible();
        QVERIFY(names.contains("100-1"));
        QVERIFY(names.contains("200-1"));
        QVERIFY(names.contains("300-1"));
        QVERIFY(!names.contains("50"));
        QVERIFY(!names.contains("100-2"));
        QVERIFY(!names.contains("100-10"));
        int aside = 0;
        for (const QString &name : names)
            aside += name.startsWith(QLatin1String(".removing-")) ? 1 : 0;
        QCOMPARE(aside, 4);
        for (const QString &path : retired) {
            QVERIFY2(QFileInfo(path).fileName().startsWith(QLatin1String(".removing-")), qPrintable(path));
            QVERIFY(QFileInfo::exists(path));
        }
        // Set aside folders are left out of what a prune counts: nothing more goes.
        QCOMPARE(ClipboardSync::retireReceived(dir.path(), 2, skip).size(), 4); // the same four, as leftovers

        ClipboardSync::removeRetired(retired, skip);
        QCOMPARE(visible(), QStringList({"100-1", "200-1", "300-1"}));

        // A leftover is deleted by an ordinary prune too, which keeps the newest as before.
        QVERIFY(dir.mkpath(QStringLiteral(".removing-20-1-def/x")));
        ClipboardSync::pruneReceived(dir.path(), 3, skip);
        QCOMPARE(visible(), QStringList({"100-1", "200-1", "300-1"}));
        ClipboardSync::pruneReceived(dir.path(), 1, skip);
        QCOMPARE(visible(), QStringList({"100-1", "300-1"}));
    }

    // A folder that could not be renamed is deleted in place later only if nothing uses it by
    // then; a renamed one always goes.
    void removeRetiredChecksUnrenamedFoldersAgain()
    {
        QTemporaryDir received;
        const QDir dir(received.path());
        QVERIFY(dir.mkpath("100-1"));
        QVERIFY(dir.mkpath("100-2"));
        const QString aside = ClipboardSync::retireFolder(dir.filePath("100-2"));
        QVERIFY(!aside.isEmpty());
        QVERIFY(QFileInfo(aside).fileName().startsWith(QLatin1String(".removing-100-2-")));
        QVERIFY(!QFileInfo::exists(dir.filePath("100-2")));
        // "100-1" is pinned by now: kept; the renamed one goes whatever the check says.
        ClipboardSync::removeRetired({dir.filePath("100-1"), aside}, [](const QString &) { return true; });
        QVERIFY(QFileInfo::exists(dir.filePath("100-1")));
        QVERIFY(!QFileInfo::exists(aside));
        ClipboardSync::removeRetired({dir.filePath("100-1")}, [](const QString &) { return false; });
        QVERIFY(!QFileInfo::exists(dir.filePath("100-1")));
        QVERIFY(ClipboardSync::retireFolder(dir.filePath("missing")).isEmpty());
    }

    // A paste's cleanup that finds a folder another prune is already deleting returns only once
    // that one is done: the transfer's free space check runs when it returns, and must see the
    // space freed (it used to skip the folder and return at once, so a copy that fits was refused).
    void removeRetiredWaitsForAnotherPrune()
    {
        QTemporaryDir received;
        const QDir dir(received.path());
        const QString leftover = dir.filePath(QStringLiteral(".removing-10-1-abc"));
        // Enough files that deleting them takes a while.
        for (int d = 0; d < 40; ++d) {
            const QString sub = QStringLiteral("%1/d%2").arg(leftover).arg(d);
            QVERIFY(dir.mkpath(sub));
            for (int i = 0; i < 150; ++i) {
                QFile f(QStringLiteral("%1/f%2.txt").arg(sub).arg(i));
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("old");
            }
        }
        std::atomic<bool> started = false;
        std::atomic<bool> pruned = false;
        std::thread other([&] {
            started = true;
            ClipboardSync::pruneReceived(dir.path(), 3, {}); // claims the leftover first
            pruned = true;
        });
        while (!started.load())
            QThread::yieldCurrentThread();
        QThread::msleep(30);
        const bool overlapped = !pruned.load();
        ClipboardSync::removeRetired({leftover}, {});
        const bool gone = !QFileInfo::exists(leftover);
        other.join();
        if (!overlapped)
            QSKIP("The other prune finished before the cleanup started: nothing to wait for");
        QVERIFY(gone);
    }
};

QTEST_GUILESS_MAIN(TestClipboard)
#include "tst_clipboard.moc"
