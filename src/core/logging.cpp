#include "logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

#include <cstdio>

namespace df {

namespace {
constexpr qint64 kMaxLogSize = 5 * 1024 * 1024;

QMutex g_mutex;
QFile *g_file = nullptr;
bool g_stderr = false;

char levelChar(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return 'D';
    case QtInfoMsg: return 'I';
    case QtWarningMsg: return 'W';
    case QtCriticalMsg: return 'C';
    case QtFatalMsg: return 'F';
    }
    return '?';
}

void rotateIfNeeded()
{
    if (g_file->size() < kMaxLogSize)
        return;
    const QString path = g_file->fileName();
    g_file->close();
    QFile::remove(path + QStringLiteral(".1"));
    QFile::rename(path, path + QStringLiteral(".1"));
    if (!g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        std::fprintf(stderr, "Cannot open log file %s\n", qPrintable(g_file->fileName()));
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QByteArray line = QStringLiteral("%1 %2 %3: %4\n")
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                                .arg(QLatin1Char(levelChar(type)))
                                .arg(QLatin1String(context.category ? context.category : "default"), message)
                                .toUtf8();
    QMutexLocker lock(&g_mutex);
    if (g_file && g_file->isOpen() && type != QtDebugMsg) {
        g_file->write(line);
        g_file->flush();
        rotateIfNeeded();
    }
    if (g_stderr) {
        std::fputs(line.constData(), stderr);
        std::fflush(stderr);
    }
}
} // namespace

void installFileLogging(const QString &dir, const QString &fileName, bool alsoStderr, bool toFile)
{
    const QString path = QDir(dir).filePath(fileName);
    if (toFile) {
        QDir().mkpath(dir);
        g_file = new QFile(path);
        if (!g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            std::fprintf(stderr, "Cannot open log file %s\n", qPrintable(g_file->fileName()));
    } else {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".1"));
    }
    g_stderr = alsoStderr;
    qInstallMessageHandler(handler);
}

} // namespace df
