#pragma once

// Dragging files from one device to another (move semantics), as if it were a local drag.
//
// The roles, which may be the same device:
//   master  the device whose keyboard and mouse are shared (InputController decides when a drag
//           starts, where the cursor is, and when the button is released);
//   source  the device the files are dragged on: its agent catches the drag (a drop target under
//           the cursor reads the dragged paths and ends that drag without a drop); it offers the
//           files for transfer (FileTransfer) with their names and sizes;
//   device  whichever device the cursor is on during the drag: if its agent runs native drags
//           (kDragCapability), a real drag goes on there (with the files' icon), so an application
//           takes the drop as it would a local one. Files from another device are virtual files,
//           transferred (fetchDragged) when the drop target reads them; released over a folder
//           window or the desktop, the agent hands the drop to the service instead, which
//           transfers the files into that folder (with the transfer's progress shown). On the
//           source itself the originals are dragged and Windows moves them. A device without
//           native drags is shown nothing, and files released on it are saved into its Received
//           folder.
// When a device other than the source took the files and confirmed it kept them (the service
// finished writing them, or the drop target reported the move: performed drop effect, paste
// succeeded), the source sends the originals to the Recycle Bin. A drop target that read the files
// without confirming gets a copy: the originals stay, and the user is told. If anything fails, the
// originals stay and the user is told.
//
// Messages (reliable channel):
//   master -> source  drag.catch {x, y}                    what is dragged at this point?
//   source -> master  drag.caught {pending, n}             files found, still being listed: wait
//                                                          longer (masters >= kDragRevisionVersion)
//   source -> master  drag.caught {o, n, dc, b, fl}        offer id (0: no files), file count,
//                                                          folder count, bytes, [[relative path,
//                                                          size], ...] (a folder: [path, -1])
//   master -> device  drag.hover {o, s, n, dc, b, fl}      the drag is on you now (s: source); only
//                                                          to a device whose agent shows it (dnd)
//   master -> device  drag.unhover {o}                     ...not any more
//   master -> device  drag.release {o, x, y}               the button was released: drop here
//   master -> source  drag.dropped {o, d}                  released on device d (the only one whose
//                                                          result counts); d the source itself:
//                                                          nothing to transfer (also sent for a
//                                                          catch answered after the master gave up)
//   device -> source  drag.result {o, ok, k, e}            taken (ok): recycle the originals; k: only
//                                                          copied (not confirmed), keep them
//                                                          (sources >= kDragRevisionVersion; an older
//                                                          one is told ok false instead; an older
//                                                          device's ok counts as k)
//   master -> device  drag.drop {o, s, n, dc, b, x, y}     no drag shown there (too many files to
//                                                          list, or no native drags): fetch into the
//                                                          folder under the cursor, else Received
// A device without native drags (its service declares no kDragCapability: no agent running, a
// Wayland session, macOS) is not sent drag.hover: a release there sends drag.drop. Devices before
// 0.1.22 do not say, and are shown the drag as before.
// Only the master is obeyed for catch, hover, release and drop, like for input. Folders (the dragged
// ones and every subfolder, so empty ones survive) are counted apart (dc) and listed only to a peer
// that understands them (kFoldersVersion): an older one gets the files alone.

#include "filetransfer.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QStringList>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace df {

class Node;
class Workspace;
class InputController;

