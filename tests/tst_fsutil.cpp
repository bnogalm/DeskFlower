// Links planted in a user's folder must not redirect what the service writes or deletes there.

#include "fsutil.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace df;
using test::makeJunction;

namespace {
void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(content);
}
#ifdef Q_OS_WIN
// A rename as another process (or the user in Explorer) would make it.
bool renames(const QString &from, const QString &to)
{
    return MoveFileExW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(from).utf16()),
                       reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(to).utf16()), 0);
}
#endif
} // namespace

class TestFsUtil : public QObject
{
    Q_OBJECT
private slots:
    void inside()
    {
        QVERIFY(fs::isInside("C:/Users/ana/Deskflower", "C:/Users/ana"));
        QVERIFY(fs::isInside("C:/Users/ana", "C:/Users/ana/"));
        QVERIFY(!fs::isInside("C:/Users/anabel", "C:/Users/ana"));
        QVERIFY(!fs::isInside("C:/Users/ana/../bob", "C:/Users/ana"));
        QVERIFY(!fs::isInside("C:/Windows/System32", "C:/Users/ana"));
#ifdef Q_OS_WIN
        QVERIFY(fs::isInside("c:/users/ANA/x", "C:/Users/ana"));
#endif
    }

    void removeTreeDoesNotFollowLinks()
    {
        QTemporaryDir outside, folder;
        writeFile(outside.filePath("precious.txt"), "keep me");
        writeFile(folder.filePath("old/a.txt"), "a");
        QVERIFY(makeJunction(folder.filePath("old/link"), outside.path()));

        QVERIFY(fs::removeTree(folder.filePath("old")));
        QVERIFY(!QFileInfo::exists(folder.filePath("old")));
        QVERIFY(QFile::exists(outside.filePath("precious.txt")));
    }

    // A stop asked for ends the walk at once, leaving the rest in place.
    void removeTreeStopsWhenAsked()
    {
        QTemporaryDir folder;
        for (int i = 0; i < 20; ++i)
            writeFile(folder.filePath(QStringLiteral("old/%1.txt").arg(i)), "x");
        int asked = 0;
        QVERIFY(!fs::removeTree(folder.filePath("old"), [&] { return ++asked > 5; }));
        QVERIFY(QFileInfo::exists(folder.filePath("old")));
        QVERIFY(!QDir(folder.filePath("old")).isEmpty());
        QVERIFY(fs::removeTree(folder.filePath("old"), [] { return false; }));
        QVERIFY(!QFileInfo::exists(folder.filePath("old")));
    }

    void linksOnThePathAreSeen()
    {
#ifndef Q_OS_WIN
        QSKIP("Only checked on Windows, where the service runs as LocalSystem");
#else
        QTemporaryDir outside, folder;
        QVERIFY(!fs::passesThroughLink(folder.filePath("Clipboard/123")));
        QVERIFY(makeJunction(folder.filePath("Clipboard"), outside.path()));
        QVERIFY(fs::passesThroughLink(folder.filePath("Clipboard")));
        QVERIFY(fs::passesThroughLink(folder.filePath("Clipboard/123/file.txt")));
#endif
    }

