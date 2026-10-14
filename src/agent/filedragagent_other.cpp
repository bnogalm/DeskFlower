#include "filedragagent.h"

#include <QLabel>

// Dragging files between devices is not supported on this platform yet: nothing is caught, no
// folder is found, nothing is recycled.

struct FileDragAgent::Catcher {};
struct FileDragAgent::Hover {};

FileDragAgent::FileDragAgent(QObject *parent) : QObject(parent) {}

FileDragAgent::~FileDragAgent() = default;

void FileDragAgent::catchFiles(QPoint, std::function<void(const QStringList &)> done)
{
    done({});
}

QString FileDragAgent::folderAt(QPoint) const
{
    return {};
}

bool FileDragAgent::recycle(const QStringList &, QString *error)
{
    *error = tr("not supported on this platform");
    return false;
}

void FileDragAgent::setBadge(const QString &) {}

void FileDragAgent::finishCatch() {}

void FileDragAgent::hover(const HoverSpec &, ClipboardBridge::FetchFiles, std::function<void(bool, bool)> done)
{
    done(false, false);
}

void FileDragAgent::endHover(bool) {}

void FileDragAgent::runHover() {}

void FileDragAgent::feedDrag() {}
