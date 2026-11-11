#pragma once

// Bulk transfers over encrypted TCP streams (SecureStream).
//
// A device publishes an "offer": named blobs (e.g. a large clipboard image) and files/folders.
// Peers fetch it by id; the publisher only ever serves what it offered. Protocol 3
// (wire::kProtocolVersion), one stream per fetch, every frame starting with a kind byte
// (1 = CBOR control, 3 = data):
//   client: get {o: offer id, files: bool, v: 3, pv: 3}
//   server: manifest {pv: 3, items: [{k: "blob"|"file"|"dir", n: name or relative path, s: size}],
//                     more: bool}
//           data frames, packed: items concatenated in manifest order (folders carry no data),
//           every frame full (SecureStream::kMaxFrame bytes with its kind byte) but the last, a
//           frame often ending one file and starting the next
//           end {}            or   error {r}
// Folder items ("dir": the chosen folders and every subfolder, so empty ones survive); a manifest
// split over several frames ("more": another manifest frame follows; a frame holds at most
// SecureStream::kMaxFrame bytes).
// Protocols are not mixed: 0.1.22-0.1.30 (protocol 2, never advertised: get v 2 without pv,
// manifests without pv, data frames of kind 2) and earlier ones are refused both ways with an
// error that names the device to update. A publisher answers a get without pv 3 with that error
// (an older receiver shows it as its transfer error); a receiver refuses a manifest frame without
// pv 3 (what an older publisher sends) and data frames of kind 2; an older receiver ignores data
// frames of kind 3 and fails with "Transfer ended early", never misreading them. A fetch from a
// peer whose hello advertises another protocol (Node::transfersCompatible) fails at once, without
// opening a stream (incompatibleReason).
// The receiver sanitises relative paths and writes files under its target directory.
// Guests (Node::isGuest) may only fetch offers they were let into (allowPeer), and a gated offer
// (Offer::gated, the shared clipboard) is served to nobody else either: only to the peers the
// publisher let in, one by one.
//
// Threads: the receiving side's disk work (folders, files, free space, cleanup) runs on one
// worker thread per fetch, as the console user, never on the thread that owns the FileTransfer
// (in the service it routes input and keepalives, which a disk must not hold up). That thread
// keeps the network: it hands the received frames to the worker through a bounded queue, pauses
// reading the stream when the worker falls behind (SecureStream::setReadPaused), and opens the
// stream (and sends the get) only once the worker is ready to write: the publisher waits for the
// get under its handshake timeout, which a long preparation would outlast.
// The worker makes the folders (held open while the fetch writes: fs::SafeTree, so no link can
// redirect a file and no folder can be renamed or swapped meanwhile) and splits the data between
// the files; a few writer threads of the fetch (kMaxWriters at most; one unless it has at least
// 64 files under 1 MB, so that a tree of small files with a few huge ones gets several too)
// create, write and close the files, all of one file on one writer, as the console user checked
// against the user the fetch started with. Their queues share a budget (kWriterQueueHigh) behind
// which the worker waits, and the stream is paused behind it as before. A cancel, a failure or
// the end waits for every writer before the result is reported and before any cleanup.
// The serving side is split the same way: one reader thread per upload builds the manifest and
// reads the offered files, as the console user, into a bounded queue of ready frames; the
// owning thread only sends them, as fast as the stream takes them. A tree of small files waits
// for opening and closing them, one after another: a few helper threads of the upload (at most
// kReadAheadThreads) open and read the small files (kReadAheadMax bytes at most) ahead of it, in
// manifest order, as the same console user (checked against the identity the upload recorded
// first), into memory bounded by kUploadQueueHigh; the reader still packs every item in manifest
// order, so the frames are the very ones it sent before (no protocol change), and a file that
// could not be opened or changed size fails the upload with the same error, at the same item.
// A list fetch (fetchList: the manifest alone, for a paste that shows the files before their
// contents) writes nothing and stays on the owning thread. It is an ordinary get: the receiver
// ends the stream after the last manifest frame, and the publisher, told nothing, sees its
// receiver go and stops (it may have read up to kHighWater ahead).

#include <QByteArray>
#include <QCborArray>
#include <QCborValue>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <functional>
#include <map>
#include <memory>

namespace df::detail {
class TaskPool;
}