class FileDrag : public QObject
{
    Q_OBJECT
public:
    using ListedFile = df::ListedFile; // a file or a folder (dir)
    struct Hover {
        quint64 offer = 0;
        bool own = false;          // the originals are on this device (paths)
        QStringList paths;
        QList<ListedFile> files;   // otherwise (folders included)
        int count = 0;             // files and folders
        bool local = false;        // this device is the master: its own mouse drives the drag
    };
    // How a drag shown on this device ended (Agent::hover).
    struct HoverEnd {
        bool dropped = false;     // something under the cursor took the drop
        bool confirmed = false;   // ...and reported that it kept the files (virtual files)
        QString folder;           // released over this folder instead (a folder window, the
                                  // desktop): the service transfers the files into it
        bool unavailable = false; // no drag could be shown here (no agent, or no native drags):
                                  // a release saves the files into Received
        bool lost = false;        // the agent showing it went away or never answered: ended,
                                  // nothing taken (the user is told)
    };
    // What the local agent does (the service runs as LocalSystem; Explorer talks to the agent).
    struct Agent {
        // Paths dragged at `at` (empty: nothing, or not files).
        std::function<void(QPoint at, std::function<void(const QStringList &paths)> done)> catchFiles;
        // Runs a drag on this device until endHover, or until it ends there by itself (Escape, a
        // watchdog: neither dropped nor a folder). The agent gets virtual files' contents
        // through fetchDragged.
        std::function<void(const Hover &hover, std::function<void(const HoverEnd &end)> done)> hover;
        std::function<void(bool drop)> endHover;
        // Folder under the cursor at `at` (empty: none); used without a drag shown.
        std::function<void(QPoint at, std::function<void(const QString &folder)> done)> folderAt;
        // Moves the paths to the Recycle Bin.
        std::function<void(const QStringList &paths, std::function<void(bool ok, const QString &error)> done)> recycle;
        // A short text next to the cursor (empty: none), e.g. while a large folder is listed.
        std::function<void(const QString &text)> badge;
    };
    using FolderProvider = std::function<QString()>;

    // The same bounds as copied files (ClipboardSync::kMaxFileBytes, kMaxFiles).
    static constexpr qint64 kMaxBytes = 1024LL * 1024 * 1024 * 1024;
    static constexpr int kMaxFiles = 100000;
    static constexpr int kMaxListedFiles = 2000;
    static constexpr int kKeptDownloads = 3;
    // A notice about the same refusal (a guest, a device that takes no input) at most this often.
    static constexpr int kRefusalNoticeMs = 60000;
    // How long the source keeps the offer of files released on a device, waiting for its result,
    // once another drag starts (a transfer into a folder or a drop target's own extraction may
    // still be reading it).
    static constexpr qint64 kDroppedResultMs = 2LL * 60 * 60 * 1000;
    // First version that understands drag.caught {pending} and drag.result {k}; an older device's
    // drag.result ok is taken as a copy (it said ok without the drop target confirming a move).
    static constexpr const char *kDragRevisionVersion = "0.1.20";

    // downloads: the Received folder; dragged files are transferred into its "Dragged" folder
    // before the drop target copies them, and saved into it when no drag can be shown.
    FileDrag(Node *node, FileTransfer *transfer, Workspace *workspace, InputController *input, Agent agent,
             FolderProvider downloads, QObject *parent = nullptr);
    ~FileDrag() override;

    // The drop target reads the virtual files of the drag on this device: transfer them. Always
    // answered: with an error as soon as the drag is cancelled or leaves this device.
    void fetchDragged(quint64 offer, std::function<void(bool ok, const QStringList &items, const QString &error)> done);

    // Whether this device's agent runs native drags (announced to peers as kDragCapability). On by
    // default when the Agent has a hover function; the service follows its agents.
    void setNativeDrag(bool on);
    bool nativeDrag() const { return m_nativeDrag; }

signals:
    // For the user (moved, failed, ...).
    void notice(const QString &text, bool problem);

private:
    using Waiter = std::function<void(bool, const QStringList &, const QString &)>;

    // Source side.
    struct Outgoing {
        QStringList paths;
        QByteArray target;
        int count = 0; // files
        int dirs = 0;  // folders
        QList<ListedFile> files;
        qint64 droppedAt = 0; // when it was released on `target`
    };
    // done: the offer (0: nothing), its files and folders, bytes, and the list (empty: too long).
    using CaughtCallback = std::function<void(quint64 offer, int count, int dirs, qint64 bytes, const QList<ListedFile> &)>;
    // pending: files were found and are being listed (approximate count: the dragged items).
    void catchHere(QPoint at, std::function<void(int approx)> pending, CaughtCallback done);
    void onResult(const QByteArray &from, quint64 offer, bool ok, bool kept, const QString &error);
    void dropped(quint64 offer, const QByteArray &target);
    // Unpublishes an offer nobody drags any more (the master gave up its catch).
    void withdraw(quint64 offer);

