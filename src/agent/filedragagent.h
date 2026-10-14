#pragma once

// The agent's part of dragging files between devices (see FileDrag in the service): it runs as the
// user, so it can talk to Explorer, which the service (LocalSystem) cannot.
//  - catchFiles: a drop target under the cursor learns which files are being dragged there and
//    ends that drag without a drop (the files are moved by Deskflower instead);
//  - hover: while the cursor is on this device, a real drag (with the files' icon) goes on here,
//    so Explorer, the desktop or an application decide where the files go, as for a local drag.
//    Files from another device are virtual files transferred when dropped; back on the device
//    they came from, the originals are dragged and Windows moves them itself. It ends when the
//    master says (endHover: drop or cancel), not with this device's button;
//  - folderAt: the folder of the Explorer window, or the desktop, under a point (fallback when
//    there is no file list);
//  - recycle: moves files to the Recycle Bin (undoable);
//  - badge: a small label next to the cursor.
// Windows only; elsewhere every request answers "nothing".

#include "clipboardbridge.h"

#include <QObject>
#include <QPoint>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>

class QLabel;

class FileDragAgent : public QObject
{
    Q_OBJECT
public:
    explicit FileDragAgent(QObject *parent = nullptr);
    ~FileDragAgent() override;

    // `at` in native desktop coordinates.
    void catchFiles(QPoint at, std::function<void(const QStringList &paths)> done);
    QString folderAt(QPoint at) const;
    bool recycle(const QStringList &paths, QString *error);
    void setBadge(const QString &text);

    struct HoverSpec {
        bool own = false;                         // the originals are here (paths)
        QStringList paths;
        QList<ClipboardBridge::ListedFile> files; // otherwise: names and sizes of virtual files
        int count = 0;
    };
    // done(dropped, transferred): whether something accepted the drop, and whether the virtual
    // files were transferred (always false for own files).
    void hover(const HoverSpec &spec, ClipboardBridge::FetchFiles fetch, std::function<void(bool, bool)> done);
    void endHover(bool drop);

    static constexpr int kCatchTimeoutMs = 900;

private:
    struct Catcher;
    void finishCatch();
    struct Hover;
    void runHover();
    void feedDrag();
    QTimer m_feed; // see feedDrag

    std::unique_ptr<Hover> m_hover;   // the drag going on here
    std::unique_ptr<Hover> m_nextHover; // asked for while another one was ending
    bool m_inDrag = false;

    std::unique_ptr<Catcher> m_catcher;
    std::unique_ptr<QLabel> m_badge;
    QTimer m_follow;
};
