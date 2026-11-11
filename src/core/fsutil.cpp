#include "fsutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <mutex>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace df::fs {

namespace {
// A symbolic link or a junction (mount points included), never a Windows shortcut: Qt 6 reports a
// .lnk file as isSymLink(), but it is an ordinary file that redirects nothing.
bool isLink(const QFileInfo &info) { return info.isSymbolicLink() || info.isJunction(); }
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

// --- WriteFile ---------------------------------------------------------------------------------

namespace {
#ifdef Q_OS_WIN
std::wstring longPath(const QString &absolute);
bool isLinkTag(ULONG tag);
#endif
} // namespace

struct WriteFile::Impl {
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    std::unique_ptr<QFile> file;
#endif
};

WriteFile::WriteFile() : d(std::make_unique<Impl>()) {}

WriteFile::~WriteFile() { close(); }

bool WriteFile::isOpen() const
{
#ifdef Q_OS_WIN
    return d->handle != INVALID_HANDLE_VALUE;
#else
    return d->file != nullptr;
#endif
}

bool WriteFile::open(const QString &path)
{
    close();
#ifdef Q_OS_WIN
    const std::wstring w = longPath(QDir(path).absolutePath());
    // Opened (or created) without following a reparse point at its own name, checked, and only
    // then emptied: FILE_FLAG_OPEN_REPARSE_POINT is not for CREATE_ALWAYS, which would act on the
    // link's target or replace the link before it could be checked.
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof(info))
        || ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && isLinkTag(info.ReparseTag))
        || (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        return false;
    }
    if (existed) { // replaced, as CREATE_ALWAYS would: from empty
        FILE_END_OF_FILE_INFO eof{};
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof))) {
            CloseHandle(h);
            return false;
        }
    }
    d->handle = h;
    return true;
#else
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Unbuffered))
        return false;
    d->file = std::move(file);
    return true;
#endif
}

bool WriteFile::write(const char *data, qint64 size)
{
#ifdef Q_OS_WIN
    if (d->handle == INVALID_HANDLE_VALUE)
        return false;
    while (size > 0) {
        const DWORD n = DWORD(std::min<qint64>(size, 1 << 30));
        DWORD written = 0;
        if (!::WriteFile(d->handle, data, n, &written, nullptr) || written == 0)
            return false;
        data += written;
        size -= written;
    }
    return true;
#else
    return d->file && d->file->write(data, size) == size;
#endif
}

bool WriteFile::close()
{
#ifdef Q_OS_WIN
    if (d->handle == INVALID_HANDLE_VALUE)
        return true;
    const bool ok = CloseHandle(d->handle);
    d->handle = INVALID_HANDLE_VALUE;
    return ok;
#else
    if (!d->file)
        return true;
    d->file->close();
    const bool ok = d->file->error() == QFileDevice::NoError;
    d->file.reset();
    return ok;
#endif
}

// --- SafeTree ----------------------------------------------------------------------------------

namespace {
#ifdef Q_OS_WIN
constexpr ULONG kLxSymlinkTag = 0xA000001D; // IO_REPARSE_TAG_LX_SYMLINK (WSL)

bool isLinkTag(ULONG tag)
{
    return tag == IO_REPARSE_TAG_MOUNT_POINT || tag == IO_REPARSE_TAG_SYMLINK || tag == kLxSymlinkTag;
}

std::wstring longPath(const QString &absolute)
{
    const QString native = QDir::toNativeSeparators(QDir::cleanPath(absolute));
    if (native.startsWith(QLatin1String("\\\\?\\")))
        return native.toStdWString();
    if (native.startsWith(QLatin1String("\\\\")))
        return (QLatin1String("\\\\?\\UNC\\") + native.mid(2)).toStdWString();
    return (QLatin1String("\\\\?\\") + native).toStdWString();
}

enum class Held { Ok, Missing, Link, NotDir, Failed };

// Opens the folder at `path` without following a reparse point, and keeps the handle in *out
// (pinned: no FILE_SHARE_DELETE, so it cannot be renamed or deleted, unless another handle with
// delete access is open already; then it is only checked). A folder the user may cross but not
// list is checked and not pinned.
Held openFolder(const std::wstring &path, HANDLE *out)
{
    constexpr DWORD kFlags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    constexpr DWORD kList = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    HANDLE h = CreateFileW(path.c_str(), kList, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, kFlags,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
        h = CreateFileW(path.c_str(), kList, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, kFlags, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, kFlags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? Held::Missing : Held::Failed;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof(info))) {
        CloseHandle(h);
        return Held::Failed;
    }
    // Links redirect; other reparse points (cloud placeholders, deduplicated files) do not, as
    // passesThroughLink sees it.
    if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && isLinkTag(info.ReparseTag)) {
        CloseHandle(h);
        return Held::Link;
    }
    if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        return Held::NotDir;
    }
    *out = h;
    return Held::Ok;
}
#endif
} // namespace

