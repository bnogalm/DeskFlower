// Dragging files between two paired devices with fake agents: A (the master) drags two items
// across its right edge with the button held; the drag is shown on B while the cursor is there;
// releasing it there makes B's (fake) drop target read the files, which transfers them, and A's
// originals go to the (fake) Recycle Bin. Also: no files, dropping back on A, no drop target.

#include "agentrequests.h"
#include "filedrag.h"
#include "filetransfer.h"
#include "inputcontroller.h"
#include "testutil.h"
#include "workspace.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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

// What an agent and Explorer would do, minus the windows.
struct FakeAgent {
    FileDrag *drag = nullptr;
    QStringList dragged;          // what is being dragged on this device (source)
    bool holdCatch = false;       // keep the catch unanswered (heldCatch) instead
    std::function<void(const QStringList &)> heldCatch;
    bool nativeDrag = true;       // runs native drags (a hover function), as the Windows agent
    bool acceptDrop = true;       // something under the cursor takes the drop
    bool confirm = true;          // ...and reports a move of what it read (performed drop effect)
    QString dropFolder;           // released over this folder window: handed to the service
    bool holdDrop = false;        // a release is not acted on until heldDrop runs (or heldEnd)
    std::function<void()> heldDrop;                      // ...the drop target reads the files
    std::function<void(const FileDrag::HoverEnd &)> heldEnd; // ...or the drag ends some other way
    QString folderUnderCursor;    // folderAt (no drag shown)
    QStringList recycled;
    QStringList badges;
    QPoint caughtAt;
    // The drag shown here.
    bool hovering = false;
    FileDrag::Hover hover;
    std::function<void(const FileDrag::HoverEnd &)> done;
    QStringList received;         // what the drop target got
    int hovers = 0;

    static FileDrag::HoverEnd dropped(bool confirmed)
    {
        FileDrag::HoverEnd end;
        end.dropped = true;
        end.confirmed = confirmed;
        return end;
    }

    FileDrag::Agent api()
    {
        FileDrag::Agent a;
        a.catchFiles = [this](QPoint at, std::function<void(const QStringList &)> d) {
            caughtAt = at;
            if (holdCatch)
                heldCatch = std::move(d);
            else
                d(dragged);
        };
        if (nativeDrag) {
            a.hover = [this](const FileDrag::Hover &h, std::function<void(const FileDrag::HoverEnd &)> d) {
                hovering = true;
                hover = h;
                done = std::move(d);
                ++hovers;
            };
        }
        a.endHover = [this](bool drop) {
            hovering = false;
            auto d = std::move(done);
            if (!d)
                return;
            if (!drop || !acceptDrop) {
                d({});
                return;
            }
            if (hover.own) { // Windows moves the originals itself
                d(dropped(false));
                return;
            }
            if (!dropFolder.isEmpty()) { // Explorer: the service transfers into that folder
                FileDrag::HoverEnd end;
                end.folder = dropFolder;
                d(end);
                return;
            }
            // The drop target reads the virtual files: they are transferred now.
            auto read = [this, d, offer = hover.offer] {
                drag->fetchDragged(offer, [this, d](bool ok, const QStringList &items, const QString &) {
                    received = items;
                    d(dropped(ok && confirm));
                });
            };
            if (holdDrop) {
                heldDrop = read;
                heldEnd = d;
                return;
            }
            read();
        };
        a.folderAt = [this](QPoint, std::function<void(const QString &)> d) { d(folderUnderCursor); };
        a.recycle = [this](const QStringList &paths, std::function<void(bool, const QString &)> d) {
            recycled = paths;
            d(true, {});
        };
        a.badge = [this](const QString &text) { badges.append(text); };
        return a;
    }
};

bool hasNotice(const QSignalSpy &spy, const QString &part)
{
    for (const QList<QVariant> &args : spy) {
        if (args.value(0).toString().contains(part))
            return true;
    }
    return false;
}

