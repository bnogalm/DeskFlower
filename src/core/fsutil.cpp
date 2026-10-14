#include "fsutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace df::fs {

namespace {
bool isLink(const QFileInfo &info) { return info.isSymLink() || info.isJunction(); }
} // namespace

bool passesThroughLink(const QString &path)
{
#ifndef Q_OS_WIN
    // Elsewhere the service runs as the user, and system folders are links (macOS: /var, /tmp).
    Q_UNUSED(path);
    return false;
#else
    QString current = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (;;) {
        if (isLink(QFileInfo(current)))
            return true;
        const QString parent = QFileInfo(current).path(); // "C:/" and "/" are their own parent
        if (parent.isEmpty() || parent == QLatin1String(".") || parent == current)
            return false;
        current = parent;
    }
#endif
}

bool isInside(const QString &path, const QString &folder)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QString f = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (f.isEmpty() || p.isEmpty())
        return false;
    if (p.compare(f, cs) == 0)
        return true;
    if (!f.endsWith(QLatin1Char('/')))
        f += QLatin1Char('/');
    return p.startsWith(f, cs);
}

bool removeTree(const QString &path)
{
    const QFileInfo info(path);
    if (isLink(info)) // the link itself (a junction is removed like an empty directory)
        return QFile::remove(path) || QDir().rmdir(path);
    if (!info.isDir()) {
        if (!info.exists())
            return true;
        if (QFile::remove(path))
            return true;
        QFile::setPermissions(path, QFile::permissions(path) | QFile::WriteOwner | QFile::WriteUser);
        return QFile::remove(path);
    }
    bool ok = true;
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries)
        ok = removeTree(entry.filePath()) && ok;
    return QDir().rmdir(path) && ok;
}

} // namespace df::fs