struct SafeTree::Impl {
    mutable std::mutex mutex;
    QString root; // clean, absolute, '/' separated
    bool opened = false;
    // Folders made sure of, relative ("" is the root), by their exact name (never folded: a folder
    // may be case sensitive, on any system; two spellings of one folder are just held twice);
    // Windows: their handles.
    QHash<QString, quintptr> dirs;
    int lookups = 0;
#ifdef Q_OS_WIN
    std::vector<HANDLE> above; // the root's existing ancestors (the drive or share itself left out)
#endif

    bool ensure(const QString &rel, QString *error);
};

SafeTree::SafeTree() : d(std::make_unique<Impl>()) {}

SafeTree::~SafeTree() { close(); }

bool SafeTree::open(const QString &root, QString *error)
{
    close();
    std::lock_guard<std::mutex> lock(d->mutex);
    d->root = QDir::cleanPath(QDir::fromNativeSeparators(QDir(root).absolutePath()));
    d->opened = true;
#ifdef Q_OS_WIN
    // Every existing folder from the drive (or share) down to the root, the root included when it
    // exists: checked and held. The first missing one ends the walk: ensureDir makes the rest.
    QStringList parts = d->root.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString path;
    int first = 1; // "C:"
    if (d->root.startsWith(QLatin1String("//"))) {
        path = QStringLiteral("//") + parts.value(0) + QLatin1Char('/') + parts.value(1); // //server/share
        first = 2;
    } else {
        path = parts.value(0);
    }
    for (int i = first; i < parts.size(); ++i) {
        path += QLatin1Char('/') + parts.at(i);
        HANDLE h = nullptr;
        const Held held = openFolder(longPath(path), &h);
        if (held == Held::Missing)
            break;
        if (held != Held::Ok) {
            *error = held == Held::Link || held == Held::NotDir
                         ? QStringLiteral("The destination folder goes through a link")
                         : QStringLiteral("The destination folder could not be opened");
            for (HANDLE a : d->above)
                CloseHandle(a);
            d->above.clear();
            d->opened = false;
            return false;
        }
        if (i == parts.size() - 1)
            d->dirs.insert(QString(), quintptr(h));
        else
            d->above.push_back(h);
    }
#else
    Q_UNUSED(error);
#endif
    return true;
}

bool SafeTree::ensureDir(const QString &rel, QString *error)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (!d->opened) {
        *error = QStringLiteral("A received folder could not be created");
        return false;
    }
    return d->ensure(rel, error);
}

