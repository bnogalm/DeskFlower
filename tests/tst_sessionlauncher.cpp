// SessionLauncher with this test program as the session helper (started with the helper's token
// variable, it only records what it got and waits, or exits at once): a helper is restarted only
// when the graphical session the agent reports differs from what it really got, and one that
// keeps exiting is restarted less and less often. Linux (where agents report their session) and
// macOS (the backoff only); a console process cannot be asked to end on Windows.

#include "sessionlauncher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <thread>

using namespace df;

namespace {

const char *const kLogVariable = "DESKFLOWER_TEST_HELPER_LOG";
const char *const kExitVariable = "DESKFLOWER_TEST_HELPER_EXIT";

// The fake helper: one line per start, "<ms since epoch> <XDG_SESSION_TYPE>".
int runHelper()
{
    QFile log(qEnvironmentVariable(kLogVariable));
    if (log.open(QIODevice::Append)) {
        log.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + ' ' + qgetenv("XDG_SESSION_TYPE") + '\n');
        log.close();
    }
    if (qEnvironmentVariable(kExitVariable) == QLatin1String("1"))
        return 1;
    for (;;) // until the launcher ends it
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

QList<QByteArrayList> starts(const QString &path)
{
    QFile log(path);
    QList<QByteArrayList> out;
    if (!log.open(QIODevice::ReadOnly))
        return out;
    for (const QByteArray &line : log.readAll().split('\n')) {
        if (!line.isEmpty())
            out.append(line.split(' '));
    }
    return out;
}

} // namespace

class TestSessionLauncher : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_log = m_dir.filePath(QStringLiteral("starts-%1").arg(++m_run));
        qputenv(kLogVariable, m_log.toUtf8());
        qunsetenv(kExitVariable);
        qputenv("XDG_SESSION_TYPE", "x11");
    }

    void theSameSessionKeepsTheHelper()
    {
#ifdef Q_OS_MACOS
        QSKIP("launchd gives the helper its session on macOS (setSessionEnvironment is Linux only)");
#endif
        SessionLauncher launcher(QString(), QString(), QString(), false);
        launcher.setProgram(QCoreApplication::applicationFilePath());
        launcher.start();
        QTRY_COMPARE(starts(m_log).size(), 1);
        QCOMPARE(starts(m_log).at(0).value(1), QByteArray("x11"));
        // The first agent reports what the service had already given the helper: it stays.
        launcher.setSessionEnvironment({{QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("x11")}});
        QTest::qWait(3000);
        QCOMPARE(starts(m_log).size(), 1);
        // Another session: the helper is started again with it.
        launcher.setSessionEnvironment({{QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("wayland")}});
        QTRY_COMPARE_WITH_TIMEOUT(starts(m_log).size(), 2, 10000);
        QCOMPARE(starts(m_log).at(1).value(1), QByteArray("wayland"));
    }

    void aHelperThatKeepsExitingWaitsLongerEachTime()
    {
        qputenv(kExitVariable, "1");
        SessionLauncher launcher(QString(), QString(), QString(), false);
        launcher.setProgram(QCoreApplication::applicationFilePath());
        launcher.start();
        QTRY_COMPARE_WITH_TIMEOUT(starts(m_log).size(), 3, 15000);
        const QList<QByteArrayList> s = starts(m_log);
        const qint64 first = s.at(1).value(0).toLongLong() - s.at(0).value(0).toLongLong();
        const qint64 second = s.at(2).value(0).toLongLong() - s.at(1).value(0).toLongLong();
        QVERIFY2(first >= 1500, qPrintable(QString::number(first)));
        QVERIFY2(second >= first + 1000, qPrintable(QStringLiteral("%1 then %2").arg(first).arg(second)));
    }

private:
    QTemporaryDir m_dir;
    QString m_log;
    int m_run = 0;
};

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsEmpty("DESKFLOWER_SESSION_TOKEN"))
        return runHelper();
    QCoreApplication app(argc, argv);
    TestSessionLauncher test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_sessionlauncher.moc"
