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

// One device: node, file transfer and clipboard sync with its own downloads folder.
struct Device {
    QTemporaryDir config;
    QTemporaryDir downloads;
    std::unique_ptr<Node> node;
    std::unique_ptr<FileTransfer> transfer;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<ClipboardSync> clip;
    std::unique_ptr<QSignalSpy> applied;

    Device()
    {
        node = test::makeNode(config.path());
        transfer = std::make_unique<FileTransfer>(node.get());
        const QString dl = downloads.path();
        settings = std::make_unique<Settings>(config.path());
        clip = std::make_unique<ClipboardSync>(node.get(), transfer.get(), settings.get(), [dl] { return dl; });
        applied = std::make_unique<QSignalSpy>(clip.get(), &ClipboardSync::applyLocally);
    }
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

        ClipboardData rich = text("hello");
        rich.insert(QStringLiteral("text/html"), "<b>hello</b>");
        rich.insert(QStringLiteral("application/x-ignored"), "dropped");
        a.clip->localChanged(rich);
        QTRY_COMPARE(b.applied->size(), 1);
        rich.remove(QStringLiteral("application/x-ignored"));
        QCOMPARE(b.lastFormats(), rich);

        // B's helper reports the content it just applied: no echo back to A.
        b.clip->localChanged(rich);
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);

        b.clip->localChanged(text("from B"));
        QTRY_COMPARE(a.applied->size(), 1);
        QCOMPARE(a.lastFormats(), text("from B"));
    }

    void disabledDeviceNeitherSendsNorReceives()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        b.clip->setEnabled(false);
        a.clip->localChanged(text("secret"));
        b.clip->localChanged(text("other secret"));
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);
        QCOMPARE(b.applied->size(), 0);

        Settings reloadedSettings(b.config.path());
        ClipboardSync reloaded(b.node.get(), b.transfer.get(), &reloadedSettings, [] { return QString(); });
        QVERIFY(!reloaded.isEnabled());
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
        QCOMPARE(b.lastFormats(), data);
    }

    void copiedFilesArePastedAsLocalCopies()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTemporaryDir source;
        writeFile(source.filePath("notes.txt"), "remember the milk");
        writeFile(source.filePath("project/src/main.cpp"), "int main() {}");
        writeFile(source.filePath("project/README.md"), "# project");

        a.clip->localChanged({}, {source.filePath("notes.txt"), source.filePath("project")});
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
        QTest::qWait(300);
        QCOMPARE(a.applied->size(), 0);
    }

    void gossipReachesDevicesPairedThroughAThirdOne()
    {
        Device a, b, c;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QVERIFY(test::pairNodes(*b.node, *c.node)); // A and C are not paired
        QTemporaryDir source;
        writeFile(source.filePath("photo.jpg"), QByteArray(500000, 'j'));

        a.clip->localChanged(text("via B"), {source.filePath("photo.jpg")});
        QTRY_COMPARE_WITH_TIMEOUT(c.applied->size(), 1, 15000);
        QCOMPARE(c.lastFormats(), text("via B"));
        // Files are only offered by the device they were copied on: B never transferred them, so
        // C (not paired with A) gets the text only. B itself can paste them.
        QCOMPARE(c.lastPending(), 0);
        QCOMPARE(b.lastPending(), 1);
        const QStringList atB = b.paste();
        QCOMPARE(atB.size(), 1);
        QCOMPARE(readFile(atB.first()), QByteArray(500000, 'j'));
    }

    void oversizedFormatIsNotShared()
    {
        Device a, b;
        QVERIFY(test::pairNodes(*a.node, *b.node));
        a.clip->localChanged({{QStringLiteral("image/png"), QByteArray(ClipboardSync::kMaxFormatBytes + 1, 'x')}});
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
    }

    void olderClipboardDoesNotWinAfterReconnect()
    {
        Device a, b;
        a.clip->localChanged(text("old"));
        QTest::qWait(20);
        b.clip->localChanged(text("new"));
        QVERIFY(test::pairNodes(*a.node, *b.node));
        QTRY_COMPARE(a.applied->size(), 1);
        QCOMPARE(a.lastFormats(), text("new"));
        QTest::qWait(300);
        QCOMPARE(b.applied->size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestClipboard)
#include "tst_clipboard.moc"
