#include "platform.h"

#include "fsutil.h"

#include <QDir>

namespace df::platform {

namespace {
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kCase = Qt::CaseSensitive;
#endif

// Where the operating system and the installed programs live (outside any home folder).
QStringList systemFolders()
{
#ifdef Q_OS_WIN
    QStringList folders;
    for (const char *name : {"SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "ProgramData"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty())
            folders << QDir::cleanPath(QDir::fromNativeSeparators(value));
    }
    folders << QStringLiteral("C:/Windows") << QStringLiteral("C:/Program Files")
            << QStringLiteral("C:/Program Files (x86)") << QStringLiteral("C:/ProgramData");
    return folders;
#else
    return {QStringLiteral("/bin"), QStringLiteral("/boot"), QStringLiteral("/dev"), QStringLiteral("/etc"),
            QStringLiteral("/lib"), QStringLiteral("/lib32"), QStringLiteral("/lib64"), QStringLiteral("/libx32"),
            QStringLiteral("/opt"), QStringLiteral("/proc"), QStringLiteral("/root"), QStringLiteral("/sbin"),
            QStringLiteral("/srv"), QStringLiteral("/sys"), QStringLiteral("/usr"), QStringLiteral("/var"),
            QStringLiteral("/System"), QStringLiteral("/Library"), QStringLiteral("/Applications"),
            QStringLiteral("/private"), QStringLiteral("/cores")};
#endif
}

bool isDriveRoot(const QString &path) { return QDir(path).isRoot(); }
} // namespace

QString sharedFolderProblem(const QString &path, const QString &home, bool insideHomeOnly)
{
    if (!QDir::isAbsolutePath(path))
        return QStringLiteral("it is not an absolute path");
    const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString homeFolder = QDir::cleanPath(QDir::fromNativeSeparators(home));
    if (!home.isEmpty() && fs::isInside(folder, homeFolder)) {
        if (folder.compare(homeFolder, kCase) == 0)
            return QStringLiteral("that is your whole user folder; choose or create a folder inside it");
        // Below the home folder: no hidden folders and no folders where programs keep their
        // settings (AppData holds the Startup folder, for instance).
        const QStringList parts = folder.mid(homeFolder.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (part.startsWith(QLatin1Char('.')))
                return QStringLiteral("it is a hidden folder");
        }
#ifdef Q_OS_WIN
        if (!parts.isEmpty() && parts.first().compare(QLatin1String("AppData"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("programs keep their settings there (AppData)");
#elif defined(Q_OS_MACOS)
        if (!parts.isEmpty() && parts.first() == QLatin1String("Library"))
            return QStringLiteral("programs keep their settings there (Library)");
#endif
    } else {
        if (insideHomeOnly)
            return QStringLiteral("it must be inside your user folder (%1)").arg(QDir::toNativeSeparators(home));
        if (isDriveRoot(folder))
            return QStringLiteral("it is a whole drive; choose or create a folder on it");
        for (const QString &system : systemFolders()) {
            if (fs::isInside(folder, system))
                return QStringLiteral("it is a system folder");
        }
    }
    return {};
}

bool isAllowedSharedFolder(const QString &path, bool serviceMode, QString *why)
{
    // A Windows service writes and serves the files as LocalSystem: only the console user's own
    // folders, reached without links. Elsewhere the service runs as the user, who may also share
    // another disk, but no system folder.
    const QString home = interactiveUserHome(serviceMode);
    QString problem;
    if (serviceMode && home.isEmpty())
        problem = QStringLiteral("nobody is signed in");
    else
        problem = sharedFolderProblem(path, home, separatesUsers(serviceMode));
    if (problem.isEmpty() && fs::passesThroughLink(path))
        problem = QStringLiteral("it goes through a link or junction");
    if (why)
        *why = problem;
    return problem.isEmpty();
}

QString sharedFolder(const QString &chosen, const QString &fallback, bool serviceMode)
{
    QString folder = chosen;
    if (!folder.isEmpty() && !isAllowedSharedFolder(folder, serviceMode))
        folder.clear();
    if (folder.isEmpty()) {
        const QString home = interactiveUserHome(serviceMode);
        folder = home.isEmpty() ? fallback : QDir(home).filePath(QStringLiteral("Deskflower"));
    }
    if (const auto asUser = actAsInteractiveUser(serviceMode))
        QDir().mkpath(folder);
    return folder;
}

QString receivedFolder(const QString &fallback, bool serviceMode)
{
    const QString home = interactiveUserHome(serviceMode);
    if (home.isEmpty())
        return fallback;
#if defined(Q_OS_WIN)
    const QString folder = QDir(home).filePath(QStringLiteral("AppData/Local/Deskflower/Received"));
#elif defined(Q_OS_MACOS)
    const QString folder = QDir(home).filePath(QStringLiteral("Library/Caches/Deskflower/Received"));
#else
    // The service runs as the user here, so its environment is the user's.
    QString cache = qEnvironmentVariable("XDG_CACHE_HOME");
    if (!QDir::isAbsolutePath(cache))
        cache = QDir(home).filePath(QStringLiteral(".cache"));
    const QString folder = QDir(cache).filePath(QStringLiteral("deskflower/received"));
#endif
    if (const auto asUser = actAsInteractiveUser(serviceMode))
        QDir().mkpath(folder);
    return folder;
}

SessionEvents *SessionEvents::instance()
{
    static SessionEvents events;
    return &events;
}

} // namespace df::platform