bool SafeTree::Impl::ensure(const QString &rel, QString *error)
{
    if (dirs.contains(rel))
        return true;
    ++lookups;
    if (!rel.isEmpty()) { // parents first: each one held before its child is made in it
        const qsizetype slash = rel.lastIndexOf(QLatin1Char('/'));
        if (!ensure(slash < 0 ? QString() : rel.left(slash), error))
            return false;
    }
    const QString path = rel.isEmpty() ? root : root + QLatin1Char('/') + rel;
#ifdef Q_OS_WIN
    if (rel.isEmpty()) {
        // The root's missing tail: made folder by folder, each one checked and held.
        QString existing = root;
        QStringList missing;
        HANDLE probe = nullptr;
        // The drive ("D:") or share ("//server/share") itself is not opened (as in open()): "\\?\D:"
        // is the volume, not its root folder. The walk ends there, and the rest is made in it.
        const QString volume = root.startsWith(QLatin1String("//"))
                                   ? root.left(root.indexOf(QLatin1Char('/'), root.indexOf(QLatin1Char('/'), 2) + 1))
                                   : root.left(root.indexOf(QLatin1Char('/')));
        for (;;) {
            if (existing.compare(volume, Qt::CaseInsensitive) == 0)
                break;
            const Held held = openFolder(longPath(existing), &probe);
            if (held == Held::Ok) {
                CloseHandle(probe);
                break;
            }
            if (held != Held::Missing) {
                *error = QStringLiteral("The destination goes through a link");
                return false;
            }
            const qsizetype slash = existing.lastIndexOf(QLatin1Char('/'));
            if (slash <= 0) {
                *error = QStringLiteral("A received folder could not be created");
                return false;
            }
            missing.prepend(existing.mid(slash + 1));
            existing = existing.left(slash);
        }
        for (const QString &part : std::as_const(missing)) {
            existing += QLatin1Char('/') + part;
            const std::wstring w = longPath(existing);
            if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                *error = QStringLiteral("A received folder could not be created");
                return false;
            }
            HANDLE h = nullptr;
            const Held held = openFolder(w, &h);
            if (held != Held::Ok) {
                *error = held == Held::Link || held == Held::NotDir
                             ? QStringLiteral("The destination goes through a link")
                             : QStringLiteral("A received folder could not be created");
                return false;
            }
            if (existing == root)
                dirs.insert(QString(), quintptr(h));
            else
                above.push_back(h);
        }
        if (!dirs.contains(QString())) { // it existed after all (made meanwhile)
            HANDLE h = nullptr;
            if (openFolder(longPath(root), &h) != Held::Ok) {
                *error = QStringLiteral("The destination goes through a link");
                return false;
            }
            dirs.insert(QString(), quintptr(h));
        }
        return true;
    }
    const std::wstring w = longPath(path);
    if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        *error = QStringLiteral("A received folder could not be created");
        return false;
    }
    HANDLE h = nullptr;
    const Held held = openFolder(w, &h);
    if (held != Held::Ok) {
        *error = held == Held::Link || held == Held::NotDir ? QStringLiteral("The destination goes through a link")
                                                            : QStringLiteral("A received folder could not be created");
        return false;
    }
    dirs.insert(rel, quintptr(h));
    return true;
#else
    if (!QDir().mkpath(path) || !QFileInfo(path).isDir()) {
        *error = QStringLiteral("A received folder could not be created");
        return false;
    }
    dirs.insert(rel, 0);
    return true;
#endif
}

void SafeTree::close()
{
    std::lock_guard<std::mutex> lock(d->mutex);
#ifdef Q_OS_WIN
    for (const quintptr h : std::as_const(d->dirs))
        CloseHandle(HANDLE(h));
    for (HANDLE h : d->above)
        CloseHandle(h);
    d->above.clear();
#endif
    d->dirs.clear();
    d->opened = false;
}

QString SafeTree::root() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->root;
}

int SafeTree::heldForTests() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
#ifdef Q_OS_WIN
    return int(d->dirs.size() + d->above.size());
#else
    return 0;
#endif
}

int SafeTree::diskLookupsForTests() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->lookups;
}

bool removeTree(const QString &path, const std::function<bool()> &stop)
{
    if (stop && stop())
        return false;
    const QFileInfo info(path);
    if (isLink(info)) // the link itself (a junction is removed like an empty directory)
        return QFile::remove(path) || QDir().rmdir(path);
    if (info.isShortcut() || !info.isDir()) { // a shortcut is a file, whatever Qt says it points to
        if (QFile::remove(path))
            return true;
        if (!info.exists() && !info.isShortcut()) // (Qt answers for a shortcut's target)
            return true;
        QFile::setPermissions(path, QFile::permissions(path) | QFile::WriteOwner | QFile::WriteUser);
        return QFile::remove(path);
    }
    bool ok = true;
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (stop && stop())
            return false;
        ok = removeTree(entry.filePath(), stop) && ok;
    }
    return QDir().rmdir(path) && ok;
}

} // namespace df::fs
