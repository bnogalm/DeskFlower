// Dragging files between two paired devices with fake agents: A (the master) drags two items
// across its right edge with the button held; the drag is shown on B while the cursor is there;
// releasing it there makes B's (fake) drop target read the files, which transfers them, and A's
// originals go to the (fake) Recycle Bin. Also: no files, dropping back on A, no drop target.

#include "filedrag.h"
#include "filetransfer.h"
#include "inputcontroller.h"
#include "testutil.h"
#include "workspace.h"

#include <QDir>
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
    bool acceptDrop = true;       // something under the cursor takes the drop
    QStringList recycled;
    QPoint caughtAt;
    // The drag shown here.
    bool hovering = false;
    FileDrag::Hover hover;
    std::function<void(bool, bool)> done;
    QStringList received;         // what the drop target got
    int hovers = 0;

    FileDrag::Agent api()
    {
        FileDrag::Agent a;
        a.catchFiles = [this](QPoint at, std::function<void(const QStringList &)> d) {
            caughtAt = at;
            d(dragged);
        };
        a.hover = [this](const FileDrag::Hover &h, std::function<void(bool, bool)> d) {
            hovering = true;
            hover = h;
            done = std::move(d);
            ++hovers;
        };
        a.endHover = [this](bool drop) {
            hovering = false;
            auto d = std::move(done);
            if (!drop || !acceptDrop) {
                d(false, false);
                return;
            }
            if (hover.own) { // Windows moves the originals itself
                d(true, false);
                return;
            }
            // The drop target reads the virtual files: they are transferred now.
            drag->fetchDragged(hover.offer, [this, d](bool ok, const QStringList &items, const QString &) {
                received = items;
                d(true, ok);
            });
        };
        a.recycle = [this](const QStringList &paths, std::function<void(bool, const QString &)> d) {
            recycled = paths;
            d(true, {});
        };
        return a;
    }
};

struct Pair {
    QTemporaryDir dirA, dirB, downloadsA, downloadsB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Workspace> wa, wb;
    std::unique_ptr<InputController> ia, ib;
    std::unique_ptr<FileTransfer> ta, tb;
    FakeAgent agentA, agentB;
    std::unique_ptr<FileDrag> da, db;

    bool setup()
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        wa = std::make_unique<Workspace>(a.get(), dirA.path());
        wb = std::make_unique<Workspace>(b.get(), dirB.path());
        wa->setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        wb->setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}});
        if (!test::pairNodes(*a, *b))
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
        return true;
    }

    void dragAcross()
    {
        ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 500}, {"lb", true}}));
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
        QCOMPARE(p.agentB.hover.files.size(), 2);
        QCOMPARE(p.agentB.hover.files.first().path, QString("report.pdf"));
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
};

QTEST_GUILESS_MAIN(TestFileDrag)
#include "tst_filedrag.moc"