QStringList entries(const QString &dir)
{
    return QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

// What an older `from` would declare to `to` (the hello is a control message).
bool announceVersion(Node &from, Node &to, const QString &version)
{
    QCborMap hello;
    hello.insert(QStringLiteral("t"), QStringLiteral("hello"));
    hello.insert(QStringLiteral("n"), from.name());
    hello.insert(QStringLiteral("tp"), from.tcpPort());
    hello.insert(QStringLiteral("v"), version);
    hello.insert(QStringLiteral("tl"), QStringLiteral("device"));
    from.sendMessage(to.id(), hello);
    return QTest::qWaitFor([&] {
        for (const PeerView &v : to.peers())
            if (v.id == from.id())
                return v.version == version;
        return false;
    }, 5000);
}

struct Pair {
    QTemporaryDir dirA, dirB, downloadsA, downloadsB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Workspace> wa, wb;
    std::unique_ptr<InputController> ia, ib;
    std::unique_ptr<FileTransfer> ta, tb;
    FakeAgent agentA, agentB;
    std::unique_ptr<FileDrag> da, db;

    // guestB: A pairs B as a guest (B still lets A drive it). Whether B's agent runs native drags:
    // agentB.nativeDrag.
    bool setup(bool guestB = false)
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        wa = std::make_unique<Workspace>(a.get(), dirA.path());
        wb = std::make_unique<Workspace>(b.get(), dirB.path());
        wa->setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        wb->setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}});
        // B chooses Device: left alone, its dialog would offer what A chose (Guest).
        if (!test::pairNodes(*a, *b, guestB ? std::optional(TrustLevel::Guest) : std::nullopt,
                             guestB ? std::optional(TrustLevel::Device) : std::nullopt))
            return false;
        if (!QTest::qWaitFor([this] { return wa->devices().size() == 2; }, 5000))
            return false;
        wa->setLayout({{a->id(), {0, 0}}, {b->id(), {1920, 0}}});
        wa->setSharing(true);
        if (!QTest::qWaitFor([this] { return wb->masterId() == a->id(); }, 5000))
            return false;
        ia = std::make_unique<InputController>(a.get(), wa.get());
        ib = std::make_unique<InputController>(b.get(), wb.get());
        ta = std::make_unique<FileTransfer>(a.get());
        tb = std::make_unique<FileTransfer>(b.get());
        const QString dlA = downloadsA.path(), dlB = downloadsB.path();
        da = std::make_unique<FileDrag>(a.get(), ta.get(), wa.get(), ia.get(), agentA.api(), [dlA] { return dlA; });
        db = std::make_unique<FileDrag>(b.get(), tb.get(), wb.get(), ib.get(), agentB.api(), [dlB] { return dlB; });
        agentA.drag = da.get();
        agentB.drag = db.get();
        ia->sessionConnected();
        ib->sessionConnected();
        // Each knows whether the other shows drags (kDragCapability, from the hello).
        return QTest::qWaitFor([this] { return knowsCapabilities(*a, *b) && knowsCapabilities(*b, *a); }, 5000);
    }

    static bool knowsCapabilities(Node &viewer, Node &peer)
    {
        for (const PeerView &v : viewer.peers()) {
            if (v.id == peer.id())
                return v.capabilitiesKnown && v.capabilities == peer.capabilities();
        }
        return false;
    }

    void dragAcross()
    {
        ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}, {"lb", true}}));
    }

    // Back from B onto A without the button, then (past the return guard) across again with it.
    void returnAndDragAgain()
    {
        ia->handleSessionMessage(cmd("mm", {{"x", -3000}, {"y", 0}}));
        QTRY_COMPARE(ia->activeDevice(), a->id());
        QTest::qWait(400);
        dragAcross();
    }

    // The cursor on B, its button held there (forwarded), then back over B's left edge onto A:
    // dragging from B, the master being A.
    void dragFromB()
    {
        ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}}));
        ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}}));
        ia->handleSessionMessage(cmd("mm", {{"x", -50}, {"y", 0}}));
    }
};
} // namespace

