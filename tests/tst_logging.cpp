// Log files are off unless the "logging" setting asks for them, and never get debug messages.

#include "logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QTest>

Q_LOGGING_CATEGORY(lcTest, "deskflower.test")
Q_LOGGING_CATEGORY(lcDragTest, "deskflower.drag.test")

namespace {
QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

class TestLogging : public QObject
{
    Q_OBJECT
private slots:
    void offByDefaultAndOnlyInformative()
    {
        QTemporaryDir dir;
        const QString log = dir.filePath(QStringLiteral("service.log"));
        // Files of an earlier version, written before logging was off by default.
        for (const QString &name : {log, log + QStringLiteral(".1")}) {
            QFile f(name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("old\n");
        }

        df::installFileLogging(dir.path(), QStringLiteral("service.log"), false, false);
        qCInfo(lcTest) << "not written anywhere";
        QVERIFY(!QFile::exists(log));
        QVERIFY(!QFile::exists(log + QStringLiteral(".1")));

        df::installFileLogging(dir.path(), QStringLiteral("service.log"), false, true);
        qCDebug(lcTest) << "a debug message";
        qCInfo(lcTest) << "an info message";
        qCWarning(lcTest) << "a warning";
        const QByteArray text = readFile(log);
        QVERIFY(!text.contains("not written anywhere"));
        QVERIFY(!text.contains("a debug message"));
        QVERIFY(text.contains("I deskflower.test: an info message"));
        QVERIFY(text.contains("W deskflower.test: a warning"));
        qInstallMessageHandler(nullptr);
    }

    // The drag diagnostics are kept whatever the setting, for their categories only, small.
    void diagnosticLogIsAlwaysOnAndSmall()
    {
        QTemporaryDir dir;
        const QString log = dir.filePath(QStringLiteral("drag.log"));
        df::installFileLogging(dir.path(), QStringLiteral("service.log"), false, false); // logging off
        df::installDiagnosticLog(dir.path(), QStringLiteral("drag.log"), {QStringLiteral("deskflower.drag")});
        qCInfo(lcDragTest) << "a milestone";
        qCDebug(lcDragTest) << "a detail";
        qCInfo(lcTest) << "something else";
        QByteArray text = readFile(log);
        QVERIFY(text.contains("I deskflower.drag.test: a milestone"));
        QVERIFY(!text.contains("a detail"));
        QVERIFY(!text.contains("something else"));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("service.log"))));

        // Rotated past its size: one older file, never more.
        const QString filler(200, QLatin1Char('x'));
        for (qint64 written = 0; written < 3 * df::kDiagnosticLogSize; written += filler.size())
            qCInfo(lcDragTest).noquote() << filler;
        QVERIFY(QFileInfo(log).size() <= df::kDiagnosticLogSize + 1024);
        QVERIFY(QFile::exists(log + QStringLiteral(".1")));
        QVERIFY(!QFile::exists(log + QStringLiteral(".2")));
        qInstallMessageHandler(nullptr);
    }
};

QTEST_GUILESS_MAIN(TestLogging)
#include "tst_logging.moc"