namespace df {

class Node;
class SecureStream;

// Gives the calling thread, for as long as the returned guard lives, the rights to read and write
// the console user's files. The Windows service runs as LocalSystem and must never use its own
// rights on paths a console-session process supplied: it serves and receives files while
// impersonating that user. A null guard means nobody is signed in: the operation is refused.
// Default (tests, and platforms where the service runs as the user): a no-op guard that is never
// null. Injected from src/service/main.cpp with platform::actAsInteractiveUser.
using Impersonator = std::function<std::shared_ptr<void>()>;

// First version that understands folder items: in transfer manifests (get v 2) and in the file
// lists of clipboard announcements and drags ([path, -1]). An older peer gets lists without them.
inline constexpr const char *kFoldersVersion = "0.1.20";
bool peerUnderstandsFolders(const QString &version);

// One entry of a file list shown before anything is transferred (clipboard announcements, drags):
// a file with its size, or a folder (size -1 on the wire and over IPC).
struct ListedFile {
    QString path; // relative, '/' separated, top-level item first ("photos/2024/a.jpg")
    qint64 size = 0;
    bool dir = false;
};
// [[path, size], ...], folders as [path, -1]; withDirs false leaves the folders out (older peers).
QCborArray listedToCbor(const QList<ListedFile> &list, bool withDirs = true);
// The reverse. Every name must be safe, or none is used: false, and an empty list.
bool listedFromCbor(const QCborValue &value, QList<ListedFile> *out);
// The number of folders in a list.
int listedDirCount(const QList<ListedFile> &list);

struct Offer {
    struct File {
        QString absolutePath;
        QString relativePath; // includes the top-level item name ("photos/2024/a.jpg")
        qint64 size = 0;
    };
    QMap<QString, QByteArray> blobs;
    QList<File> files;
    // Folders, as relative paths: the chosen folders themselves and every subfolder, parents
    // first. The receiver creates them, so empty folders are not lost.
    QStringList dirs;
    // What addPaths left out, as relative paths: symbolic links and junctions inside a chosen
    // folder (they could lead anywhere), entries that are neither a file nor a folder, chosen paths
    // that do not exist. The caller tells the user.
    QStringList skipped;
    QSet<QByteArray> allowed; // peers let in (FileTransfer::allowPeer): guests need it, and so does
                              // everyone when the offer is gated
    bool gated = false;       // served only to `allowed`, trusted peers included

