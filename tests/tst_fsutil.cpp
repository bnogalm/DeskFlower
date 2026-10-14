// Links planted in a user's folder must not redirect what the service writes or deletes there.

#include "fsutil.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(content);
}

// A directory junction (Windows; no rights needed), as a user could plant one.
bool makeJunction(const QString &link, const QString &target)
{
#ifdef Q_OS_WIN
    return QProcess::execute(QStringLiteral("cmd.exe"),
                             {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                              QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)})
           == 0;
#else
    return QFile::link(target, link);
#endif
}
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
};

QTEST_GUILESS_MAIN(TestFsUtil)
#include "tst_fsutil.moc"
