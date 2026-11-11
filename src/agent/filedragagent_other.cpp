#include "filedragagent.h"

#include <QLabel>
#include <QStandardPaths>

// No native drags on this platform yet (macOS): nothing is caught, nothing is recycled, and a
// drag is never shown (the agent declares no "dnd", so masters send drag.drop; a hover from an
// older master answers "unavailable"). Files dragged here from another device go into folderAt,
// the Downloads folder, and the notice says so.

struct FileDragAgent::Catcher {};
struct FileDragAgent::Hover {};

FileDragAgent::FileDragAgent(QObject *parent) : QObject(parent) {}

FileDragAgent::~FileDragAgent() = default;

bool FileDragAgent::supportsNativeDrag()
{
    return false;
}

void FileDragAgent::catchFiles(QPoint, std::function<void(const QStringList &)> done)
{
    done({});
}

QString FileDragAgent::folderAt(QPoint) const
{
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

bool FileDragAgent::recycle(const QStringList &, QString *error)
{
    *error = tr("not supported on this platform");
    return false;
}

void FileDragAgent::setBadge(const QString &) {}

void FileDragAgent::finishCatch() {}

void FileDragAgent::hover(const HoverSpec &, ClipboardBridge::FetchFiles, std::function<void(const HoverResult &)> done)
{
    HoverResult result;
    result.unavailable = true;
    done(result);
}

void FileDragAgent::endHover(bool) {}

void FileDragAgent::alive() {}

void FileDragAgent::serviceLost() {}

void FileDragAgent::runHover() {}

void FileDragAgent::feedDrag() {}

void FileDragAgent::watchDrag() {}

void FileDragAgent::cancelHere(const char *) {}