    qint64 fileBytes() const;
    qint64 blobBytes() const;
    bool hasFiles() const { return !files.isEmpty() || !dirs.isEmpty(); }
    // Folders first (parents before children), then the files.
    QList<ListedFile> listing() const;
    // Expands files and folders (recursively), every folder an entry of its own. Hidden and system
    // files and Windows shortcuts (.lnk: an ordinary file, though Qt reports its target's type and
    // size) are offered like any file. A chosen path that is a link is followed (the user picked
    // it); links met inside a chosen folder are skipped and listed in `skipped`. Fails if the files
    // exceed maxBytes or maxFiles, or the folders maxFiles (counted apart). Runs as whoever calls
    // it: use FileTransfer::addOfferPaths so the service reads the console user's files with their
    // rights, not LocalSystem's.
    static bool addPaths(Offer *offer, const QStringList &paths, qint64 maxBytes, int maxFiles, QString *error);
};

struct FetchResult {
    bool ok = false;
    QString error;
    QMap<QString, QByteArray> blobs;
    QStringList files; // absolute local paths of the top-level items (files and folders)
    bool cancelled = false; // ended by FileTransfer::cancel (error "Cancelled")
};

struct FetchOptions {
    // Non-empty ("clipboard", "drag", "drop"): a fetch the user sees (see FileTransfer::fetch).
    QString kind;
    // Run on the fetch's worker thread, as the console user, before anything is asked of the
    // peer (making room: setting older received copies aside), and before the stream to it is
    // opened: the transfer starts only once it returns, so it should be quick (renaming, not
    // deleting: see cleanupAsUser). It must not touch the caller's objects: it may run after they
    // are gone.
    std::function<void()> prepareAsUser;
    // The slow part of making room (deleting what prepareAsUser set aside), run once
    // prepareAsUser has returned (whether the fetch goes on or not: also after a cancel or a
    // failure), on a thread of FileTransfer::runAsUser's pool, as the console user the fetch
    // recorded (not at all if nobody is signed in or another user is by then), alongside the
    // transfer. A fetch whose files do not fit in the free space waits for it to end before it is
    // refused (the space it frees counts), so it returns only once that space is free (not while
    // another thread still deletes what it set aside). Dropped if the FileTransfer goes first. The
    // same rules as prepareAsUser: it must not touch the caller's objects.
    std::function<void()> cleanupAsUser;
};

class FileTransfer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kChunk = 60 * 1024;
    static constexpr qint64 kHighWater = 1024 * 1024; // pause sending above this many queued bytes
    static constexpr int kKeptOffers = 4;
    // Received frames waiting for the worker: reading pauses at the high-water mark and resumes
    // at the low one.
    static constexpr qint64 kReceiveQueueHigh = 8 * 1024 * 1024;
    static constexpr qint64 kReceiveQueueLow = 4 * 1024 * 1024;
    // Frames an upload's reader thread has ready to send: it waits at the high-water mark (the
    // stream itself holds up to kHighWater more).
    static constexpr qint64 kUploadQueueHigh = 2 * 1024 * 1024;
    static constexpr qint64 kUploadQueueLow = 1024 * 1024;
    // An upload's read-ahead (see above): helper threads, and the largest file they read. What
    // they hold read and not yet packed stays within kUploadQueueHigh (every file counted as at
    // least kReadAheadSlot, so empty files do not run ahead without bound).
    static constexpr int kReadAheadThreads = 4;
    static constexpr qint64 kReadAheadMax = 256 * 1024;
    static constexpr qint64 kReadAheadSlot = 4096;
    // Uploads of one offer to one peer at the same time: each reads the whole offer (a reader
    // thread, its manifest, its queue), so a peer that keeps asking for the same offer is told
    // "Too many transfers of these files at once" rather than given yet another thread. Far above
    // anything a user does (pasting the same copy again while it still arrives).
    static constexpr int kMaxUploadsPerOfferAndPeer = 8;
    // The writer threads of a fetch (see filetransfer.cpp): at most this many, and the bytes their
    // queues hold together before the fetch's worker waits for them (it goes on at the low mark).
    // A fetch with at least kParallelSmallFiles files under kSmallFile bytes gets several
    // (half the processors, 2 to kMaxWriters), whatever else it holds; any other gets one.
    static constexpr int kMaxWriters = 8;
    static constexpr int kParallelSmallFiles = 64;
    static constexpr qint64 kSmallFile = 1024 * 1024;
    static constexpr qint64 kWriterQueueHigh = 8 * 1024 * 1024;
    static constexpr qint64 kWriterQueueLow = 4 * 1024 * 1024;
    // A fetch with no news for this long fails ("Transfer timed out"). Disk work keeps it alive.
    static constexpr int kIdleTimeoutMs = 30000;
    // How long the destructor waits for the worker threads before leaving them to end alone.
    static constexpr int kShutdownWaitMs = 2000;
    // An upper bound on the number of items (blobs plus files) a manifest may announce, so a
    // hostile sender cannot force an unbounded item list into memory. One manifest is one frame
    // (SecureStream::kMaxFrame, 64 KB) or a split one; this is above anything legitimate (copied
    // files and folders are bounded at 100,000 each, plus a few blobs).
    static constexpr int kMaxItems = 250000;

    explicit FileTransfer(Node *node, QObject *parent = nullptr);
    // Pending fetches are dropped without calling their callbacks; their workers are stopped (at
    // most kShutdownWaitMs is spent waiting for them) and clean up nothing.
    ~FileTransfer() override;

    // The service reads and writes the console user's files as that user (see Impersonator): never
    // its own LocalSystem rights on a path a console-session process chose. Default: a no-op.
    void setImpersonator(Impersonator impersonate) { m_impersonate = std::move(impersonate); }
    // The guard for one read/write pass (null: nobody is signed in, refuse the operation).
    std::shared_ptr<void> impersonate() const;
    // The impersonator itself, for work done on another thread (empty: the no-op default).
    Impersonator impersonator() const { return m_impersonate; }
    // Who the impersonated user is, called on a worker thread while impersonating (Windows: the
    // SID of the thread's token, platform::impersonatedUserId). A fetch records it at its first
    // batch of work and stops ("The signed-in user changed") when a later batch differs, without
    // deleting anything: it would be deleting as somebody else. An empty answer (the query failed)
    // counts as unknown, never as a change. Default: none (no check).
    void setUserIdentity(std::function<QString()> identity) { m_identity = std::move(identity); }
    // Runs `work` on a small pool of threads (two) as the console user (skipped if nobody is
    // signed in), then `done`, if any, on this object's thread (skipped if it is gone by then).
    // For disk work that must not hold up this thread, such as deleting old received copies.
    // `work` must not touch objects that may go away meanwhile.
    void runAsUser(std::function<void()> work, std::function<void()> done = {});

    quint64 publish(const Offer &offer);
    void unpublish(quint64 offerId);
    // Lets `peerId` fetch the offer (a guest, or anyone when the offer is gated), or not any more
    // (unfinished transfers go on).
    void allowPeer(quint64 offerId, const QByteArray &peerId, bool on);