class TestFileDrag : public QObject
{
    Q_OBJECT
private slots:
    void filesMoveWhereTheyAreDropped()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("report.pdf"), "the report");
        writeFile(source.filePath("photos/beach.jpg"), QByteArray(300000, 'b'));
        QVERIFY(QDir().mkpath(source.filePath("photos/empty"))); // an empty folder is moved too
        p.agentA.dragged = {source.filePath("report.pdf"), source.filePath("photos")};
        QSignalSpy noticesA(p.da.get(), &FileDrag::notice);

        // Button held at A's right edge: the crossing waits for the catch, then the drag shows on B
        // with the names and sizes of what is dragged (nothing transferred yet).
        p.dragAcross();
        QTRY_COMPARE(p.ia->activeDevice(), p.b->id());
        QCOMPARE(p.agentA.caughtAt, QPoint(1919, 500));
        QVERIFY(p.ia->isDraggingFiles());
        QTRY_VERIFY(p.agentB.hovering);
        QVERIFY(!p.agentB.hover.own);
        // Folders first (parents before children), then the files.
        QCOMPARE(p.agentB.hover.files.size(), 4);
        QCOMPARE(p.agentB.hover.count, 4);
        QCOMPARE(p.agentB.hover.files.at(0).path, QString("photos"));
        QVERIFY(p.agentB.hover.files.at(0).dir);
        QCOMPARE(p.agentB.hover.files.at(1).path, QString("photos/empty"));
        QVERIFY(p.agentB.hover.files.at(1).dir);
        QCOMPARE(p.agentB.hover.files.at(2).path, QString("report.pdf"));
        QVERIFY(!p.agentB.hover.files.at(2).dir);
        QVERIFY(QDir(p.downloadsB.path()).isEmpty());

        // Released on B: the drop target reads the files, the originals go to A's Recycle Bin.
        p.ia->handleSessionMessage(cmd("mm", {{"x", 200}, {"y", 0}}));
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QVERIFY(!p.ia->isDraggingFiles());
        QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled.size(), 2, 15000);
        QCOMPARE(p.agentA.recycled, p.agentA.dragged);
        QCOMPARE(p.agentB.received.size(), 2);
        const QString root = QFileInfo(p.agentB.received.first()).absolutePath();
        QCOMPARE(readFile(root + "/report.pdf"), QByteArray("the report"));
        QCOMPARE(readFile(root + "/photos/beach.jpg"), QByteArray(300000, 'b'));
        QVERIFY(QFileInfo(root + "/photos/empty").isDir());
        QTRY_VERIFY(!noticesA.isEmpty());
    }

    void notTakenKeepsTheOriginals()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};
        p.agentB.acceptDrop = false; // e.g. released over something that takes no files

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        QSignalSpy notices(p.da.get(), &FileDrag::notice);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 10000); // "not moved"
        QVERIFY(p.agentA.recycled.isEmpty());
        QVERIFY(QFile::exists(source.filePath("a.txt")));
    }

    void droppedBackOnTheSourceTransfersNothing()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        // Back to A (through B's left edge): B's drag is cancelled, A drags the originals.
        p.ia->handleSessionMessage(cmd("mm", {{"x", -50}, {"y", 0}}));
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QTRY_VERIFY(!p.agentB.hovering);
        QTRY_VERIFY(p.agentA.hovering);
        QVERIFY(p.agentA.hover.own);
        QCOMPARE(p.agentA.hover.paths, p.agentA.dragged);
        // Released on A (its own button): Windows would move them; nothing is recycled.
        p.ia->handleSessionMessage(cmd("dragRelease", {{"x", 1800}, {"y", 500}}));
        QTRY_VERIFY(!p.agentA.hovering);
        QTest::qWait(300);
        QVERIFY(p.agentA.recycled.isEmpty());
        QVERIFY(QDir(p.downloadsB.path()).isEmpty());
    }

    // Only the device a catch was sent to may answer drag.caught: a third peer cannot slip its own
    // offer in and have the drop target fetch its files instead of the real source's.
    void aThirdNodeCannotSubstituteTheDraggedFiles()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir dirC;
        auto c = test::makeNode(dirC.path());
        QVERIFY(test::pairNodes(*p.a, *c));

        QTemporaryDir source;
        writeFile(source.filePath("real.txt"), "the real file");
        p.agentA.dragged = {source.filePath("real.txt")};

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        QCOMPARE(p.agentB.hover.files.first().path, QString("real.txt"));

        // C races in with an unsolicited drag.caught naming its own offer.
        QCborMap spoof;
        spoof.insert(QStringLiteral("t"), QStringLiteral("drag.caught"));
        spoof.insert(QStringLiteral("o"), qint64(0x1234));
        spoof.insert(QStringLiteral("n"), 1);
        spoof.insert(QStringLiteral("b"), 100);
        spoof.insert(QStringLiteral("fl"), QCborArray{QCborArray{QStringLiteral("evil.txt"), 100}});
        c->sendMessage(p.a->id(), spoof);
        QTest::qWait(300);

        // The drop still delivers the real source's file.
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentB.received.size(), 1, 15000);
        const QString root = QFileInfo(p.agentB.received.first()).absolutePath();
        QCOMPARE(readFile(root + "/real.txt"), QByteArray("the real file"));
        QCOMPARE(p.agentA.recycled, p.agentA.dragged);
    }

    // B's agent cannot show a drag (a Wayland session, macOS) but names a folder (its Downloads
    // folder): nothing is shown there, and the release sends drag.drop, which puts the files
    // (folders included) into that folder, and B's user is told where.
    void deviceWithoutDragsGetsTheFilesInAFolder()
    {
        Pair p;
        p.agentB.nativeDrag = false;
        QVERIFY(p.setup());
        QTRY_VERIFY([&] {
            for (const PeerView &v : p.a->peers())
                if (v.id == p.b->id())
                    return v.capabilitiesKnown && !v.capabilities.contains(QLatin1String(kDragCapability));
            return false;
        }());
        QTemporaryDir source, downloads;
        writeFile(source.filePath("report.pdf"), "the report");
        writeFile(source.filePath("photos/beach.jpg"), "sand");
        QVERIFY(QDir().mkpath(source.filePath("photos/empty")));
        p.agentA.dragged = {source.filePath("report.pdf"), source.filePath("photos")};
        p.agentB.folderUnderCursor = downloads.path();
        QSignalSpy noticesB(p.db.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_COMPARE(p.ia->activeDevice(), p.b->id());
        QVERIFY(p.ia->isDraggingFiles());
        QTest::qWait(300);
        QCOMPARE(p.agentB.hovers, 0);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled.size(), 2, 15000);
        QCOMPARE(readFile(downloads.filePath("report.pdf")), QByteArray("the report"));
        QCOMPARE(readFile(downloads.filePath("photos/beach.jpg")), QByteArray("sand"));
        QVERIFY(QFileInfo(downloads.filePath("photos/empty")).isDir());
        // B's user is told where they went.
        QTRY_VERIFY([&] {
            for (const QList<QVariant> &n : noticesB)
                if (n.at(0).toString().contains(QDir::toNativeSeparators(downloads.path())) && !n.at(1).toBool())
                    return true;
            return false;
        }());
    }

    // A device older than kFoldersVersion would refuse a whole list over one folder entry ([path,
    // -1]): it is shown the files alone, counted as before (n), with no folder count (dc).
    void olderDeviceIsShownTheFilesWithoutFolders()
    {
        Pair p;
        QVERIFY(p.setup());
        QCborMap hello; // what an older Windows B would declare (the hello is a control message)
        hello.insert(QStringLiteral("t"), QStringLiteral("hello"));
        hello.insert(QStringLiteral("n"), p.b->name());
        hello.insert(QStringLiteral("tp"), p.b->tcpPort());
        hello.insert(QStringLiteral("v"), QStringLiteral("0.1.19"));
        hello.insert(QStringLiteral("pl"), QStringLiteral("windows-x86_64")); // shows drags (no "dnd" yet)
        hello.insert(QStringLiteral("tl"), QStringLiteral("device"));
        p.b->sendMessage(p.a->id(), hello);
        QTRY_VERIFY([&] {
            for (const PeerView &v : p.a->peers())
                if (v.id == p.b->id())
                    return v.version == QLatin1String("0.1.19");
            return false;
        }());

        QTemporaryDir source;
        writeFile(source.filePath("photos/beach.jpg"), "sand");
        QVERIFY(QDir().mkpath(source.filePath("photos/empty")));
        p.agentA.dragged = {source.filePath("photos")};
        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        QCOMPARE(p.agentB.hover.count, 1);
        QCOMPARE(p.agentB.hover.files.size(), 1);
        QCOMPARE(p.agentB.hover.files.first().path, QString("photos/beach.jpg"));
        QVERIFY(!p.agentB.hover.files.first().dir);
    }

    void holdingTheButtonWithoutFilesCrossesAsUsual()
    {
        Pair p;
        QVERIFY(p.setup());
        p.agentA.dragged.clear(); // e.g. dragging a window, or selecting text
        p.dragAcross();
        QTRY_COMPARE(p.ia->activeDevice(), p.b->id());
        QVERIFY(!p.ia->isDraggingFiles());
        QCOMPARE(p.agentB.hovers, 0);
    }

    void crossingOutOfAGuestWithTheButtonHeldDoesNotWait()
    {
        // A paired B as a guest: what B says it caught would be ignored (it could name any
        // files), so A does not ask and crosses back at once, not after the catch timeout.
        Pair p;
        QVERIFY(p.setup(true));
        QVERIFY(p.a->isGuest(p.b->id()));
        QSignalSpy notices(p.da.get(), &FileDrag::notice);
        p.ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}}));
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}})); // held on B
        QElapsedTimer t;
        t.start();
        p.ia->handleSessionMessage(cmd("mm", {{"x", -50}, {"y", 0}})); // back over A's edge
        QTRY_COMPARE_WITH_TIMEOUT(p.ia->activeDevice(), p.a->id(), InputController::kDragCatchTimeoutMs);
        QVERIFY2(t.elapsed() < InputController::kDragCatchTimeoutMs / 2, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!p.ia->isDraggingFiles());
        // Not silently: the user learns why nothing was dragged (once, not at every crossing).
        QVERIFY(hasNotice(notices, QStringLiteral("guest")));
        QTest::qWait(400); // past the return guard
        p.ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}}));
        QCOMPARE(p.ia->activeDevice(), p.b->id());
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", true}}));
        p.ia->handleSessionMessage(cmd("mm", {{"x", -50}, {"y", 0}}));
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QCOMPARE(notices.size(), 1);
    }

    // A drop target that read the files but never reported a move (a browser uploading them, a
    // mail attaching them) got a copy: the originals stay, and the user is told.
    void droppedWithoutConfirmationKeepsTheOriginals()
    {
        Pair p;
        p.agentB.confirm = false;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};
        QSignalSpy notices(p.da.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentB.received.size(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(hasNotice(notices, QStringLiteral("originals were kept")), 10000);
        QTest::qWait(200);
        QVERIFY(p.agentA.recycled.isEmpty());
        QVERIFY(QFile::exists(source.filePath("a.txt")));
    }

    // The drop target reads the files, then the drag leaves the device (or another one replaces
    // it) before the transfer ends: the reader is answered at once with an error, never left
    // waiting, and the transfer stops.
    void supersededHoverAnswersTheWaiters()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        {
            QFile big(source.filePath("big.bin"));
            QVERIFY(big.open(QIODevice::WriteOnly));
            QVERIFY(big.resize(qint64(512) * 1024 * 1024)); // long enough to still be on its way
        }
        p.agentA.dragged = {source.filePath("big.bin")};
        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        QSignalSpy finished(p.tb.get(), &FileTransfer::transferFinished);
        QSignalSpy progress(p.tb.get(), &FileTransfer::transferProgress);

        bool answered = false, ok = true;
        QString error;
        p.db->fetchDragged(p.agentB.hover.offer, [&](bool success, const QStringList &, const QString &why) {
            answered = true;
            ok = success;
            error = why;
        });
        // Data is on its way.
        QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty() && progress.last().at(3).toLongLong() > 0, 10000);
        QVERIFY(finished.isEmpty());
        QVERIFY(!answered);
        p.ia->handleSessionMessage(cmd("mm", {{"x", -50}, {"y", 0}})); // back to A: B's drag is cancelled
        QTRY_VERIFY_WITH_TIMEOUT(answered, 5000);
        QVERIFY(!ok);
        QVERIFY(!error.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(finished.last().at(3).toString(), QStringLiteral("cancelled"));
        // A later read of that drag is refused at once too.
        bool late = false;
        p.db->fetchDragged(p.agentB.hover.offer, [&](bool success, const QStringList &, const QString &) {
            late = !success;
        });
        QVERIFY(late);
    }

    // A device whose agent shows no drags (a Wayland session, macOS, or no agent running) is
    // shown nothing; files released on it are saved into its Received folder, with a notice, and
    // the move completes.
    void targetWithoutNativeDragsGetsTheFilesInReceived()
    {
        Pair p;
        p.agentB.nativeDrag = false;
        QVERIFY(p.setup());
        QVERIFY(!p.db->nativeDrag());
        QVERIFY(!p.b->capabilities().contains(QLatin1String(kDragCapability)));
        QTemporaryDir source;
        writeFile(source.filePath("notes.txt"), "notes");
        QVERIFY(QDir().mkpath(source.filePath("empty")));
        p.agentA.dragged = {source.filePath("notes.txt"), source.filePath("empty")};
        QSignalSpy noticesB(p.db.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_COMPARE(p.ia->activeDevice(), p.b->id());
        QVERIFY(p.ia->isDraggingFiles());
        QTest::qWait(200);
        QCOMPARE(p.agentB.hovers, 0); // nothing to show it with
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled, p.agentA.dragged, 15000);
        QCOMPARE(readFile(p.downloadsB.filePath("notes.txt")), QByteArray("notes"));
        QVERIFY(QFileInfo(p.downloadsB.filePath("empty")).isDir());
        QVERIFY(hasNotice(noticesB, QDir::toNativeSeparators(p.downloadsB.path())));
        // No staging folder left behind.
        QCOMPARE(entries(p.downloadsB.path()), QStringList({"empty", "notes.txt"}));
    }

    // An agent that cannot show the drag after all (no agent connected: the service gets no
    // answer) behaves the same way when the button is released there.
    void unavailableDragSavesIntoReceived()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};
        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        FileDrag::HoverEnd unavailable;
        unavailable.unavailable = true;
        std::exchange(p.agentB.done, nullptr)(unavailable);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled, p.agentA.dragged, 15000);
        QCOMPARE(readFile(p.downloadsB.filePath("a.txt")), QByteArray("a"));
    }

    // Released over a folder window or the desktop, the agent hands the folder back: the service
    // transfers into it (progress shown, no Explorer waiting inside the drop), and that counts as
    // a confirmed move.
    void releasedOverAFolderIsTransferredThere()
    {
        Pair p;
        QTemporaryDir folder;
        writeFile(folder.filePath("report.pdf"), "an older report"); // never overwritten
        p.agentB.dropFolder = folder.path();
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("report.pdf"), "the report");
        writeFile(source.filePath("photos/beach.jpg"), "sand");
        QVERIFY(QDir().mkpath(source.filePath("photos/empty")));
        p.agentA.dragged = {source.filePath("report.pdf"), source.filePath("photos")};
        QSignalSpy progress(p.tb.get(), &FileTransfer::transferFinished);

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled, p.agentA.dragged, 15000);
        QVERIFY(p.agentB.received.isEmpty()); // no virtual files were read
        QCOMPARE(readFile(folder.filePath("report.pdf")), QByteArray("an older report"));
        QCOMPARE(readFile(folder.filePath("report (2).pdf")), QByteArray("the report"));
        QCOMPARE(readFile(folder.filePath("photos/beach.jpg")), QByteArray("sand"));
        QVERIFY(QFileInfo(folder.filePath("photos/empty")).isDir());
        QCOMPARE(entries(folder.path()), QStringList({"photos", "report (2).pdf", "report.pdf"}));
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(2).toString(), QStringLiteral("drop")); // shown to the user
    }

    // Dragged on B (the master being A), over to A and back to B: on B the originals are dragged,
    // nothing is downloaded from B's own offer, and nothing is recycled (Windows moves them).
    void dragBackToARemoteSourceUsesTheOriginals()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        QVERIFY(QDir().mkpath(source.filePath("folder/empty")));
        p.agentB.dragged = {source.filePath("a.txt"), source.filePath("folder")};

        p.dragFromB();
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QVERIFY(p.ia->isDraggingFiles());
        QTRY_VERIFY(p.agentA.hovering);
        QVERIFY(!p.agentA.hover.own);
        QVERIFY(p.agentA.hover.local); // A's own mouse drives it there
        // Back onto B (the helper reports A's edge, past its return guard; the button is still held).
        QTest::qWait(400);
        p.ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}, {"lb", true}}));
        QTRY_COMPARE(p.ia->activeDevice(), p.b->id());
        QTRY_VERIFY(!p.agentA.hovering);
        QTRY_VERIFY(p.agentB.hovering);
        QVERIFY(p.agentB.hover.own);
        QVERIFY(!p.agentB.hover.local);
        QCOMPARE(p.agentB.hover.paths, p.agentB.dragged);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_VERIFY(!p.agentB.hovering);
        QTest::qWait(300);
        QVERIFY(p.agentB.recycled.isEmpty());
        QVERIFY(p.agentB.received.isEmpty());
        QVERIFY(entries(p.downloadsB.path()).isEmpty());
        QVERIFY(entries(p.downloadsA.path()).isEmpty());
    }

    // Listing a large dragged tree takes a while: the source answers "pending" at once, the master
    // waits up to kDragCatchExtendedMs instead of crossing without the files, and the source's
    // cursor says it is preparing them.
    void slowListingExtendsTheCatch()
    {
        Pair p;
        p.agentB.holdCatch = true;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("tree/a.txt"), "a");
        p.agentB.dragged = {source.filePath("tree")};

        p.dragFromB();
        QTRY_COMPARE(p.ia->catchSource(), p.b->id());
        // What a source still listing says (here sent by hand, the agent being held).
        QCborMap pending;
        pending.insert(QStringLiteral("t"), QStringLiteral("drag.caught"));
        pending.insert(QStringLiteral("pending"), true);
        pending.insert(QStringLiteral("n"), 1);
        p.b->sendMessage(p.a->id(), pending);
        QTest::qWait(InputController::kDragCatchTimeoutMs + 500);
        QCOMPARE(p.ia->catchSource(), p.b->id()); // still waiting
        QCOMPARE(p.ia->activeDevice(), p.b->id());

        QVERIFY(p.agentB.heldCatch);
        std::exchange(p.agentB.heldCatch, nullptr)(p.agentB.dragged);
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id());
        QVERIFY(p.ia->isDraggingFiles());
        QTRY_VERIFY(p.agentA.hovering);
        QCOMPARE(p.agentA.hover.files.size(), 2); // tree, tree/a.txt
        QVERIFY(p.agentB.badges.size() >= 2);
        QVERIFY(!p.agentB.badges.first().isEmpty());
        QVERIFY(p.agentB.badges.last().isEmpty());
    }

    // The device declares this one a guest while the catch is pending: nothing can be entered, so
    // the drag ends where it started (no drag left on with nowhere to drop) and the user is told.
    void refusedCrossingEndsTheDrag()
    {
        Pair p;
        p.agentA.holdCatch = true;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};
        QSignalSpy notices(p.da.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_VERIFY(bool(p.agentA.heldCatch));
        p.b->setTrust(p.a->id(), TrustLevel::Guest);
        QTRY_VERIFY(p.a->declaredTrust(p.b->id()) == TrustLevel::Guest);
        std::exchange(p.agentA.heldCatch, nullptr)(p.agentA.dragged);
        QTRY_VERIFY(hasNotice(notices, QStringLiteral("takes no keyboard and mouse")));
        QCOMPARE(p.ia->activeDevice(), p.a->id());
        QVERIFY(!p.ia->isDraggingFiles());
        QCOMPARE(p.agentB.hovers, 0);
        QVERIFY(p.agentA.recycled.isEmpty());
    }

    // A device older than 0.1.22 says ok as soon as the drop target read the virtual files, without
    // any confirmation that it kept them (the user's case: the file arrived in Received\Dragged,
    // the target never saved it): the source keeps the originals.
    void olderTargetsOkIsTakenAsACopy()
    {
        Pair p;
        QVERIFY(p.setup());
        QVERIFY(announceVersion(*p.b, *p.a, QStringLiteral("0.1.19")));
        QTemporaryDir source;
        writeFile(source.filePath("big.7z"), "archive");
        p.agentA.dragged = {source.filePath("big.7z")};
        QSignalSpy notices(p.da.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_COMPARE_WITH_TIMEOUT(p.agentB.received.size(), 1, 15000); // B answers a plain ok
        QTRY_VERIFY_WITH_TIMEOUT(hasNotice(notices, QStringLiteral("originals were kept")), 10000);
        QTest::qWait(200);
        QVERIFY(p.agentA.recycled.isEmpty());
        QVERIFY(QFile::exists(source.filePath("big.7z")));
    }

    // The source said it was listing (pending) but took longer than kDragCatchExtendedMs: the
    // crossing goes ahead without the files, the master says nothing was moved, and the late
    // answer has the source withdraw its offer (nothing can fetch it any more).
    void catchPastTheExtendedDeadlineTellsTheUser()
    {
        Pair p;
        p.agentB.holdCatch = true;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("tree/a.txt"), "a");
        p.agentB.dragged = {source.filePath("tree")};
        QSignalSpy notices(p.da.get(), &FileDrag::notice);
        QSignalSpy timedOut(p.ia.get(), &InputController::dragCatchTimedOut);

        p.dragFromB();
        QTRY_COMPARE(p.ia->catchSource(), p.b->id());
        QCborMap pending;
        pending.insert(QStringLiteral("t"), QStringLiteral("drag.caught"));
        pending.insert(QStringLiteral("pending"), true);
        pending.insert(QStringLiteral("n"), 1);
        p.b->sendMessage(p.a->id(), pending);
        QTRY_COMPARE_WITH_TIMEOUT(timedOut.size(), 1, InputController::kDragCatchExtendedMs + 3000);
        QCOMPARE(timedOut.first().at(0).toByteArray(), p.b->id());
        QTRY_COMPARE(p.ia->activeDevice(), p.a->id()); // crossed as usual
        QVERIFY(!p.ia->isDraggingFiles());
        QVERIFY(hasNotice(notices, QStringLiteral("took too long")));

        // The listing ends after all: B publishes and answers, A has it withdrawn.
        quint64 offer = 0;
        const auto watch = connect(p.a.get(), &Node::messageReceived, this, [&](const QByteArray &, const QCborMap &m) {
            if (m.value(QStringLiteral("t")).toString() == QLatin1String("drag.caught"))
                offer = quint64(m.value(QStringLiteral("o")).toInteger());
        });
        std::exchange(p.agentB.heldCatch, nullptr)(p.agentB.dragged);
        QTRY_VERIFY(offer != 0);
        QTest::qWait(300);
        QCOMPARE(p.agentA.hovers, 0);
        QTemporaryDir into;
        bool answered = false, ok = true;
        p.ta->fetch(p.b->id(), offer, into.path(), true, 10, [&](const FetchResult &r) {
            answered = true;
            ok = r.ok;
        });
        QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
        disconnect(watch);
        QVERIFY(!ok);
    }

    // A catch replaced by a newer one (the first was given up, the button crossed again) must not
    // answer: drag.caught names no catch, so "no files" from the old one would end the new one.
    void replacedCatchDoesNotAnswerTheNewOne()
    {
        Pair p;
        p.agentA.holdCatch = true;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};

        p.dragAcross();
        QTRY_VERIFY(bool(p.agentA.heldCatch));
        auto first = std::exchange(p.agentA.heldCatch, nullptr);
        // No answer in time: crossed as usual. Back, and across again with the button held.
        QTRY_COMPARE_WITH_TIMEOUT(p.ia->activeDevice(), p.b->id(), InputController::kDragCatchTimeoutMs + 2000);
        p.returnAndDragAgain();
        QTRY_VERIFY(bool(p.agentA.heldCatch));
        QCOMPARE(p.ia->catchSource(), p.a->id());
        first(QStringList()); // the old catch finds nothing (any answer would do)
        QTest::qWait(200);
        QCOMPARE(p.ia->catchSource(), p.a->id()); // still waiting for the new one
        std::exchange(p.agentA.heldCatch, nullptr)(p.agentA.dragged);
        QTRY_VERIFY(p.agentB.hovering);
        QVERIFY(p.ia->isDraggingFiles());
    }

    // Files released on B are still being taken there when another drag starts on A: the first
    // drag's offer stays published and its confirmed result still recycles the originals.
    void aReleasedDragOutlivesTheNextCatch()
    {
        Pair p;
        p.agentB.holdDrop = true;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("first.txt"), "first");
        writeFile(source.filePath("second.txt"), "second");
        p.agentA.dragged = {source.filePath("first.txt")};

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_VERIFY(bool(p.agentB.heldDrop));
        auto firstDrop = std::exchange(p.agentB.heldDrop, nullptr);
        p.agentB.heldEnd = nullptr;

        p.agentB.holdDrop = false;
        p.agentA.dragged = {source.filePath("second.txt")};
        p.returnAndDragAgain();
        QTRY_VERIFY(p.agentB.hovering);
        QCOMPARE(p.agentB.hover.files.first().path, QString("second.txt"));

        firstDrop(); // the first drop target reads its files only now
        QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled, QStringList({source.filePath("first.txt")}), 15000);
        QCOMPARE(p.agentB.received.size(), 1);
        QCOMPARE(readFile(p.agentB.received.first()), QByteArray("first"));
    }

    // The folder the agent hands back must be an absolute path to an existing folder: anything
    // else is not written to (the files go into Received, which says where).
    void unusableDropFolderGoesToReceived()
    {
        for (const QString &bad : {QStringLiteral("relative-drop-folder"), QStringLiteral("/no/such/deskflower/folder")}) {
            Pair p;
            p.agentB.dropFolder = bad;
            QVERIFY(p.setup());
            QTemporaryDir source;
            writeFile(source.filePath("a.txt"), "a");
            p.agentA.dragged = {source.filePath("a.txt")};
            QSignalSpy noticesB(p.db.get(), &FileDrag::notice);

            p.dragAcross();
            QTRY_VERIFY(p.agentB.hovering);
            p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
            QTRY_COMPARE_WITH_TIMEOUT(p.agentA.recycled, p.agentA.dragged, 15000);
            QCOMPARE(readFile(p.downloadsB.filePath("a.txt")), QByteArray("a"));
            QVERIFY(hasNotice(noticesB, QDir::toNativeSeparators(p.downloadsB.path())));
            QVERIFY(!QFileInfo::exists(bad));
        }
    }

    // The agent showing the drag goes away after the release (it crashed): the service answers
    // the hover as lost; nothing is taken, both devices say so, the originals stay.
    void lostDragAfterTheReleaseTellsBothSides()
    {
        Pair p;
        p.agentB.holdDrop = true;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};
        QSignalSpy noticesA(p.da.get(), &FileDrag::notice);
        QSignalSpy noticesB(p.db.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_VERIFY(bool(p.agentB.heldEnd));
        p.agentB.heldDrop = nullptr;
        FileDrag::HoverEnd lost;
        lost.lost = true;
        std::exchange(p.agentB.heldEnd, nullptr)(lost);
        QVERIFY(hasNotice(noticesB, QStringLiteral("ended unexpectedly")));
        QTRY_VERIFY_WITH_TIMEOUT(hasNotice(noticesA, QStringLiteral("not moved")), 10000);
        QVERIFY(p.agentA.recycled.isEmpty());
        QVERIFY(entries(p.downloadsB.path()).isEmpty());
    }

    // ...and before the release: the drag there is over (with a notice); the release takes nothing.
    void lostDragBeforeTheReleaseTakesNothing()
    {
        Pair p;
        QVERIFY(p.setup());
        QTemporaryDir source;
        writeFile(source.filePath("a.txt"), "a");
        p.agentA.dragged = {source.filePath("a.txt")};
        QSignalSpy noticesA(p.da.get(), &FileDrag::notice);
        QSignalSpy noticesB(p.db.get(), &FileDrag::notice);

        p.dragAcross();
        QTRY_VERIFY(p.agentB.hovering);
        FileDrag::HoverEnd lost;
        lost.lost = true;
        std::exchange(p.agentB.done, nullptr)(lost);
        QVERIFY(hasNotice(noticesB, QStringLiteral("stopped")));
        p.ia->handleSessionMessage(cmd("mb", {{"b", 1}, {"d", false}}));
        QTRY_VERIFY_WITH_TIMEOUT(hasNotice(noticesA, QStringLiteral("not moved")), 10000);
        QVERIFY(p.agentA.recycled.isEmpty());
        QVERIFY(entries(p.downloadsB.path()).isEmpty());
    }

    // The service's requests to its agents: only an agent asked may answer, the first answer wins,
    // and once every agent asked is gone the request is answered (empty) at once, not at its
    // timeout (a drag shown by an agent that crashed ends straight away).
    void agentRequestsEndWhenTheirAgentsLeave()
    {
        struct Client {};
        Client one, two, other;
        AgentRequests<Client> requests;
        QList<QCborMap> answers;
        auto record = [&](const QCborMap &m) { answers.append(m); };
        QCborMap reply;
        reply.insert(QStringLiteral("dropped"), true);

        const qint64 r1 = requests.add(record, {&one, &two});
        QVERIFY(!requests.reply(r1, &other, reply)); // not asked
        requests.disconnected(&one);
        QVERIFY(requests.isPending(r1)); // two may still answer
        requests.disconnected(&two);
        QVERIFY(!requests.isPending(r1));
        QCOMPARE(answers.size(), 1);
        QVERIFY(answers.last().isEmpty());

        const qint64 r2 = requests.add(record, {&one, &two});
        QVERIFY(requests.reply(r2, &two, reply));
        QVERIFY(!requests.reply(r2, &one, reply)); // answered already
        requests.expire(r2);
        requests.disconnected(&one);
        QCOMPARE(answers.size(), 2);
        QVERIFY(answers.last().value(QStringLiteral("dropped")).toBool());

        const qint64 r3 = requests.add(record, {&one});
        requests.expire(r3);
        QCOMPARE(answers.size(), 3);
        QVERIFY(answers.last().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestFileDrag)
#include "tst_filedrag.moc"
