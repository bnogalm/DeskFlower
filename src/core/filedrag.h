#pragma once

// Dragging files from one device to another (move semantics), as if it were a local drag.
//
// The roles, which may be the same device:
//   master  the device whose keyboard and mouse are shared (InputController decides when a drag
//           starts, where the cursor is, and when the button is released);
//   source  the device the files are dragged on: its agent catches the drag (a drop target under
//           the cursor reads the dragged paths and ends that drag without a drop); it offers the
//           files for transfer (FileTransfer) with their names and sizes;
//   device  whichever device the cursor is on during the drag: its agent runs a real drag there
//           (with the files' icon), so Explorer, the desktop or an application take the drop as
//           they would a local one. Files from another device are virtual files, transferred
//           (fetchDragged) when the drop target reads them; on the source itself the originals are
//           dragged and Windows moves them.
// When a device other than the source took the files, the source sends the originals to the
// Recycle Bin. If anything fails, the originals stay.
//
// Messages (reliable channel):
//   master -> source  drag.catch {x, y}                    what is dragged at this point?
//   source -> master  drag.caught {o, n, b, fl}            offer id (0: no files), count, bytes,
//                                                          [[relative path, size], ...]
//   master -> device  drag.hover {o, s, n, b, fl}          the drag is on you now (s: source)
//   master -> device  drag.unhover {o}                     ...not any more
//   master -> device  drag.release {o, x, y}               the button was released: drop here
//   master -> source  drag.dropped {o, d}                  released on device d (the only one whose
//                                                          result counts)
//   device -> source  drag.result {o, ok, e}               taken (ok): recycle the originals
//   master -> device  drag.drop {o, s, n, b, x, y}         without a file list (too many files):
//                                                          fetch into the folder under the cursor
// Only the master is obeyed for catch, hover, release and drop, like for input.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QStringList>
#include <functional>

namespace df {

class Node;
class FileTransfer;
class Workspace;
class InputController;

class FileDrag : public QObject
{
    Q_OBJECT
public:
    struct ListedFile {
        QString path; // relative, '/' separated, top-level item first
        qint64 size = 0;
    };
    struct Hover {
        quint64 offer = 0;
        bool own = false;          // the originals are on this device (paths)
        QStringList paths;
        QList<ListedFile> files;   // otherwise
        int count = 0;
    };
    // What the local agent does (the service runs as LocalSystem; Explorer talks to the agent).
    struct Agent {
        // Paths dragged at `at` (empty: nothing, or not files).
        std::function<void(QPoint at, std::function<void(const QStringList &paths)> done)> catchFiles;
        // Runs a drag on this device until endHover; done(dropped, transferred). The agent gets
        // virtual files' contents through fetchDragged.
        std::function<void(const Hover &hover, std::function<void(bool dropped, bool transferred)> done)> hover;
        std::function<void(bool drop)> endHover;
        // Folder under the cursor at `at` (empty: none); used without a file list.
        std::function<void(QPoint at, std::function<void(const QString &folder)> done)> folderAt;
        // Moves the paths to the Recycle Bin.
        std::function<void(const QStringList &paths, std::function<void(bool ok, const QString &error)> done)> recycle;
    };
    using FolderProvider = std::function<QString()>;

    static constexpr qint64 kMaxBytes = 4LL * 1024 * 1024 * 1024;
    static constexpr int kMaxFiles = 10000;
    static constexpr int kMaxListedFiles = 2000;
    static constexpr int kKeptDownloads = 3;

    // downloads: where dragged files are transferred to before the drop target copies them.
    FileDrag(Node *node, FileTransfer *transfer, Workspace *workspace, InputController *input, Agent agent,
             FolderProvider downloads, QObject *parent = nullptr);

    // The drop target reads the virtual files of the drag on this device: transfer them.
    void fetchDragged(quint64 offer, std::function<void(bool ok, const QStringList &items, const QString &error)> done);

signals:
    // For the user (moved, failed, ...).
    void notice(const QString &text, bool problem);

private:
    // Source side.
    struct Outgoing {
        QStringList paths;
        QByteArray target;
        int count = 0;
        QList<ListedFile> files;
    };
    void catchHere(QPoint at, std::function<void(quint64 offer, int count, qint64 bytes, const QList<ListedFile> &)> done);
    void onResult(const QByteArray &from, quint64 offer, bool ok, const QString &error);
    void dropped(quint64 offer, const QByteArray &target);

    // Device side (the cursor is here).
    struct Here {
        quint64 offer = 0;
        QByteArray source;
        int count = 0;
        qint64 bytes = 0;
        bool released = false;
        quint64 fetchHandle = 0;
        QStringList fetched;
        QList<std::function<void(bool, const QStringList &, const QString &)>> waiters;
    };
    void hoverHere(const QByteArray &source, quint64 offer, int count, qint64 bytes, const QList<ListedFile> &files);
    void unhoverHere(quint64 offer);
    void releaseHere(quint64 offer);
    void reportTo(const QByteArray &source, quint64 offer, bool ok, const QString &error);
    // Without a file list: fetch straight into the folder under the cursor.
    void dropHere(const QByteArray &source, quint64 offer, int count, qint64 bytes, QPoint at);

    // Master side.
    void onCatchRequested(const QByteArray &source, QPoint at);
    void onDropped(const QByteArray &target, QPoint at);
    void onActiveChanged(const QByteArray &device);
    void sendHover(const QByteArray &device);
    void sendUnhover(const QByteArray &device);

    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    QString peerName(const QByteArray &id) const;
    void pruneDownloads(const QString &dir) const;

    Node *m_node;
    FileTransfer *m_transfer;
    Workspace *m_workspace;
    InputController *m_input;
    Agent m_agent;
    FolderProvider m_downloads;

    // Master: the drag in progress.
    QByteArray m_dragSource;
    quint64 m_dragOffer = 0;
    int m_dragCount = 0;
    qint64 m_dragBytes = 0;
    QList<ListedFile> m_dragFiles;
    QByteArray m_hoverDevice; // device the drag is shown on

    // Source: offers of dragged files, until their result.
    QHash<quint64, Outgoing> m_outgoing;
    // Device: the drag shown here.
    Here m_here;
};

} // namespace df