    // Offer::addPaths while impersonating the console user: the service enumerates and later serves
    // only files that user may read. Reading is bounded by that user's rights, so a chosen path
    // that goes through a link (a junction such as "My Documents", a redirected folder) is fine;
    // links inside a chosen folder are skipped (Offer::skipped).
    bool addOfferPaths(Offer *offer, const QStringList &paths, qint64 maxBytes, int maxFiles, QString *error);

    using Callback = std::function<void(const FetchResult &)>;
    // Downloads offer `offerId` from `peerId`. Files go under targetDir. Returns a handle for cancel().
    // A non-empty `kind` ("clipboard", "drag", "drop") makes the fetch one the user sees: its
    // progress is reported (transferProgress: when the manifest arrives, then at most every
    // kProgressIntervalMs) and its end (transferFinished).
    // The callback runs later, on this object's thread (never inside fetch), or inside cancel().
    quint64 fetch(const QByteArray &peerId, quint64 offerId, const QString &targetDir, bool includeFiles,
                  qint64 maxBytes, Callback done, FetchOptions options);
    quint64 fetch(const QByteArray &peerId, quint64 offerId, const QString &targetDir, bool includeFiles,
                  qint64 maxBytes, Callback done, const QString &kind = {});
    // Ends a fetch in progress: its callback runs at once with "Cancelled" (FetchResult::cancelled)
    // and its worker removes what it wrote, later (also when it had just finished). A failed fetch
    // is reported first and cleaned up afterwards, by its worker too. Unknown handles are ignored.
    void cancel(quint64 handle);
    // `refused` (with !ok): the publisher's manifest itself was refused (the checks below, or data
    // before it), which asking again would not change; otherwise a failure of the way there (not
    // reachable, closed, timed out, the publisher's error).
    using ListCallback =
        std::function<void(bool ok, bool refused, const QString &error, const QList<ListedFile> &items)>;
    // The files and folders of offer `offerId` at `peerId` (folders first, as Offer::listing),
    // without their contents: an ordinary get whose stream is dropped once the manifest is in, so
    // a publisher of the same protocol serves it (another one is refused). The manifest gets
    // the same checks as a fetch's (sizes within maxBytes, kMaxItems, sanitised names, paths that
    // differ only in case on Windows and macOS). Nothing is written: no worker, no free-space
    // check, no impersonation. Returns a handle for cancelList(). The callback runs later, on this
    // object's thread (never inside fetchList), once; never if this object goes first.
    quint64 fetchList(const QByteArray &peerId, quint64 offerId, qint64 maxBytes, ListCallback done);
    // Drops a list fetch in progress without calling its callback. Unknown handles are ignored.
    void cancelList(quint64 handle);
    // Why transfers with `peerId` are refused (its hello advertises another protocol than this
    // device's: Node::transfersCompatible), as the user reads it; empty when they are not.
    QString incompatibleReason(const QByteArray &peerId) const;
    // Whether `handle` is a fetch in progress that the user sees (fetched with a kind).
    bool isVisibleFetch(quint64 handle) const;
    // Whether a fetch still at work (its worker's cleanup after a cancel or a failure included)
    // writes into `dir` or below it. Thread-safe.
    bool isActiveTarget(const QString &dir) const;
    // The same check as a function that stays valid on any thread, also after this object is gone
    // (it then answers from the workers still running).
    std::function<bool(const QString &)> activeTargetCheck() const;