    // A junction on the root's path (an existing folder above it, or the root itself) is refused
    // at open(); a root that does not exist yet is made by ensureDir("").
    void safeTreeRefusesJunctionInRoot()
    {
#ifndef Q_OS_WIN
        QSKIP("Only checked on Windows, where the service runs as LocalSystem");
#else
        QTemporaryDir outside, folder;
        QVERIFY(makeJunction(folder.filePath("Clipboard"), outside.path()));
        QString error;
        fs::SafeTree through;
        QVERIFY(!through.open(folder.filePath("Clipboard/123"), &error));
        QCOMPARE(error, QString("The destination folder goes through a link"));
        fs::SafeTree at;
        QVERIFY(!at.open(folder.filePath("Clipboard"), &error));
        QVERIFY(!QFileInfo::exists(outside.filePath("123")));

        fs::SafeTree fresh;
        QVERIFY2(fresh.open(folder.filePath("Received/123"), &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(folder.filePath("Received"))); // nothing made yet
        QVERIFY2(fresh.ensureDir(QString(), &error), qPrintable(error));
        QVERIFY(QFileInfo(folder.filePath("Received/123")).isDir());
#endif
    }

    // A junction where a folder is to be made (planted before, or in place of one not made yet)
    // is refused, and nothing is made through it.
    void safeTreeRefusesJunctionSubdir()
    {
        QTemporaryDir outside, folder;
        const QString root = folder.filePath("dl");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(makeJunction(root + "/sub", outside.path()));
        fs::SafeTree tree;
        QString error;
        QVERIFY2(tree.open(root, &error), qPrintable(error));
        QVERIFY2(tree.ensureDir("ok/deeper", &error), qPrintable(error));
#ifdef Q_OS_WIN
        QVERIFY(!tree.ensureDir("sub", &error));
        QCOMPARE(error, QString("The destination goes through a link"));
        QVERIFY(!tree.ensureDir("sub/inner", &error));
        QVERIFY(!QFileInfo::exists(outside.filePath("inner")));
#endif
        tree.close();
        QVERIFY(fs::removeTree(root));
    }

    // While the tree is open its folders, and the root's folders above it, cannot be renamed
    // (nor replaced by a junction); once closed they can.
    void safeTreePinsAgainstRename()
    {
#ifndef Q_OS_WIN
        QSKIP("Held open on Windows only");
#else
        QTemporaryDir folder;
        const QString parent = folder.filePath("Received");
        const QString root = parent + "/123";
        QVERIFY(QDir().mkpath(root));
        fs::SafeTree tree;
        QString error;
        QVERIFY2(tree.open(root, &error), qPrintable(error));
        QVERIFY2(tree.ensureDir("a/b", &error), qPrintable(error));
        QVERIFY(tree.heldForTests() >= 4); // the root, a, a/b and the folders above
        QVERIFY(!renames(root + "/a/b", root + "/a/c"));
        QVERIFY(!renames(root + "/a", root + "/x"));
        QVERIFY(!renames(root, parent + "/124"));
        QVERIFY(!renames(parent, folder.filePath("Moved")));
        QVERIFY(!QDir(root + "/a").removeRecursively());
        // Files can still be made and written inside.
        QFile f(root + "/a/b/file.txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write("x"), 1);
        f.close();
        tree.close();
        QCOMPARE(tree.heldForTests(), 0);
        QVERIFY(renames(root + "/a/b", root + "/a/c"));
        QVERIFY(renames(parent, folder.filePath("Moved")));
#endif
    }

    // A folder made once is a lookup afterwards: no disk work per file.
    void safeTreeCachesDirectories()
    {
        QTemporaryDir folder;
        fs::SafeTree tree;
        QString error;
        QVERIFY(tree.open(folder.filePath("dl"), &error));
        QVERIFY(tree.ensureDir("a/b/c", &error)); // "", a, a/b, a/b/c
        QCOMPARE(tree.diskLookupsForTests(), 4);
        for (int i = 0; i < 100; ++i) {
            QVERIFY(tree.ensureDir("a/b/c", &error));
            QVERIFY(tree.ensureDir("a/b", &error));
            QVERIFY(tree.ensureDir(QString(), &error));
        }
        QCOMPARE(tree.diskLookupsForTests(), 4);
        QVERIFY(tree.ensureDir("a/b/d", &error));
        QCOMPARE(tree.diskLookupsForTests(), 5);
        // Another spelling is not folded into the first (a folder may be case sensitive on any
        // system): looked at on the disk, and fine where case is ignored (the same folder).
        QVERIFY2(tree.ensureDir("A/B/C", &error), qPrintable(error));
        QCOMPARE(tree.diskLookupsForTests(), 8);
        QVERIFY(QFileInfo(folder.filePath("dl/a/b/c")).isDir());
        QVERIFY(QFileInfo(folder.filePath("dl/a/b/d")).isDir());
        QVERIFY(QFileInfo(folder.filePath("dl/A/B/C")).isDir());
    }

    // A file is replaced from empty (as CREATE_ALWAYS would), and written.
    void writeFileReplacesExisting()
    {
        QTemporaryDir folder;
        const QString path = folder.filePath("x.txt");
        writeFile(path, "a much longer old content");
        fs::WriteFile file;
        QVERIFY(file.open(path));
        QVERIFY(file.write("new", 3));
        QVERIFY(file.close());
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("new"));
        f.close();
        QVERIFY(file.open(folder.filePath("fresh.txt")));
        QVERIFY(file.write("y", 1));
        QVERIFY(file.close());
        QCOMPARE(QFileInfo(folder.filePath("fresh.txt")).size(), 1);
    }

    // A link at the file's own name is refused, and neither the link nor its target is touched.
    void writeFileRefusesLinkAtItsName()
    {
#ifndef Q_OS_WIN
        QSKIP("Only checked on Windows, where the service runs as LocalSystem");
#else
        QTemporaryDir folder, outside;
        const QString target = outside.filePath("precious.txt");
        writeFile(target, "keep me");
        const QString link = folder.filePath("link.txt");
        // Held for the call: utf16() of a temporary would point into freed memory.
        const QString nativeLink = QDir::toNativeSeparators(link);
        const QString nativeTarget = QDir::toNativeSeparators(target);
        if (!CreateSymbolicLinkW(reinterpret_cast<const wchar_t *>(nativeLink.utf16()),
                                 reinterpret_cast<const wchar_t *>(nativeTarget.utf16()),
                                 SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
            QSKIP("Symbolic links cannot be made here (no developer mode or privilege)");
        fs::WriteFile file;
        QVERIFY(!file.open(link));
        QVERIFY(!file.isOpen());
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("keep me"));
        f.close();
        QVERIFY(QFileInfo(link).isSymLink());
#endif
    }

    // Paths beyond MAX_PATH are made and held.
    void safeTreeLongPaths()
    {
        QTemporaryDir folder;
        const QString root = folder.filePath("dl");
        fs::SafeTree tree;
        QString error;
        QVERIFY(tree.open(root, &error));
        QString rel;
        for (int i = 0; i < 12; ++i)
            rel += (rel.isEmpty() ? QString() : QStringLiteral("/")) + QString(30, QLatin1Char('a' + i));
        QVERIFY(root.size() + rel.size() > 300);
        QVERIFY2(tree.ensureDir(rel, &error), qPrintable(error));
        QVERIFY(QFileInfo(root + "/" + rel).isDir());
        tree.close();
        QVERIFY(fs::removeTree(root));
    }

    // A shortcut (.lnk) is an ordinary file, though Qt 6 reports it as isSymLink(): it neither
    // counts as a link on a path nor is followed when deleted (the shortcut goes, not its target).
    void shortcutsAreNotLinks()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows shortcuts");
#else
        QTemporaryDir folder, outside;
        writeFile(outside.filePath("target.txt"), "keep me");
        QVERIFY(test::makeShortcut(folder.filePath("to-folder.lnk"), outside.path()));
        QVERIFY(test::makeShortcut(folder.filePath("to-file.lnk"), outside.filePath("target.txt")));
        QVERIFY(!fs::passesThroughLink(folder.filePath("to-folder.lnk")));
        QVERIFY(!fs::passesThroughLink(folder.filePath("to-file.lnk")));
        // ...while a junction next to it still is one.
        QVERIFY(makeJunction(folder.filePath("junction"), outside.path()));
        QVERIFY(fs::passesThroughLink(folder.filePath("junction/target.txt")));

        QVERIFY(fs::removeTree(folder.filePath("to-folder.lnk")));
        QVERIFY(!QFileInfo::exists(folder.filePath("to-folder.lnk")));
        QVERIFY(QFile::exists(outside.filePath("target.txt")));
#endif
    }
};

QTEST_GUILESS_MAIN(TestFsUtil)
#include "tst_fsutil.moc"
