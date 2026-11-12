// macOS only (tests/CMakeLists.txt): ClipboardBridge on the real pasteboard, with the cocoa
// platform (the offscreen one has no pasteboard). It overwrites the user's clipboard: skipped
// unless DESKFLOWER_DESKTOP_TESTS=1 (ctest label "desktop").
//  - what another program copies is noticed, although the application is never activated (the
//    pasteboard's change count is polled);
//  - sealed content is put as lazily provided items: nothing is decrypted or transferred when it is
//    set, its own change is not reported back, and reading it renders each format and transfers
//    the files once, one file URL per top-level item;
//  - without the list of names (a guest's copy, a long one) the files are still pasteable: one
//    item with every path, the files themselves replacing it once transferred;
//  - a refused transfer is asked once per paste, not once per item.

#include "clipboardbridge.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#import <AppKit/AppKit.h>

class TestMacClipboard : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qgetenv("DESKFLOWER_DESKTOP_TESTS") != "1")
            QSKIP("Uses the real pasteboard: set DESKFLOWER_DESKTOP_TESTS=1 to run it");
        if (QGuiApplication::platformName() != QLatin1String("cocoa"))
            QSKIP("Needs the cocoa platform (QT_QPA_PLATFORM=cocoa)");
    }

    void copiesElsewhereAreNoticed()
    {
        ClipboardBridge bridge;
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        QProcess pbcopy;
        pbcopy.start(QStringLiteral("pbcopy"));
        QVERIFY(pbcopy.waitForStarted(5000));
        pbcopy.write("copied by pbcopy");
        pbcopy.closeWriteChannel();
        QVERIFY(pbcopy.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 3000);
        const QCborMap formats = changed.last().at(0).value<QCborMap>();
        QCOMPARE(QString::fromUtf8(formats.value(QStringLiteral("text/plain")).toByteArray()),
                 QStringLiteral("copied by pbcopy"));
    }

    void sealedContentIsRenderedOnlyWhenRead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath(QStringLiteral("a.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("abc");
        file.close();
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Folder/Empty")));

        ClipboardBridge bridge;
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        int unseals = 0;
        int fetches = 0;
        bridge.applySealed(
            {ClipboardBridge::SealedFormat{QStringLiteral("text/plain"), 5}},
            {ClipboardBridge::ListedFile{QStringLiteral("a.txt"), 3, false},
             ClipboardBridge::ListedFile{QStringLiteral("Folder"), 0, true},
             ClipboardBridge::ListedFile{QStringLiteral("Folder/Empty"), 0, true}},
            3,
            [&unseals](const QString &mime, QString *) {
                ++unseals;
                return mime == QLatin1String("text/plain") ? QByteArray("hello") : QByteArray();
            },
            [&fetches, &dir](QString *) {
                ++fetches;
                return QStringList{dir.filePath(QStringLiteral("a.txt")), dir.filePath(QStringLiteral("Folder"))};
            });
        QVERIFY(bridge.ownsClipboard());
        QTest::qWait(1000); // the change poll ran meanwhile
        QCOMPARE(changed.size(), 0); // its own content is not reported back
        QCOMPARE(unseals, 0);
        QCOMPARE(fetches, 0);

        const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime);
        const QList<QUrl> urls = mime->urls();
        QCOMPARE(fetches, 1);
        QCOMPARE(urls.size(), 2); // one item per top-level file or folder
        const auto canonical = [](const QString &path) { return QFileInfo(path).canonicalFilePath(); };
        QCOMPARE(canonical(urls.at(0).toLocalFile()), canonical(dir.filePath(QStringLiteral("a.txt"))));
        QCOMPARE(canonical(urls.at(1).toLocalFile()), canonical(dir.filePath(QStringLiteral("Folder"))));
        QCOMPARE(mime->text(), QStringLiteral("hello"));
        QCOMPARE(unseals, 1);
        QCOMPARE(fetches, 1); // transferred once for every item
    }

    void unlistedFilesArePasteable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("One")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Two/Nested")));
        const QStringList paths{dir.filePath(QStringLiteral("One")), dir.filePath(QStringLiteral("Two"))};

        ClipboardBridge bridge;
        bridge.setEnabled(true);
        int fetches = 0;
        bridge.applySealed({}, {}, 2, [](const QString &, QString *) { return QByteArray(); },
                           [&fetches, paths](QString *) {
                               ++fetches;
                               return paths;
                           });
        QCOMPARE(fetches, 0);

        // The legacy list: whether the pasteboard asks the item for it or builds it from the
        // items' file URLs (macOS 15 does, giving only the one item's path), the files are
        // transferred once and the first is there. (The session helper lists an unlisted copy on
        // demand and writes one item per top-level file or folder, so a first paste gets them all.)
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSArray *listed = [pasteboard propertyListForType:@"NSFilenamesPboardType"];
        QCOMPARE(fetches, 1);
        QVERIFY([listed isKindOfClass:[NSArray class]]);
        QVERIFY(int(listed.count) >= 1);
        const auto canonical = [](const QString &path) { return QFileInfo(path).canonicalFilePath(); };
        QCOMPARE(canonical(QString::fromNSString(listed[0])), canonical(paths.at(0)));

        // Then the files themselves, one file URL each.
        QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->mimeData()->urls().size(), 2, 3000);
        QCOMPARE(fetches, 1);
    }

    void refusedTransferIsAskedOncePerPaste()
    {
        ClipboardBridge bridge;
        bridge.setEnabled(true);
        int fetches = 0;
        bridge.applySealed({},
                           {ClipboardBridge::ListedFile{QStringLiteral("a.txt"), 3, false},
                            ClipboardBridge::ListedFile{QStringLiteral("b.txt"), 3, false},
                            ClipboardBridge::ListedFile{QStringLiteral("c.txt"), 3, false}},
                           3, [](const QString &, QString *) { return QByteArray(); },
                           [&fetches](QString *error) {
                               ++fetches;
                               *error = QStringLiteral("refused");
                               return QStringList();
                           });
        QVERIFY(QGuiApplication::clipboard()->mimeData()->urls().isEmpty());
        QCOMPARE(fetches, 1); // the other items gave up at once
        QTest::qWait(2000);   // a later paste asks again
        QVERIFY(QGuiApplication::clipboard()->mimeData()->urls().isEmpty());
        QCOMPARE(fetches, 2);
    }
};

QTEST_MAIN(TestMacClipboard)
#include "tst_macclipboard.moc"