    // For tests: a delay before every write to disk (a slow disk), also for the fetches at work.
    void setWriteDelayForTests(int ms);
    // For tests: the idle timeout (kIdleTimeoutMs), the free space reported for a destination, and
    // pauses of a worker before and after it settles how its fetch ends. They apply to the fetches
    // started afterwards.
    void setIdleTimeoutForTests(int ms) { m_idleTimeoutMs = ms; }
    void setFreeSpaceForTests(std::function<qint64(const QString &)> freeSpace) { m_freeSpace = std::move(freeSpace); }
    void setFinishPausesForTests(int beforeMs, int afterMs)
    {
        m_finishPauseBeforeMs = beforeMs;
        m_finishPauseAfterMs = afterMs;
    }
    // For tests: the most received bytes ever queued for a worker, and how many times reading was
    // paused for one.
    qint64 peakQueuedBytesForTests() const { return m_peakQueued; }
    int readPausesForTests() const { return m_readPauses; }
    // For tests: the writer threads of the fetches started from now on (0: chosen from the
    // manifest, see kParallelSmallFiles), and the most bytes ever queued for a fetch's writers.
    void setWriterThreadsForTests(int writers) { m_writerThreads = writers; }
    // For tests: the same for every FileTransfer that sets none (a whole test run with 1 or 8).
    static void setDefaultWriterThreadsForTests(int writers);
    qint64 peakWriterQueuedForTests() const;
    // For tests: how many writers the last fetch that started them has.
    int lastWriterCountForTests() const;
    // The writers a fetch would start for a manifest of these file sizes (kParallelSmallFiles;
    // `forced` > 0: setWriterThreadsForTests), without starting any.
    static int writerCountFor(const QList<qint64> &fileSizes, int forced = 0);
    // For tests: whether uploads read ahead (on by default; off: every file is read by the
    // upload's reader itself, as before the read-ahead).
    void setReadAheadForTests(bool on) { m_readAhead = on; }
    // For the benchmark: data frames sent (uploads) and received (fetches), the time writers spent
    // on their operations (summed over the writers) and the time fetches' workers waited for room
    // in their writers' queues, since this object was made; the files uploads took from their
    // read-ahead, and the time their readers waited for it.
    struct Counters {
        qint64 dataFramesSent = 0;
        qint64 dataFramesReceived = 0;
        qint64 writerBusyNs = 0;
        qint64 writerRoomWaitNs = 0;
        qint64 readAheadFiles = 0;
        qint64 readAheadWaitNs = 0;
    };
    Counters countersForTests() const;
    // For tests: the uploads whose reader thread has not returned yet.
    int liveUploadsForTests() const;
    // For tests: the manifest frames an upload of `offer` sends.
    static QList<QByteArray> manifestFramesForTests(const Offer &offer);

    static constexpr int kProgressIntervalMs = 250;

    // "a/../b", absolute paths and drive letters are rejected; returns empty if unsafe.
    static QString sanitizeRelativePath(const QString &path);

signals:
    // A fetch the user sees (see fetch): bytes received of the total announced, and files
    // completed of the files announced; target: the folder it writes into (fetch's targetDir).
    void transferProgress(quint64 handle, const QByteArray &peerId, const QString &kind, qint64 done, qint64 total,
                          int files, int fileCount, const QString &target);
    // state: "done", "failed" or "cancelled" (cancel()); error: why it failed; target as above.
    void transferFinished(quint64 handle, const QByteArray &peerId, const QString &kind, const QString &state,
                          const QString &error, const QString &target);

private:
    struct Upload;      // an upload as this thread sees it: the stream
    struct UploadJob;   // what an upload shares with its reader thread
    struct Download;    // a fetch as this thread sees it: the stream, the idle timer
    struct DownloadJob; // what a fetch shares with its worker thread
    struct Shared;      // what the workers share with this object, which may go first
    struct ListFetch;   // a list fetch (fetchList): the stream, the manifest so far
    void onStreamAccepted(SecureStream *stream);
    void drainUpload(quint64 uploadHandle);
    void onReceived(quint64 handle, const QByteArray &frame);
    void maybeSendGet(Download *d);
    void onJobReady(quint64 handle);
    void onJobBeat(quint64 handle);
    void onJobResume(quint64 handle);
    void onJobProgress(quint64 handle);
    void onJobFinished(const std::shared_ptr<DownloadJob> &job, FetchResult result);
    // Runs the job's FetchOptions::cleanupAsUser on the pool, as the user the job recorded.
    void startCleanup(const std::shared_ptr<DownloadJob> &job);
    void reportProgress(Download *d, bool force);
    // Removes the fetch, closes its stream, reports its end and runs its callback.
    void endDownload(quint64 handle, const FetchResult &result, const QString &state);
    void onListFrame(quint64 handle, const QByteArray &frame);
    // The peer's name for an error ("The other device" when it has none).
    QString peerName(const QByteArray &peerId) const;
    // Removes the list fetch, drops its stream and runs its callback (if any).
    void endList(quint64 handle, bool ok, bool refused, const QString &error, const QList<ListedFile> &items);

    Node *m_node;
    Impersonator m_impersonate;
    std::function<QString()> m_identity;
    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<detail::TaskPool> m_pool;
    std::map<quint64, Offer> m_offers;
    std::map<quint64, Upload *> m_uploads;
    std::map<quint64, Download *> m_downloads;
    std::map<quint64, ListFetch *> m_lists;
    quint64 m_nextHandle = 1;
    int m_idleTimeoutMs = kIdleTimeoutMs;
    std::function<qint64(const QString &)> m_freeSpace;
    int m_finishPauseBeforeMs = 0;
    int m_finishPauseAfterMs = 0;
    qint64 m_peakQueued = 0;
    int m_readPauses = 0;
    int m_writerThreads = 0;
    bool m_readAhead = true;
};

} // namespace df
