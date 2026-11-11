#pragma once

// The agent's part of dragging files between devices (see FileDrag in the service): it runs as the
// user, so it can talk to Explorer, which the service (LocalSystem) cannot.
//  - catchFiles: a drop target under the cursor learns which files are being dragged there and
//    ends that drag without a drop (the files are moved by Deskflower instead);
//  - hover: while the cursor is on this device, a real drag (with the files' icon) goes on here,
//    so an application decides what to do with the files, as for a local drag. Files from another
//    device are virtual files transferred when the drop target reads them (asynchronously where the
//    target supports it, with its own progress); released over a folder window or the desktop, the
//    drag is ended without a drop and the folder is handed back instead (HoverResult::folder): the
//    service transfers the files there, with its progress shown, rather than Explorer waiting
//    inside the drop with nothing to show. Back on the device they came from, the originals are
//    dragged and Windows moves them itself. It ends when the master says (endHover: drop or
//    cancel), not with this device's button, or here by itself: Escape, the user's own button
//    released while this device is the master and the service says nothing, the service silent
//    for kAliveTimeoutMs (dragAlive, from a service that announced it: HoverSpec::heartbeat or a
//    first dragAlive), or the connection to the service lost;
//  - folderAt: the folder of the Explorer window, or the desktop, under a point; the Downloads
//    folder outside Windows (no file manager says which folder is under a point);
//  - recycle: moves files to the Recycle Bin, or the trash (undoable);
//  - badge: a small label next to the cursor.
// Native drags (supportsNativeDrag, announced to the service, which tells peers: kDragCapability)
// exist on Windows (filedragagent_win.cpp), Linux X11 (filedragagent_x11.cpp, XDND, where a drop
// waits for the transfer and folder windows are not told apart) and macOS (filedragagent_mac.mm:
// the drag pasteboard, a transparent window and NSDraggingSession with file promises; it needs the
// Accessibility permission, and answers "unavailable" without it). In a Wayland session catchFiles
// answers "nothing" and hover "unavailable" at once: masters send files dragged onto the device
// with drag.drop, and they go into folderAt.

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

    // Whether hover runs native drags on this platform (the "dnd" the agent declares).
    static bool supportsNativeDrag();

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
        bool local = false;                       // this device is the master (its own mouse)
        bool heartbeat = false;                   // the service says dragAlive while it runs (an
                                                  // older one does not: no silence watchdog)
    };
    struct HoverResult {
        bool dropped = false;     // something under the cursor took the drop
        bool confirmed = false;   // the target reported moving the virtual files (and they were
                                  // transferred): the originals may go
        QString folder;           // released over this folder window or the desktop instead
        bool unavailable = false; // no drag can be shown here
    };
    void hover(const HoverSpec &spec, ClipboardBridge::FetchFiles fetch, std::function<void(const HoverResult &)> done);
    void endHover(bool drop);
    // The service is still there (it says so every couple of seconds while a drag is shown).
    void alive();
    // The connection to the service dropped: nothing here can finish a drag any more.
    void serviceLost();

    static constexpr int kCatchTimeoutMs = 900;
    static constexpr int kAliveTimeoutMs = 10000;
    // The master's own button released here, and still no word from the service: cancelled.
    static constexpr int kLocalReleaseGraceMs = 1500;
    // How long a drop target may extract virtual files on its own thread.
    static constexpr unsigned long kAsyncDropTimeoutMs = 30UL * 60 * 1000;

signals:
    // Something the user must do for dragging to work here (macOS: a permission).
    void notice(const QString &text);

private:
    struct Catcher;
    void finishCatch();
    struct Hover;
    void runHover();
    void feedDrag();
    void watchDrag();
    // Ends the drag going on here without a drop (why: for the log).
    void cancelHere(const char *why);
    QTimer m_feed;  // see feedDrag
    QTimer m_watch; // see watchDrag

    std::shared_ptr<Hover> m_hover;     // the drag going on here (its loop)
    std::shared_ptr<Hover> m_nextHover; // asked for while another one was ending
    bool m_inDrag = false;              // a drag loop runs

    std::unique_ptr<Catcher> m_catcher;
    std::unique_ptr<QLabel> m_badge;
    QTimer m_follow;
};
