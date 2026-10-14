#include "clipboardsync.h"
#include "filetransfer.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
ClipboardData text(const QString &s) { return {{QStringLiteral("text/plain"), s.toUtf8()}}; }

// One device: node, file transfer and clipboard sync with its own downloads folder, and where it
// believes the shared cursor is (what InputController would say).
struct Device {
    QTemporaryDir config;
    QTemporaryDir downloads;
    std::unique_ptr<Node> node;
    std::unique_ptr<FileTransfer> transfer;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<ClipboardSync> clip;
    std::unique_ptr<QSignalSpy> applied;
    bool cursorHere = true;
    QByteArray master;

    Device()
    {
        node = test::makeNode(config.path());
        transfer = std::make_unique<FileTransfer>(node.get());
        const QString dl = downloads.path();
        settings = std::make_unique<Settings>(config.path());
        ClipboardSync::Presence presence{[this](qint64) { return cursorHere; }, [this] { return master; }};
        clip = std::make_unique<ClipboardSync>(node.get(), transfer.get(), settings.get(), presence,
                                               [dl] { return dl; });
        applied = std::make_unique<QSignalSpy>(clip.get(), &ClipboardSync::applyLocally);
    }
    QByteArray id() const { return node->id(); }
    ClipboardData lastFormats() const
    {
        return applied->isEmpty() ? ClipboardData() : applied->last().at(0).value<ClipboardData>();
    }
    QStringList lastFiles() const { return applied->isEmpty() ? QStringList() : applied->last().at(1).toStringList(); }
    int lastPending() const { return applied->isEmpty() ? 0 : applied->last().at(2).toInt(); }
    // What a paste gets: the files transferred on demand.
    QStringList paste()
    {
        bool done = false;
        QStringList files;
        clip->fetchFiles([&](bool ok, const QStringList &f, const QString &) {
            done = true;
            if (ok)
                files = f;
        });
        QTest::qWaitFor([&] { return done; }, 15000);
        return files;
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
} // namespace

class TestClipboard : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<df::ClipboardData>("df::ClipboardData"); }

    void copyOnOneDevicePasteOnTheOther()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});

        ClipboardData rich = text("hello");
        rich.insert(QStringLiteral("text/html"), "<b>hello</b>");
        rich.insert(QStringLiteral("application/x-ignored"), "dropped");
        a.clip->localChanged(rich);
        // Copying sends nothing: the clipboard waits for the cursor.
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);

        moveCursor(a, a, b);
        QTRY_COMPARE(b.applied->size(), 1);
        rich.remove(QStringLiteral("application/x-ignored"));
        QCOMPARE(b.lastFormats(), rich);

        // B's helper reports the content it just applied, and the cursor goes back: no echo.
        b.clip->localChanged(rich);
        moveCursor(a, b, a);
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);

        // Copied on B (with A's keyboard), pasted on A after the cursor comes back.
        moveCursor(a, a, b);
        b.clip->localChanged(text("from B"));
        moveCursor(a, b, a);
        QTRY_COMPARE(a.applied->size(), 1);
        QCOMPARE(a.lastFormats(), text("from B"));
    }

    void onlyTheDeviceTheCursorMovesToGetsIt()
    {
        Device a, b, c;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*a.node, *c.node));
        QVERIFY(test::pairNodes(*b.node, *c.node));
        shareFrom(a, {&b, &c});

        a.clip->localChanged(text("password"));
        moveCursor(a, a, b);
        QTRY_COMPARE(b.applied->size(), 1);
        QTest::qWait(300);
        QCOMPARE(c.applied->size(), 0); // never visited

        // B to C: B hands what it has on to C only.
        moveCursor(a, b, c);
        QTRY_COMPARE(c.applied->size(), 1);
        QCOMPARE(c.lastFormats(), text("password"));
    }

    void noClipboardWhereTheCursorIsNot()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        a.clip->localChanged(text("secret"));
        // The cursor is not on B (e.g. it already moved on): B ignores what arrives.
        b.cursorHere = false;
        a.clip->cursorMoved(a.id(), b.id());
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
    }

    void onlyTheMasterMovesClipboards()
    {
        Device a, b, c;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*b.node, *c.node));
        QVERIFY(test::pairNodes(*a.node, *c.node));
        shareFrom(a, {&b, &c});
        b.clip->localChanged(text("secret on B"));
        c.clip->localChanged(text("text on C"));
        b.cursorHere = true;
        c.cursorHere = true;
        // C is not the master: it can neither pull B's clipboard nor push its own onto B.
        c.clip->cursorMoved(b.id(), c.id());
        c.clip->cursorMoved(c.id(), b.id());
        QTest::qWait(400);
        QCOMPARE(c.applied->size(), 0);
        QCOMPARE(b.applied->size(), 0);
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

    void largeImageTravelsOverTcp()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        QByteArray image(3 * 1024 * 1024, Qt::Uninitialized);
        for (int i = 0; i < image.size(); ++i)
            image[i] = char(i * 31);
        ClipboardData data = text("caption");
        data.insert(QStringLiteral("image/png"), image);
        a.clip->localChanged(data);
        moveCursor(a, a, b);
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        QCOMPARE(b.lastFormats(), data);
    }

    void copiedFilesArePastedAsLocalCopies()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        QTemporaryDir source;
        writeFile(source.filePath("notes.txt"), "remember the milk");
        writeFile(source.filePath("project/src/main.cpp"), "int main() {}");
        writeFile(source.filePath("project/README.md"), "# project");

        a.clip->localChanged({}, {source.filePath("notes.txt"), source.filePath("project")});
        moveCursor(a, a, b);
        QTRY_COMPARE_WITH_TIMEOUT(b.applied->size(), 1, 15000);
        // Copying only announces the files (three, folder contents included): nothing travels yet.
        QVERIFY(b.lastFiles().isEmpty());
        QCOMPARE(b.lastPending(), 3);
        QVERIFY(QDir(b.downloads.filePath("Clipboard")).isEmpty() || !QDir(b.downloads.filePath("Clipboard")).exists());

        // Pasting transfers them.
        const QStringList files = b.paste();
        QCOMPARE(files.size(), 2);
        for (const QString &f : files)
            QVERIFY2(f.startsWith(QDir::cleanPath(b.downloads.path() + "/Clipboard/")), qPrintable(f));
        const QString root = QFileInfo(files.first()).absolutePath();
        QCOMPARE(readFile(root + "/notes.txt"), QByteArray("remember the milk"));
        QCOMPARE(readFile(root + "/project/src/main.cpp"), QByteArray("int main() {}"));
        QCOMPARE(readFile(root + "/project/README.md"), QByteArray("# project"));

        // B's helper reports the local copies it just put on the clipboard: no echo.
        b.clip->localChanged({}, files);
        moveCursor(a, b, a);
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);
    }

    void oversizedFormatIsNotShared()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        a.clip->localChanged({{QStringLiteral("image/png"), QByteArray(ClipboardSync::kMaxFormatBytes + 1, 'x')}});
        moveCursor(a, a, b);
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
    }

    void olderClipboardDoesNotWin()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        shareFrom(a, {&b});
        b.clip->localChanged(text("old"));
        QTest::qWait(20);
        a.clip->localChanged(text("new"));
        // The cursor comes back from B with B's older clipboard: A keeps its own.
        moveCursor(a, b, a);
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestClipboard)
#include "tst_clipboard.moc"