    // Device side (the cursor is here, or the files were released here and are being taken).
    struct Here {
        QByteArray source;
        QByteArray master;
        int count = 0; // files and folders
        qint64 bytes = 0;
        bool released = false;
        bool unavailable = false; // no drag could be shown: a release saves into Received
        bool cancelled = false;   // the drag here ended by itself: a release takes nothing
        quint64 fetchHandle = 0;
        QStringList fetched;
        QList<Waiter> waiters;
    };
    void hoverHere(const QByteArray &master, const QByteArray &source, quint64 offer, int count, int dirs,
                   qint64 bytes, const QList<ListedFile> &files);
    void onHoverEnded(quint64 offer, const HoverEnd &end);
    void unhoverHere(quint64 offer, const QString &why);
    void releaseHere(quint64 offer);
    // The transfer stops and whoever waits for it is answered (the entry is already removed).
    void abandon(Here &here, const QString &why);
    // folder: where the files were put (named in the notice), if not into a drop target.
    void reportTo(const QByteArray &source, quint64 offer, bool ok, const QString &error, bool kept = false,
                  const QString &folder = {});
    // No drag shown: fetch straight into the folder under the cursor (else Received).
    void dropHere(const QByteArray &source, quint64 offer, int count, int dirs, qint64 bytes, QPoint at);
    // Transfers the offer into `folder` (through a hidden staging folder, under free names); into
    // the Received folder when `folder` is empty.
    void receiveInto(const QByteArray &source, quint64 offer, int count, qint64 bytes, const QString &folder);
    // Whether the files can be received into `folder`: an absolute path to a folder the console
    // user sees (it came from the agent).
    bool usableFolder(const QString &folder) const;

    // Master side.
    void onCatchRequested(const QByteArray &source, QPoint at);
    void onDropped(const QByteArray &target, QPoint at);
    void onActiveChanged(const QByteArray &device);
    void sendHover(const QByteArray &device);
    void sendUnhover(const QByteArray &device);
    // Whether a drag can be shown on `device` (native drags, kDragCapability).
    bool showsDrags(const QByteArray &device) const;

    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    QString peerName(const QByteArray &id) const;
    QString peerVersion(const QByteArray &id) const;
    // Whether `id` understands folder entries in lists (this device does).
    bool understandsFolders(const QByteArray &id) const;
    // Whether `id` runs kDragRevisionVersion or later (this device does).
    bool understandsRevision(const QByteArray &id) const;
    void pruneDownloads(const QString &dir) const;
    // A notice for a refusal, at most every kRefusalNoticeMs per kind and device.
    void refusalNotice(const QString &kind, const QByteArray &device, const QString &text);

    Node *m_node;
    FileTransfer *m_transfer;
    Workspace *m_workspace;
    InputController *m_input;
    Agent m_agent;
    FolderProvider m_downloads;
    bool m_nativeDrag = false;

    // Master: the drag in progress.
    QByteArray m_dragSource;
    quint64 m_dragOffer = 0;
    int m_dragCount = 0; // files
    int m_dragDirs = 0;  // folders
    qint64 m_dragBytes = 0;
    QList<ListedFile> m_dragFiles;
    QByteArray m_hoverDevice; // device the drag is shown on

    // Source: offers of dragged files, until their result.
    QHash<quint64, Outgoing> m_outgoing;
    // Source: listing dragged folders happens on threads of its own (a large tree takes a while,
    // with the console user's rights); the newest catch wins. Joined when done or on destruction.
    struct Walk;
    std::vector<std::shared_ptr<Walk>> m_walks;
    quint64 m_catchSeq = 0;
    // Device: the drag shown here (m_shown) and drags released here that are still being taken.
    QHash<quint64, Here> m_heres;
    quint64 m_shown = 0;

    QHash<QString, qint64> m_refusalNoticed; // kind + device -> when
};

} // namespace df
