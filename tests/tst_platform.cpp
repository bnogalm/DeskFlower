#include "platform.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace df;

namespace {
#ifdef Q_OS_WIN
// Whether the calling thread acts as someone (has an impersonation token).
bool impersonating()
{
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
        return false;
    CloseHandle(token);
    return true;
}

// Stands in for WTSQueryUserToken: the "console user" is whoever runs the test, and, like the real
// call, it fails while the thread acts as the user (only the service's own rights may ask).
struct FakeConsole {
    bool signedIn = true;
    int refusals = 0;

    void *token()
    {
        if (!signedIn)
            return nullptr;
        if (impersonating()) {
            ++refusals;
            SetLastError(ERROR_PRIVILEGE_NOT_HELD);
            return nullptr;
        }
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &token))
            return nullptr;
        return token;
    }
};

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}
#endif

// A scratch folder that may be shared: an ordinary folder in the home folder (the temporary
// folder is not one: on Windows it is in AppData). Removed afterwards.
QString inHome() { return QDir::homePath() + QStringLiteral("/deskflower-test-XXXXXX"); }
} // namespace

class TestPlatform : public QObject
{
    Q_OBJECT

#ifdef Q_OS_WIN
    FakeConsole m_console;
#endif

private slots:
#ifdef Q_OS_WIN
    void init()
    {
        m_console = {};
        platform::setUserTokenSourceForTests([this] { return m_console.token(); });
    }
    void cleanup()
    {
        platform::setUserTokenSourceForTests({});
        const bool leftover = impersonating();
        RevertToSelf();
        QVERIFY2(!leftover, "a test left the thread acting as someone");
    }

    void userLookupsWorkWhileActingAsTheUser()
    {
        const QString home = platform::interactiveUserHome(true);
        const QString id = platform::interactiveUserId(true);
        QVERIFY(samePath(home, QDir::homePath()));
        QVERIFY(id.startsWith(QLatin1String("S-1-")));

        // What the phone server does: act as the user, then find the shared folder.
        auto asUser = platform::actAsInteractiveUser(true);
        QVERIFY(asUser);
        QVERIFY(impersonating());
        QCOMPARE(platform::interactiveUserHome(true), home);
        QCOMPARE(platform::interactiveUserId(true), id);
        QVERIFY2(impersonating(), "a lookup stopped acting as the user");
        QCOMPARE(m_console.refusals, 0);

        asUser.reset();
        QVERIFY(!impersonating());
    }

    void nestedActingPutsBackTheOuterOne()
    {
        auto outer = platform::actAsInteractiveUser(true);
        QVERIFY(outer);
        {
            auto inner = platform::actAsInteractiveUser(true);
            QVERIFY(inner);
            QVERIFY(impersonating());
        }
        QVERIFY2(impersonating(), "the inner guard ended the outer one");
        outer.reset();
        QVERIFY(!impersonating());

        // Also when the thread already acted as someone else before.
        QVERIFY(ImpersonateSelf(SecurityImpersonation));
        platform::actAsInteractiveUser(true).reset();
        QVERIFY(impersonating());
        RevertToSelf();
    }

    void nobodySignedIn()
    {
        m_console.signedIn = false;
        QVERIFY(!platform::actAsInteractiveUser(true));
        QVERIFY(platform::interactiveUserHome(true).isEmpty());
        QVERIFY(platform::interactiveUserId(true).isEmpty());

        // A failed attempt leaves the thread as it was.
        QVERIFY(ImpersonateSelf(SecurityImpersonation));
        QVERIFY(!platform::actAsInteractiveUser(true));
        QVERIFY(impersonating());
        RevertToSelf();

        QTemporaryDir dir(inHome()); // an ordinary folder, but nobody is signed in
        const QString fallback = dir.filePath(QStringLiteral("Fallback"));
        QCOMPARE(platform::sharedFolder(dir.filePath(QStringLiteral("Shared")), fallback, true), fallback);
    }

    // The bug of 0.1.15/0.1.16: as the user, the folder lookup failed and fell back to a folder
    // in ProgramData that the user cannot reach ("the folder is unavailable" on the phone).
    void sharedFolderWhileActingAsTheUser()
    {
        QTemporaryDir dir(inHome());
        QVERIFY(dir.isValid());
        const QString chosen = dir.filePath(QStringLiteral("Shared"));
        const QString fallback = dir.filePath(QStringLiteral("Fallback"));

        auto asUser = platform::actAsInteractiveUser(true);
        QVERIFY(asUser);
        QCOMPARE(platform::sharedFolder(chosen, fallback, true), chosen);
        QVERIFY(QFileInfo(chosen).isDir());
        QVERIFY(impersonating());
        QCOMPARE(m_console.refusals, 0);
        asUser.reset();

        QString why;
        QVERIFY(!platform::isAllowedSharedFolder(QStringLiteral("C:/Windows/Temp/Deskflower"), true, &why));
        QVERIFY(why.contains(QLatin1String("inside your user folder")));
        QVERIFY(!platform::isAllowedSharedFolder(QDir::homePath() + QStringLiteral("/AppData/Roaming/x"), true, &why));
        QVERIFY(why.contains(QLatin1String("AppData")));

        // A folder that is not allowed (chosen by hand in the settings file, or allowed by an
        // older version) is never used.
        m_console.signedIn = false;
        QCOMPARE(platform::sharedFolder(QStringLiteral("C:/Windows/System32"), fallback, true), fallback);
    }
#endif

