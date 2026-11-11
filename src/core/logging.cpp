#include "logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

#include <algorithm>
#include <cstdio>

namespace df {

namespace {
constexpr qint64 kMaxLogSize = 5 * 1024 * 1024;

QMutex g_mutex;
QFile *g_file = nullptr;
bool g_stderr = false;
// installDiagnosticLog: a small file of its own, for some categories only.
QFile *g_diagFile = nullptr;
QStringList g_diagPrefixes;
QtMessageHandler g_previous = nullptr; // the handler before ours (the agent keeps Qt's default)

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

void rotateIfNeeded(QFile *file, qint64 maxSize)
{
    if (file->size() < maxSize)
        return;
    const QString path = file->fileName();
    file->close();
    QFile::remove(path + QStringLiteral(".1"));
    QFile::rename(path, path + QStringLiteral(".1"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        std::fprintf(stderr, "Cannot open log file %s\n", qPrintable(file->fileName()));
}

bool isDiagnostic(const char *category)
{
    if (!g_diagFile || !g_diagFile->isOpen() || !category)
        return false;
    const QLatin1String name(category);
    return std::any_of(g_diagPrefixes.cbegin(), g_diagPrefixes.cend(),
                       [&](const QString &prefix) { return name.startsWith(prefix); });
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
        rotateIfNeeded(g_file, kMaxLogSize);
    }
    if (type != QtDebugMsg && isDiagnostic(context.category)) {
        g_diagFile->write(line);
        g_diagFile->flush();
        rotateIfNeeded(g_diagFile, kDiagnosticLogSize);
    }
    if (g_stderr) {
        std::fputs(line.constData(), stderr);
        std::fflush(stderr);
    }
    const QtMessageHandler previous = g_previous;
    lock.unlock();
    if (previous)
        previous(type, context, message);
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
    g_previous = nullptr; // this replaces Qt's default output (stderr when asked, above)
}

void installDiagnosticLog(const QString &dir, const QString &fileName, const QStringList &categoryPrefixes)
{
    {
        QMutexLocker lock(&g_mutex);
        QDir().mkpath(dir);
        delete g_diagFile;
        g_diagFile = new QFile(QDir(dir).filePath(fileName));
        if (!g_diagFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            std::fprintf(stderr, "Cannot open log file %s\n", qPrintable(g_diagFile->fileName()));
        g_diagPrefixes = categoryPrefixes;
    }
    const QtMessageHandler previous = qInstallMessageHandler(handler);
    if (previous != handler) {
        QMutexLocker lock(&g_mutex);
        g_previous = previous;
    }
}

} // namespace df
