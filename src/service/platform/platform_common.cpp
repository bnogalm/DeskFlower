#include "platform.h"

#include "fsutil.h"

#include <QDir>

namespace df::platform {

bool isAllowedSharedFolder(const QString &path, bool serviceMode, QString *why)
{
    const auto fail = [why](const QString &reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (!QDir::isAbsolutePath(path))
        return fail(QStringLiteral("not an absolute path"));
    if (!serviceMode)
        return true;
    const QString home = interactiveUserHome(serviceMode);
    if (home.isEmpty())
        return fail(QStringLiteral("nobody is signed in"));
    if (!fs::isInside(path, home))
        return fail(QStringLiteral("it must be inside your user folder (%1)").arg(QDir::toNativeSeparators(home)));
    if (fs::passesThroughLink(path))
        return fail(QStringLiteral("it goes through a link or junction"));
    return true;
}

SessionEvents *SessionEvents::instance()
{
    static SessionEvents events;
    return &events;
}

} // namespace df::platform
