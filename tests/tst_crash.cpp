// Crash diagnostics (crashreport.h): whatever the logging setting, a process that ends badly
// leaves a line in problems.log (and on Windows a minidump), and one that ends normally says so.
// The crashes happen in a helper process (crashprobe).

#include "crashreport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {
QString probePath()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("crashprobe.exe"));
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("crashprobe"));
#endif
}

// Runs the probe; returns its exit code (-1: it crashed, as QProcess reports it on Unix; -2: it
// was still running, as one whose crash was swallowed goes on).
int runProbe(const QString &dir, const QString &mode)
{
    QProcess probe;
    probe.start(probePath(), {dir, mode});
    if (!probe.waitForFinished(20000)) {
        probe.kill();
        probe.waitForFinished();
        return -2;
    }
    return probe.exitStatus() == QProcess::CrashExit ? -1 : probe.exitCode();
}

QString problems(const QString &dir)
{
    QFile f(df::crash::problemsLogPath(dir));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QStringList dumps(const QString &dir) { return QDir(dir).entryList({QStringLiteral("probe-*.dmp")}, QDir::Files); }
} // namespace

class TestCrash : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY2(QFile::exists(probePath()), qPrintable(probePath())); }

    void aCleanRunSaysSo()
    {
        QTemporaryDir dir;
        QCOMPARE(runProbe(dir.path(), QStringLiteral("exit")), 0);
        const QString log = problems(dir.path());
        QVERIFY2(log.contains(QStringLiteral("start probe 9.9.9")), qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("exit 0")), qPrintable(log));
        QVERIFY(!log.contains(QStringLiteral("CRASH")));
        QVERIFY(dumps(dir.path()).isEmpty());
    }

    void fatalMessagesAreRecorded()
    {
        QTemporaryDir dir;
        QVERIFY(runProbe(dir.path(), QStringLiteral("fatal")) != 0);
        const QString log = problems(dir.path());
        QVERIFY2(log.contains(QStringLiteral("FATAL: probe fatal message")), qPrintable(log));
        QVERIFY(!log.contains(QStringLiteral("exit 0")));
#ifdef Q_OS_WIN
        QCOMPARE(dumps(dir.path()).size(), 1);
#endif
    }

    void terminateIsRecordedWithTheException()
    {
        QTemporaryDir dir;
        QVERIFY(runProbe(dir.path(), QStringLiteral("terminate")) != 0);
        const QString log = problems(dir.path());
        QVERIFY2(log.contains(QStringLiteral("CRASH: std::terminate: probe exception")), qPrintable(log));
        QCOMPARE(log.count(QStringLiteral("CRASH")), 1); // the abort that follows is the same crash
#ifdef Q_OS_WIN
        QCOMPARE(dumps(dir.path()).size(), 1);
#endif
    }

    void aCrashIsRecordedWithADump()
    {
        QTemporaryDir dir;
        QVERIFY(runProbe(dir.path(), QStringLiteral("segv")) != 0);
        const QString log = problems(dir.path());
#ifdef Q_OS_WIN
        QVERIFY2(log.contains(QStringLiteral("CRASH: unhandled exception 0xC0000005 at crashprobe.exe+0x")),
                 qPrintable(log));
        QCOMPARE(dumps(dir.path()).size(), 1);
        QVERIFY(QFileInfo(QDir(dir.path()).filePath(dumps(dir.path()).first())).size() > 0);
#else
        QVERIFY2(log.contains(QStringLiteral("CRASH: signal")), qPrintable(log));
#endif
    }

    // The service runs almost everything from its event loop: a crash in a slot run by a timer
    // must be recorded as well, not swallowed (Windows: WM_TIMER) with the process going on.
    void aCrashInTheEventLoopIsRecorded()
    {
        QTemporaryDir dir;
        const int rc = runProbe(dir.path(), QStringLiteral("segv-in-loop"));
        QVERIFY(rc != 0 && rc != -2);
        const QString log = problems(dir.path());
        QVERIFY(!log.contains(QStringLiteral("exit 0")));
#ifdef Q_OS_WIN
        QVERIFY2(log.contains(QStringLiteral("CRASH: unhandled exception 0xC0000005 at crashprobe.exe+0x")),
                 qPrintable(log));
        QCOMPARE(dumps(dir.path()).size(), 1);
#else
        QVERIFY2(log.contains(QStringLiteral("CRASH: signal")), qPrintable(log));
#endif
    }

#ifdef Q_OS_WIN
    // Windows swallows exceptions raised in a timer callback dispatched by the message loop
    // (UOI_TIMERPROC_EXCEPTION_SUPPRESSION): turned off at install, so that the crash is recorded
    // and ends the process instead of leaving it running with corrupt state.
    void aCrashInATimerCallbackIsNotSwallowed()
    {
        QTemporaryDir dir;
        QCOMPARE_NE(runProbe(dir.path(), QStringLiteral("segv-in-timerproc")), -2); // not swallowed
        const QString log = problems(dir.path());
        QVERIFY2(log.contains(QStringLiteral("CRASH: unhandled exception 0xC0000005 at crashprobe.exe+0x")),
                 qPrintable(log));
        QCOMPARE(dumps(dir.path()).size(), 1);
    }
#endif

    // A stack overflow leaves little stack to the handler: it is recorded all the same.
    void aStackOverflowIsRecorded()
    {
        QTemporaryDir dir;
        QVERIFY(runProbe(dir.path(), QStringLiteral("stackoverflow")) != 0);
        const QString log = problems(dir.path());
#ifdef Q_OS_WIN
        QVERIFY2(log.contains(QStringLiteral("CRASH: unhandled exception 0xC00000FD at crashprobe.exe+0x")),
                 qPrintable(log));
        QCOMPARE(dumps(dir.path()).size(), 1);
#else
        QVERIFY2(log.contains(QStringLiteral("CRASH: signal")), qPrintable(log));
#endif
    }

    void theRecordStaysSmall()
    {
        // A long problems.log moves aside at the next start; only the newest dumps are kept.
        QTemporaryDir dir;
        {
            QFile big(df::crash::problemsLogPath(dir.path()));
            QVERIFY(big.open(QIODevice::WriteOnly));
            big.write(QByteArray(int(df::crash::kMaxLogBytes) + 1, 'x'));
        }
        for (int i = 0; i < 5; ++i) {
            QFile old(QDir(dir.path()).filePath(QStringLiteral("probe-2020010%1-000000.dmp").arg(i)));
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("old");
            QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-10 + i), QFileDevice::FileModificationTime));
        }
        QVERIFY(runProbe(dir.path(), QStringLiteral("fatal")) != 0);
        QVERIFY(QFile::exists(df::crash::problemsLogPath(dir.path()) + QStringLiteral(".1")));
        QVERIFY(QFileInfo(df::crash::problemsLogPath(dir.path())).size() < 4096);
#ifdef Q_OS_WIN
        const QStringList kept = dumps(dir.path());
        QCOMPARE(kept.size(), df::crash::kMaxDumps);
        QVERIFY(kept.contains(QStringLiteral("probe-20200104-000000.dmp"))); // the newest old ones
        QVERIFY(kept.contains(QStringLiteral("probe-20200103-000000.dmp")));
#endif
    }
};

QTEST_GUILESS_MAIN(TestCrash)
#include "tst_crash.moc"