    // Copied and dragged files wait where phones never see them: not in the shared folder, and
    // not a folder that could ever be chosen as the shared one.
    void receivedFilesAreApartFromTheSharedFolder()
    {
#ifdef Q_OS_WIN
        const bool serviceMode = true; // through the fake console user
#else
        const bool serviceMode = false;
#endif
        const QString received = platform::receivedFolder(QStringLiteral("/fallback"), serviceMode);
        QVERIFY(QFileInfo(received).isDir());
        const QString shared = QDir::homePath() + QStringLiteral("/Deskflower");
        QVERIFY(!received.startsWith(shared, Qt::CaseInsensitive));
        QVERIFY2(!platform::sharedFolderProblem(received, QDir::homePath(), true).isEmpty(), qPrintable(received));
#ifdef Q_OS_WIN
        QVERIFY(samePath(received, QDir::homePath() + QStringLiteral("/AppData/Local/Deskflower/Received")));
        m_console.signedIn = false;
        QCOMPARE(platform::receivedFolder(QStringLiteral("C:/fallback"), true), QStringLiteral("C:/fallback"));
#endif
    }

    void sharedFolderRules_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("insideHomeOnly");
        QTest::addColumn<QString>("problem"); // part of the reason; empty: allowed
#ifdef Q_OS_WIN
        const QString home = QStringLiteral("C:/Users/Ana");
        QTest::newRow("default") << "C:/Users/Ana/Deskflower" << true << "";
        QTest::newRow("documents") << "C:/Users/Ana/Documents/Phone" << true << "";
        QTest::newRow("case") << "c:/users/ana/Desktop" << true << "";
        QTest::newRow("native") << "C:\\Users\\Ana\\Pictures" << true << "";
        QTest::newRow("home") << "C:/Users/Ana" << true << "whole user folder";
        QTest::newRow("home/") << "C:/Users/Ana/" << true << "whole user folder";
        QTest::newRow("appdata") << "C:/Users/Ana/AppData" << true << "AppData";
        QTest::newRow("startup") << "C:/Users/Ana/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup"
                                 << true << "AppData";
        QTest::newRow("hidden") << "C:/Users/Ana/.ssh" << true << "hidden";
        QTest::newRow("hidden below") << "C:/Users/Ana/Documents/.git/hooks" << true << "hidden";
        QTest::newRow("escape") << "C:/Users/Ana/../Bob/Documents" << true << "inside your user folder";
        QTest::newRow("prefix") << "C:/Users/Anabel/Documents" << true << "inside your user folder";
        QTest::newRow("windows") << "C:/Windows/System32" << true << "inside your user folder";
        QTest::newRow("other disk") << "D:/Shared" << true << "inside your user folder";
        // The service runs as the user (console mode): other disks are fine, system folders not.
        QTest::newRow("as user: other disk") << "D:/Shared" << false << "";
        QTest::newRow("as user: drive") << "D:/" << false << "whole drive";
        QTest::newRow("as user: windows") << "C:/Windows/Temp" << false << "system folder";
        QTest::newRow("as user: programs") << "C:/Program Files/App" << false << "system folder";
        QTest::newRow("as user: programdata") << "C:/ProgramData/Deskflower" << false << "system folder";
        QTest::newRow("as user: appdata") << "C:/Users/Ana/AppData/Local" << false << "AppData";
#else
        const QString home = QStringLiteral("/home/ana");
        QTest::newRow("default") << "/home/ana/Deskflower" << true << "";
        QTest::newRow("documents") << "/home/ana/Documents/Phone" << true << "";
        QTest::newRow("home") << "/home/ana" << true << "whole user folder";
        QTest::newRow("hidden") << "/home/ana/.config/autostart" << true << "hidden";
        QTest::newRow("escape") << "/home/ana/../bob" << true << "inside your user folder";
        QTest::newRow("prefix") << "/home/anabel" << true << "inside your user folder";
        QTest::newRow("as user: disk") << "/media/ana/USB/Shared" << false << "";
        QTest::newRow("as user: root") << "/" << false << "whole drive";
        QTest::newRow("as user: etc") << "/etc/cron.d" << false << "system folder";
        QTest::newRow("as user: usr") << "/usr/local/bin" << false << "system folder";
        QTest::newRow("as user: var") << "/var/www" << false << "system folder";
#endif
        QTest::newRow("relative") << "Documents" << false << "absolute";
        Q_UNUSED(home);
    }

    void sharedFolderRules()
    {
        QFETCH(QString, path);
        QFETCH(bool, insideHomeOnly);
        QFETCH(QString, problem);
#ifdef Q_OS_WIN
        const QString home = QStringLiteral("C:/Users/Ana");
#else
        const QString home = QStringLiteral("/home/ana");
#endif
        const QString found = platform::sharedFolderProblem(path, home, insideHomeOnly);
        if (problem.isEmpty())
            QVERIFY2(found.isEmpty(), qPrintable(found));
        else
            QVERIFY2(found.contains(problem), qPrintable(found));
    }

    void sharedFolderAsTheUser()
    {
        // Not a Windows service: the process already runs as the user; any absolute folder works.
        QTemporaryDir dir(inHome());
        const QString chosen = dir.filePath(QStringLiteral("a/b"));
        QCOMPARE(platform::sharedFolder(chosen, dir.filePath(QStringLiteral("Fallback")), false), chosen);
        QVERIFY(QFileInfo(chosen).isDir());
        QVERIFY(platform::actAsInteractiveUser(false));
        QVERIFY(!platform::isAllowedSharedFolder(QStringLiteral("relative/path"), false));
        QVERIFY(!platform::isAllowedSharedFolder(QDir::homePath(), false));
        QVERIFY(!platform::isAllowedSharedFolder(QDir::rootPath(), false));
    }
};

QTEST_GUILESS_MAIN(TestPlatform)
#include "tst_platform.moc"
