#pragma once

// Who gets the clipboard. Trusted devices (Device or Group trust, both ways as each side sees it)
// share one clipboard: a copy is announced at once to every connected device this one trusts and
// that runs kSharedClipboardVersion or later, and a device takes a clipboard from any device it
// trusts whenever it is newer (last writer wins, see below). So two devices used each with its own
// keyboard, or with nobody sharing a keyboard and mouse, still copy on one and paste on the other.
// A device that (re)connects is sent the current clipboard as well. Guests, and older peers, get
// the clipboard only the way it always travelled: following the shared cursor.
//
// Hand-off (guests and older peers): the master (whose keyboard and mouse are shared) sees every
// crossing from X to Y. If X is the master itself it sends its clipboard to Y; otherwise it tells Y
// to expect X's clipboard (clipFrom {s: X}) and X to send it to Y (clipTo {d: Y}). X obeys only the
// master, and only when the cursor has just left it. A clipboard from a device this one treats as a
// guest is taken only from the master whose cursor is on this device (in.enter) or from the device
// a pending hand-off told it to expect; nothing a guest sends otherwise replaces the clipboard.
//
// Sealing: each copy is sealed with a fresh 32-byte key under a random 16-byte id (kid):
// crypto::seal (XChaCha20-Poly1305, ad = kid || mime). What travels is the ciphertext. A trusted
// device is sent the key with it (clip {k}): when the user pastes there, the content is simply
// decrypted and handed over, with no further question, and an application reading the clipboard
// (a clipboard manager, the system history) is served too. A guest never gets the key with the
// content: the device that sealed escrows it at the master (clip.key, sent again when the master
// changes or reconnects, masterChanged), and the guest asks the master for it when the user
// pastes (see below). A device that holds ciphertext without the key (it came through a guest, or
// from a 0.1.21 peer) asks the master too, and the master grants a trusted device at once.
//
// Paste grant for guests (the master decides, clip.unseal / clip.unsealed): only with an explicit
// paste chord the master forwarded to that guest (Ctrl+V, Shift+Insert, Ctrl+Shift+V, Cmd+V on a
// Mac) within kChordWindowMs; a context-menu paste cannot grant it. The master also keeps track of
// which clipboard each device holds (the author's clip.key, and every hand-off it orchestrated): a
// guest is granted only the clipboard the cursor brought it, never one it relayed earlier. The
// first grant for a kid latches for kGrantLatchMs (applications ask several times per paste), at
// the master and in the guest's own plaintext cache: once the latch is over the plaintext is wiped
// and the next read needs a fresh grant. A guest refuses to even ask the master unless a key or
// button press from the master reached it just now (kInputWindowMs; for copied files, the paste
// rule of every device, see fetchFiles), so a clipboard manager reading on arrival costs nothing
// and the content never enters its history. The owner of the local clipboard is told again
// whether reads need the grant whenever that changes (another master, its declared trust
// arriving, a key kept), so a guest's object is never left ungated nor a trusted device's gated.
//
// Content: formats ("text/plain" and "text/html" in UTF-8, "image/png") and copied files/folders.
// The announcement goes over the reliable UDP channel and carries small sealed formats inline:
//   clip {v: stamp, a: author id, kid, k: key (trusted peers only), sf: {mime: ciphertext},
//         big: {mime: ciphertext size}, fb: file bytes, fc: file count, dc: folder count,
//         o: sealed-blob offer id, fo: copied-files offer id, fa: true when the recipient was let
//         into fo, bc: true for a copy announced to every trusted device (not a hand-off),
//         fl: [[relative path, size], ...] (up to kMaxListedFiles entries; a folder is
//         [relative path, -1]), sp: true when every relative path of the copied files and folders
//         is shorter than 260 UTF-16 units (sent only then; 0.1.30)}
// Folders (the copied ones and every subfolder, so empty ones survive) are counted apart (dc) and
// listed only to peers that understand them (kFoldersVersion); an older peer gets fc and the files.
// Large sealed formats are fetched right away by each receiver over an encrypted TCP stream
// (FileTransfer) from the device that announced them. Copied files are not: copying only announces
// them (count and size, from the file system's metadata), and they are transferred when the user
// pastes them, with a key or button pressed just before (fetchFiles: a clipboard reader that is
// not a paste moves nothing, on a trusted device too; a transfer the user sees with its
// progress), into
// <downloads>/Clipboard/<stamp>-<n>/ (n: a counter of this process started at its start time, so that
// every transfer gets a fresh folder, also across restarts). A copy too large to list in the announcement is listed on demand (listFiles: a
// list-only fetch from the publisher, once per clipboard), which moves names and sizes, never
// contents. A trusted device is let into the files' offer with the
// announcement (fa); a guest only on a paste grant (the master asks the publisher, clip.allow). A
// device hands on what it adopted with its own offer: the ciphertext, not files it has not
// fetched. A guest is not even sent the file names (nor sp), only their count and size.
//
// Pasting copied files (0.1.30): file data moves only for a paste, on every device, with a key or
// button pressed at this device just before (fetchFiles), or when the user takes them explicitly
// after a refusal (Receive: filesWaiting, receiveFiles). An owner that can paste files while they
// arrive (the Windows agent's virtual files, told so with clipSet {stream}, filesStreamable) asks
// for the list first (listFiles, no paste needed), then for a streaming transfer: it is given the
// folder the files arrive in at once and reads them as they are written; that folder is kept
// while it reads, and its transfer outlives a newer clipboard (detached) but not reset(). Every
// transfer goes into a fresh folder; the last kKeptDownloads are kept.
//
// Move paste (0.1.31, the Windows agent's large pastes, told so with clipSet {move},
// filesMovable): the paste moves the files out of the folder they were transferred into, so it
// asks for a fresh transfer each time (fetchFiles fresh) and, once they were moved, says so
// (filesConsumed): the folder's remains go and the clipboard is "not transferred" again.
//
// Older peers: a peer older than kSealingVersion is sent the clipboard as 0.1.19 understands it,
// as plaintext (clip {f: {mime: bytes}, big, fb, fc, fl, o}, one offer with the large formats and
// the files, open to that peer), at a hand-off, provided this device holds the key and the peer is
// not a guest; otherwise the user is told it could not be delivered. Its plaintext clipboard is
// understood and re-sealed here.
//
// Last-writer-wins on a logical clock: a stamp is max(wall-clock ms, the newest stamp seen + 1), so
// a copy made after seeing another one always wins over it even when the clocks of the devices
// disagree; a stamp more than kMaxClockAheadMs ahead of this device's clock is refused (and the
// user told). Not persisted: the clipboard often holds passwords. Plaintext is cached in memory
// only, until the next clipboard change (a guest's granted plaintext only while the latch holds).


#include "crypto.h"
#include "filetransfer.h"
#include "settings.h"
#include "workspace.h"

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <optional>
#include <memory>
#include <vector>

namespace df {

class Node;

using ClipboardData = QMap<QString, QByteArray>;

class ClipboardSync : public QObject
{
    Q_OBJECT
public:
    static constexpr qint64 kInlineBytes = 64 * 1024;             // per format (ciphertext), sent with the announcement
    static constexpr qint64 kMaxFormatBytes = 64 * 1024 * 1024;   // per format
    // Copied files: a sanity bound on what one copy may announce (the receiving device also
    // checks its free disk space before writing anything), and on files and on folders apiece.
    static constexpr qint64 kMaxFileBytes = 1024LL * 1024 * 1024 * 1024; // 1 TB, in total
    static constexpr int kMaxFiles = 100000;
    static constexpr int kKeptDownloads = 3;
    // Copied files are listed (names and sizes) in the announcement up to this many, so a paste can
    // show them before transferring anything.
    static constexpr int kMaxListedFiles = 2000;
    // A paste that shows copied files before their contents arrive (Explorer's virtual files)
    // takes at most this many files and folders, each relative path at most kMaxVirtualPath
    // UTF-16 units long (MAX_PATH - 1): beyond either, the files are pasted once transferred.
    static constexpr int kMaxVirtualEntries = 50000;
    static constexpr int kMaxVirtualPath = 259;

    using ListedFile = df::ListedFile; // a file or a folder (dir)
    // A notice of one kind is repeated at most this often (a clipboard manager or a pasting
    // application may ask many times in a row).
    static constexpr qint64 kNoticeIntervalMs = 10000;

    // How long after the cursor left a device it still hands its clipboard on.
    static constexpr qint64 kHandOffGraceMs = 2000;
    // How long a device waits for the clipboard it was told to expect.
    static constexpr qint64 kExpectMs = 10000;

    // Guests only (the master decides): a paste chord forwarded to the guest this recently grants
    // its paste; a guest asks the master only if a press from the master reached it this recently.
    // Also the window of the paste evidence for file data (see fetchFiles).
    static constexpr qint64 kChordWindowMs = 2000;
    static constexpr qint64 kInputWindowMs = 1000;
    // A guest's grant latches this long (applications ask several times per paste).
    static constexpr qint64 kGrantLatchMs = 5000;
    // Paste evidence for file data (fetchFiles): a request with no gesture yet waits this long for
    // one still on its way (the helper's localPress and the paste's own request travel on
    // different connections), and a list served this recently before the request makes the
    // window count from when that list was asked for.
    static constexpr int kEvidenceGraceMs = 300;
    static constexpr qint64 kListReferenceMs = 5000;
    // A reader's own list read (fetchFiles' listedAgoMs) counts as the paste's reference for at
    // most this long: Explorer creates the folders of a tree of up to 50,000 entries before it
    // reads the first file.
    static constexpr qint64 kDescriptorReferenceMs = 120 * 1000;
    // A request refused for want of a paste (NoPaste) is remembered this long: the same clipboard
    // asked for again with nothing pressed since is refused at once, with no grace (a reader that
    // asks once per item would otherwise be held kEvidenceGraceMs per item). Each such refusal
    // renews it. Short, so that a real paste soon after a reader was refused (its press still on
    // its way) gets its grace.
    static constexpr qint64 kNoPasteMemoryMs = 500;
    // The master holds an unseal request this long for a key that has not arrived yet (the author's
    // clip.key and the asker's clip.unseal race on different channels), and at most this many.
    static constexpr int kKeyWaitMs = 500;
    static constexpr int kMaxHeldUnseals = 16;
    // A device waits this long for the master's answer before giving up (LAN round trips are ms).
    // The owner of the local clipboard waits longer (ipc::kClipboardRenderTimeoutMs).
    static constexpr int kUnsealTimeoutMs = 3000;
    // Escrowed keys kept at the master (the newest kid per author, a few authors).
    static constexpr int kEscrowKids = 4;
    // The first version that seals the clipboard: an older peer is sent plaintext (0.1.19 format).
    static constexpr const char *kSealingVersion = "0.1.20";
    // The first version that shares the clipboard among trusted devices on every copy (and takes
    // the key with the content): an older sealing peer gets it at hand-offs only.
    static constexpr const char *kSharedClipboardVersion = "0.1.20";
    // A stamp this far ahead of the local wall clock is refused (a broken clock would otherwise
    // win every comparison until it is fixed).
    static constexpr qint64 kMaxClockAheadMs = 24LL * 3600 * 1000;

    using FolderProvider = std::function<QString()>;
    // Wall-clock milliseconds since the epoch (injectable for tests).
    using WallClock = std::function<qint64()>;

    // Where the shared cursor is, from this device's point of view (InputController). Used for the
    // hand-off (guests and older peers) and for a guest's paste grant.
    struct Presence {
        std::function<QByteArray()> master;                     // device sharing its keyboard and mouse
        std::function<bool()> cursorArrived;                    // the master's cursor crossed in (in.enter)
        std::function<bool(qint64 graceMs)> cursorLeftRecently; // the cursor crossed away this recently
        // Master side, a guest's paste evidence: a paste chord was forwarded to `device` this
        // recently.
        std::function<bool(const QByteArray &device, qint64 windowMs)> pasteOrderedOn;
        // Guest side, pre-check before asking the master: a press from the master reached us this
        // recently (so a manager reading the clipboard on arrival does not trigger a request).
        std::function<bool(qint64 windowMs)> inputArrived;
        // Paste evidence for file data (InputController::lastPasteGestureAt): when the user last
        // made a paste gesture at this device (a paste chord, Enter, the Apps key, Shift+F10 or a
        // button press, forwarded by the master or pressed locally), in nowMs() milliseconds;
        // -1: never. Unset: never.
        std::function<qint64()> lastPasteGesture;
        // The local helper reports this device's own presses (InputController::localPressSupported).
        std::function<bool()> localPressSupported;
        // Whether a paste gesture happened between two moments (nowMs(), both included; the second
        // < 0: up to now), from a history of recent gestures (InputController::pasteGestureBetween).
        // Unset: only lastPasteGesture is looked at.
        std::function<bool(qint64 from, qint64 to)> pasteGestureBetween;
    };

    // downloads: where received files go (a "Clipboard" subfolder is created inside).
    ClipboardSync(Node *node, FileTransfer *transfer, Settings *settings, Presence presence, FolderProvider downloads,
                  QObject *parent = nullptr);
    ~ClipboardSync() override;

    // Why a paste of copied files got no files (FilesCallback): None (it got them), NoPaste (no
    // paste gesture: a reader that is not a paste, refused quietly), Cancelled (the user cancelled
    // the transfer), Changed (the clipboard is not the one asked for any more), Other (a failure).
    enum class FetchError { None, NoPaste, Cancelled, Changed, Other };
    // The name of a code for IPC (clipFetched.code): "noPaste", "cancelled", "changed", "other";
    // empty for None.
    static QString fetchErrorName(FetchError code);
    using FilesCallback =
        std::function<void(bool ok, const QStringList &files, const QString &error, FetchError code)>;
    using UnsealCallback = std::function<void(bool ok, const QByteArray &plain, const QString &error)>;
    using ListCallback = std::function<void(bool ok, const QList<ListedFile> &items, const QString &error)>;
    // A streaming paste (fetchFiles with streaming): the folder its files are being written into,
    // as soon as it is known (they appear there one after the other, each growing to its size).
    using StartedCallback = std::function<void(const QString &root)>;

    bool isEnabled() const;
    void setEnabled(bool enabled);
    void setWallClock(WallClock clock) { m_wallClock = std::move(clock); }

    // Forgets the current clipboard: content, keys, published offers and any in-flight fetch.
    // Called when the console user changes (the clipboard often holds the previous user's
    // passwords).
    void reset();

    // The local clipboard changed (reported by the session helper).
    void localChanged(const ClipboardData &formats, const QStringList &files = {});
    // Master side: the cursor went from `from` to `to` (InputController::cursorMoved).
    void cursorMoved(const QByteArray &from, const QByteArray &to);
    // The workspace master may have changed, or a peer (re)connected: the key of our own content
    // is escrowed again with the master it has not reached yet (Workspace::changed,
    // Node::peerConnected).
    void masterChanged();
    // The local clipboard owner (re)connected: emits applyLocally again for a clipboard adopted
    // from another device (it would be lost otherwise) and returns true; false when the local
    // clipboard holds nothing of ours to put back (it was copied here, something not shared was
    // copied here since, or there is none), so the owner should drop a clipboard object it may
    // still hold from before (a service restart).
    bool reapply();

    ClipboardData current() const { return m_current.formats; }
    QStringList currentFiles() const { return m_current.files; }
    // Deletes the oldest received copies in clipboardDir (folders "<stamp>-<n>", or "<stamp>" from
    // older versions, ordered by stamp then n) until `keep` remain, leaving alone the folders for
    // which `skip` is true (a transfer still writes there, or a paste still reads there): they
    // are not counted either. Also deletes what retireReceived set aside and nobody deleted (a
    // crash). Disk work: call it as the console user, off the main thread
    // (FileTransfer::runAsUser).
    static void pruneReceived(const QString &clipboardDir, int keep, const std::function<bool(const QString &)> &skip);
    // The quick half of pruneReceived, for a paste's transfer (FetchOptions::prepareAsUser): the
    // copies it would delete are renamed aside in clipboardDir (".removing-<name>-<random>": never
    // a received copy's name, left out of the ordering, of every listing of copies and of the
    // count kept). Returns what removeRetired must delete: those, the folders set aside earlier
    // and never deleted, and any copy that could not be renamed (a file open in it, on Windows),
    // under its own name. Same rules and same user as pruneReceived.
    static QStringList retireReceived(const QString &clipboardDir, int keep,
                                      const std::function<bool(const QString &)> &skip);
    // The slow half (FetchOptions::cleanupAsUser): deletes `paths` (fs::removeTree: never through
    // a link), a folder still under its own name only if `skip` is false for it by then, and never
    // one another prune is deleting at the same time: it waits for that one to finish instead, so
    // that when it returns the space is free (the transfer's free space check runs after it).
    static void removeRetired(const QStringList &paths, const std::function<bool(const QString &)> &skip);
    // Renames the folder `path` aside in its own folder (as retireReceived): the new path, or
    // empty when it could not be renamed.
    static QString retireFolder(const QString &path);
    // Whether reading the current clipboard here needs a paste grant: this device holds the
    // ciphertext without the key and the master treats it as a guest. The owner of the local
    // clipboard then refuses reads that do not come from a paste cheaply, without asking.
    bool needsPasteEvidence() const;
    // Plaintext of one format of the clipboard `kid` for a paste (clipUnseal): decrypted with the
    // key this device holds (its own copy, one a trusted device sent, the master's escrow), or by
    // asking the master for the key (a guest: with the evidence above). `done` runs on this thread,
    // once; a kid that is not the current clipboard's fails (a stale clipboard object renders
    // nothing).
    void unseal(const QByteArray &kid, const QString &mime, UnsealCallback done);
    // The files of the current clipboard as local paths, transferring them first from the device
    // they were copied on (when the user pastes; a guest after the master grants the paste).
    // `done` runs on this thread, once (possibly inside the call); a newer clipboard makes it fail
    // (Changed), and so does a non-empty `kid` that is not the current clipboard's.
    //
    // Paste rule: copied files move only for a paste, on every device (trusted ones, the master and
    // guests alike; a guest still needs the master's grant after it). Something must have been
    // pressed at this device (Presence::lastPasteGesture: a paste chord, Enter, the Apps key,
    // Shift+F10 or a mouse button; never motion) after the clipboard was applied here and at most
    // kChordWindowMs before the request (or before the list served just before it, see
    // kListReferenceMs). A request that says how long ago its reader read the list of the files
    // (`listedAgoMs`, the agent's streaming paste: Explorer reads the descriptor at the paste,
    // then creates every folder before it reads a file's contents, which takes a while for a large
    // tree) also passes with a press made around that read (within kChordWindowMs either side of
    // it, at most kDescriptorReferenceMs ago). The agent's word adds no right: a console process
    // can take the files with receiveFiles anyway; the rule keeps readers that are not pastes from
    // moving data. Without one the request waits kEvidenceGraceMs for a press still on its
    // way (pasteGestureNoted re-checks at once), then is refused quietly (NoPaste, an info log and
    // filesWaiting, no notice unless this device's own presses are not reported): a clipboard
    // reader that is not a paste moves nothing. A refusal is remembered kNoPasteMemoryMs: the same
    // clipboard asked for again with nothing pressed since is refused with no grace. Every apply
    // (the same clipboard applied again included) restarts the evidence. Requests that come while
    // one waits or transfers join it. Files already here are given at once.
    //
    // Waiters: every request that is not answered at once is a waiter with an id (returned; 0
    // when answered inside the call), which its requester drops when it goes away
    // (dropFilesWaiter). A streaming waiter (an owner that pastes the files while they arrive,
    // reading them from the transfer's folder) is given that folder (`started`) as soon as the
    // transfer starts, or at once when it already runs; the folder is then pinned (never pruned)
    // until the waiter is dropped, also after the transfer ended. The folder may not exist yet when
    // it is given: the transfer's worker creates it (after making room, which removes anything of
    // that name) once the transfer is under way. The waiter waits for it to appear and never
    // creates it; a transfer that fails before then ends with the waiter's answer as usual. A
    // streaming request for files this device already transferred is given their folder and the
    // files at once, pinned the same way (a nonzero id then too). When the last waiter of a
    // transfer under way goes and it was a streaming one, the transfer is cancelled (nobody reads
    // it any more); a plain waiter leaving cancels nothing, the files are kept for the next paste.
    //
    // A newer clipboard cancels the transfer of the current one, unless a streaming waiter reads
    // it: that transfer then goes on, detached, for its streaming waiters only (its plain ones are
    // answered Changed then, as without them; the current clipboard is not touched by its end), and
    // is cancelled when the last of them goes. reset() cancels every transfer, detached ones included.
    //
    // fresh (the Windows agent's move paste, filesMovable): the paste takes the files away from the
    // folder it is given (Explorer moves them), so files of another device transferred here before
    // are not handed out again: the clipboard goes back to "not transferred" and a new transfer,
    // into a new folder, starts as for the first paste (paste evidence included). Files copied at
    // this device are given as always.
    quint64 fetchFiles(FilesCallback done, const QByteArray &kid = {}, StartedCallback started = {},
                       bool streaming = false, qint64 listedAgoMs = -1, bool fresh = false);
    // The requester of waiter `id` went away (its connection closed). Unknown ids are ignored.
    void dropFilesWaiter(quint64 id);
    // The paste holding waiter `id` (a streaming one, after its transfer succeeded) moved the files
    // away from its folder (the agent's move paste, clipConsumed): its pin goes, the current
    // clipboard goes back to "not transferred" if that folder held its files (the next paste
    // transfers again), and what is left in the folder (an empty folder, files the paste skipped)
    // is removed as the console user, unless another pin or a transfer at work still uses it. Only
    // a folder directly inside the received copies' folder is ever removed. Unknown ids are
    // ignored.
    void filesConsumed(quint64 id);
    // Whether the current clipboard's files can be pasted by moving them (the agent's move paste,
    // told so with clipSet {move}): another device's files, still to transfer, that this device
    // was let into, with no paste grant needed for reads (never a guest's).
    bool filesMovable() const;
    // The bytes of the files still to transfer (0 when none).
    qint64 pendingBytes() const { return pendingItems() > 0 ? m_current.pendingBytes : 0; }
    // The folders of transfers read by streaming waiters still there (see fetchFiles).
    QStringList pinnedTargets() const;
    // The user takes the copied files of the clipboard `kid` explicitly (Receive, after a paste was
    // refused for want of a gesture: filesWaiting): transferred with no paste evidence, only for
    // the current clipboard and only when its publisher let this device in already (never a
    // guest's paste, which needs the master's grant). Joins a request under way (one waiting for a
    // gesture goes on at once). Once transferred the clipboard is applied again with the local
    // files, so that an application that refused to paste before sees them. `done` as fetchFiles.
    void receiveFiles(const QByteArray &kid, FilesCallback done);
    // A paste gesture happened at this device (InputController::pasteGesture): a request waiting
    // for one is checked again at once.
    void pasteGestureNoted();
    // Names and sizes of the files and folders waiting to be pasted (empty if not listed).
    QList<ListedFile> pendingFileList() const { return pendingItems() > 0 ? m_current.fileList : QList<ListedFile>(); }
    // Names and sizes of the files and folders of the clipboard `kid` copied on another device,
    // contents not included (a file manager reading what a paste will bring): the announced list
    // when there is one, otherwise a list-only fetch from the publisher (FileTransfer::fetchList),
    // made once per clipboard (requests meanwhile join it) and kept until the clipboard changes.
    // Needs no paste: only that the publisher let this device in (never a guest). Never for this
    // device's own copy, nor fetched for a copy not announced as streamable (no sp, or more than
    // kMaxVirtualEntries): refused at once, with no network. A list beyond kMaxVirtualEntries or
    // kMaxVirtualPath, not what was announced, or whose manifest the transfer refused (paths that
    // differ only in case here, for instance) is refused, and the clipboard is applied again
    // without the streaming paste (filesStreamable). `done` runs on this thread, once (possibly
    // inside the call).
    void listFiles(const QByteArray &kid, ListCallback done);
    // Whether the files of the current clipboard can be pasted before their contents arrive: they
    // are still on the device they were copied on, it let this device in, reads need no paste
    // grant, and they are listed or announced within kMaxVirtualEntries entries with short paths
    // (sp), and no list of theirs was refused.
    bool filesStreamable() const;

signals:
    // The local clipboard should offer this content, rendered lazily: the formats present (mime ->
    // plaintext size) and their id (kid), the copied local files, pendingItems (files and folders)
    // copied on another device and transferred when pasted (fetchFiles), and whether a read needs a
    // paste grant (needsPasteEvidence).
    void applyLocally(const QByteArray &kid, const QVariantMap &formats, const QStringList &files, int pendingItems,
                      bool gated);
    void enabledChanged(bool enabled);
    // What happens to copied files, for the user (shared, not shared, received, failed).
    void filesNotice(const QString &text, bool problem);
    // A paste of the copied files of the clipboard `kid` (count files and folders, copied on the
    // device named `from`) was refused for want of a paste gesture: the user may still take them
    // explicitly (clipWaiting). Once per clipboard. count 0: that offer is over (the clipboard
    // changed, or the files are here).
    void filesWaiting(const QByteArray &kid, int count, const QString &from);
    // Anything else the user should know about the clipboard: a refusal or failure that would
    // otherwise only be logged (a clipboard that could not be delivered to a peer, an older master,
    // a paste refused or failed, a stale clipboard, content too big to share...). Repeated at most
    // every kNoticeIntervalMs per kind.
    void notice(const QString &text, bool problem);

private:
    struct Content {
        Stamp stamp;
        QByteArray kid;
        // The key, when this device holds it: it sealed the content (sealedHere), a trusted device
        // sent it, or the master granted it to keep. Wiped on replacement.
        QByteArray key;
        bool sealedHere = false;          // we sealed it: we escrow its key with the master
        ClipboardData formats;            // our own plaintext (echo check); empty when adopted
        QMap<QString, QByteArray> sealed; // ciphertext per mime (what travels and is re-published)
        QMap<QString, qint64> plainSizes; // plaintext size per mime (for the lazy render)
        QStringList files;
        Offer ownFiles;          // our own copied files as offered (also served to older peers)
        quint64 offer = 0;       // sealed-blob offer (gated; recipients are let in when sent to)
        quint64 fileOffer = 0;   // copied-files offer (gated; trusted recipients let in when sent to)
        quint64 legacyOffer = 0; // plaintext formats and files for older peers (gated, made once)
        qint64 fileBytes = 0;
        int fileCount = 0;
        int dirCount = 0;
        QList<ListedFile> fileList; // own files: what we offer; another device's: what it offers
        // Files and folders copied on another device and not transferred yet.
        QByteArray filesFrom;
        quint64 remoteFileOffer = 0;
        bool filesAllowed = false; // the publisher let us into remoteFileOffer with the announcement
        int pendingFiles = 0;
        int pendingDirs = 0;
        qint64 pendingBytes = 0;
        // Every relative path is shorter than kMaxVirtualPath + 1 (sp: computed for our own copy,
        // as announced for another device's).
        bool shortPaths = false;
        // A list of these files broke the bounds of a streaming paste or did not match the
        // announcement (listFiles): they are pasted once transferred.
        bool listRefused = false;
        // Another device's files once transferred here: the folder they were written into (a
        // streaming paste asking again is given it), and how many files and folders they were (a
        // move paste that consumed them makes them pending again, see fetchFiles).
        QString stagedRoot;
        int stagedFiles = 0;
        int stagedDirs = 0;
        // Plaintext decrypted for a paste: until the next clipboard change, or (a guest's grant,
        // no key kept) only while the grant latch holds (grantedAt).
        QMap<QString, QByteArray> plainCache;
        qint64 grantedAt = -1;
    };

    struct Escrow {
        QByteArray key;
        QByteArray author;
        Stamp stamp;
    };
    // Master side: the clipboard a device holds, as far as the master knows (see noteHeld).
    struct Held {
        QByteArray kid;
        Stamp stamp;
    };
    struct FormatWaiter {
        QString mime;
        UnsealCallback done;
    };
    // Master side: an unseal request whose key has not arrived yet, held for kKeyWaitMs.
    struct HeldUnseal {
        QByteArray peer;
        QCborMap msg;
        qint64 at = 0;
    };
    struct PendingAllow {
        QByteArray peer;      // the device waiting to unseal (the master itself for its own paste)
        quint64 rid = 0;      // its request id
        QByteArray key;       // the key to hand it once the publisher allows the fetch
        QByteArray publisher; // the device asked to let it in (only its clip.allowed counts)
        bool keep = false;    // the device may keep the key (it is not a guest)
    };

    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    void onClip(const QByteArray &peerId, const QCborMap &msg);
    // An older peer's protocol: plaintext inline (f), larger formats and copied files in one
    // ungated offer (big, fc, fb, fl, o). The plaintext is re-sealed here (adoptLegacy).
    void onLegacyClip(const QByteArray &peerId, const QCborMap &msg, const Stamp &stamp);
    void adoptLegacy(Content content, ClipboardData plain, const QByteArray &from);
    void adopt(Content content, const QByteArray &from);
    // Seals the plaintext formats into `content` under a fresh key (kept in content->key).
    void sealContent(Content *content);
    // Escrows the current content's key with the master (locally when we are the master), once
    // per master: m_keyEscrowedWith remembers which master has it. Only what we sealed.
    void escrowCurrentKey();
    // Whether a peer declared at least `version` / declared one older than kSealingVersion
    // (neither until its hello arrived).
    bool peerRunsAtLeast(const QByteArray &peerId, const char *version) const;
    bool peerKnownOlder(const QByteArray &peerId) const;
    QString peerVersion(const QByteArray &peerId) const; // empty until its hello arrived
    // A trusted device that is sent every copy (see the top of this file).
    bool sharesEveryCopy(const QByteArray &peerId) const;
    // Publishes sealed blobs and copied files for peers to fetch; fills offers, fileBytes, etc.
    void publish(Content *content);
    void publishBlobs(Content *content);
    // The copied files as listed (ok) or refused (error): offered, and the user told.
    void publishFiles(Content *content, Offer files, bool ok, const QString &error);
    // A local copy, once its files (if any) are listed: it becomes the current clipboard and is
    // announced to the trusted devices.
    void finishLocalChange(Content content);
    // Drops the copy being listed, if any (superseded by another copy, here or adopted).
    void cancelWalk();
    // The newest of the current content and the copy being listed: an incoming clipboard must be
    // newer than both to be taken.
    Stamp latestStamp() const;
    QCborMap announcement() const;
    // Sends the current clipboard to `peerId` (a hand-off, or `everyCopy`: a copy announced to
    // every trusted device), and lets it fetch what it may.
    void sendTo(const QByteArray &peerId, bool everyCopy = false);
    // The same for a peer older than kSealingVersion: plaintext, as 0.1.19 understands it.
    void sendLegacy(const QByteArray &peerId);
    // Announces the current clipboard to every trusted device that shares every copy.
    void sendToTrusted();
    void onPeersChanged();
    // keepEvidence: the same clipboard applied again keeps the time paste evidence counts from
    // (only for a re-apply caused by a paste under way); otherwise it counts from now.
    void emitApply(bool keepEvidence = false);
    // Emits applyLocally again when whether a read needs a paste grant changed since the content
    // was applied (the master changed, its declared trust arrived, or the master let us keep the
    // key): the owner's object would otherwise gate a trusted device or serve a guest freely.
    // keepEvidence: as for emitApply.
    void refreshGate(bool keepEvidence = false);
    void wipeContentKeys(Content *content);
    // Makes `content` the current clipboard: the old one's offers and keys go, and whatever was
    // waiting for it fails quietly (replaced: nothing went wrong).
    void replaceCurrent(Content content);
    // Logical clock: the newest plausible stamp seen, and the stamp for a new local copy.
    void noteStamp(quint64 version);
    quint64 nextStamp() const;
    qint64 wallNow() const { return m_wallClock ? m_wallClock() : QDateTime::currentMSecsSinceEpoch(); }

    // Master side.
    void escrowKey(const QByteArray &kid, const QByteArray &key, const QByteArray &author, const Stamp &stamp);
    // The key of `kid` the master can hand out: escrowed, or the current clipboard's own.
    QByteArray keyFor(const QByteArray &kid) const;
    // Records that `device` holds the clipboard `kid` (if newer than what it was known to hold):
    // the author's clip.key, and each hand-off the master orchestrates (cursorMoved).
    void noteHeld(const QByteArray &device, const QByteArray &kid, const Stamp &stamp);
    void handleUnsealRequest(const QByteArray &peer, const QCborMap &msg);
    // Refuses the held unseal requests older than kKeyWaitMs ("Copy again").
    void expireHeldUnseals();
    // A guest's paste: the chord, and only the clipboard it holds.
    bool guestMayUnseal(const QByteArray &kid, const QByteArray &guest);
    void grantLatched(const QByteArray &kid, const QByteArray &requester);
    void answerUnseal(const QByteArray &peer, quint64 rid, bool ok, const QByteArray &key, const QString &reason,
                      bool keep = false);

    // Requesting side.
    void requestUnseal();
    void requestFilesGrant();
    void onUnsealed(const QByteArray &key, bool keep);
    // tell: also tell the user (not when a newer clipboard simply replaced the one being pasted).
    void failFormatWaiters(const QString &error, bool tell = true);
    void serveFormatWaiters();
    // Answers a paste that fails here and tells the user (no silent empty paste).
    void failPaste(const UnsealCallback &done, const QString &error);
    // Keeps a key only if it opens the content's sealed formats (a wrong one would make every
    // paste fail; without one the master is asked).
    bool keyOpens(const Content &content, const QByteArray &key) const;
    void applyKey(const QByteArray &key); // decrypt every sealed format into the cache
    // Whether a paste gesture counts for a request made at `ref` (nowMs): one at this device
    // since the current clipboard was applied here and at most kChordWindowMs before `ref` (and,
    // with `latest` >= 0, not after `latest`).
    bool hasPasteEvidence(qint64 ref, qint64 latest = -1) const;
    // Whether the pending files request (m_job) has its evidence: around its reference
    // (m_evidenceRef), or around its reader's own list read (m_descriptorRef, see fetchFiles).
    bool jobHasPasteEvidence() const;
    // The reference time of a files request made now: when the list served just before it was
    // asked for (within kListReferenceMs), otherwise now.
    qint64 evidenceReference(qint64 now) const;
    // A files request with paste evidence (or one waiting for it that got it): on through the
    // trusted, master and guest paths.
    void proceedFetchFiles();
    // The waiting request's grace ended without a gesture: refused quietly.
    void refuseNoPaste();
    void doFetchFiles();
    // Answers the waiters of the current clipboard's files request (not transferring yet) and
    // drops it.
    void failFilesWaiters(const QString &error, bool tell = true, FetchError code = FetchError::Other);
    struct FilesJob;
    // The transfer of `job` ended (its fetch callback).
    void onFilesFetched(const std::shared_ptr<FilesJob> &job, const FetchResult &result);
    // Ends `job` now: its transfer is cancelled, its waiters (if any) told `code`.
    void cancelFilesJob(const std::shared_ptr<FilesJob> &job, FetchError code);
    // Whether `id` is a waiter still waiting, or holds a pin.
    bool hasFilesWaiter(quint64 id) const;
    // Gives a streaming waiter the folder of its transfer and pins it.
    void startWaiter(quint64 id, const QString &root, const StartedCallback &started);
    // The prune check of a transfer's folders: a transfer at work there, or a streaming paste
    // reading there (the pins as they are now: a folder pinned later is a newer one).
    std::function<bool(const QString &)> pruneSkip() const;
    // Answers the list requests waiting for the current clipboard's list fetch.
    void answerListWaiters(bool ok, const QList<ListedFile> &items, const QString &error);
    // Why a list of copied items cannot back a streaming paste (empty: it can). `files`/`dirs`:
    // the counts it must match (-1: not checked).
    static QString listViolation(const QList<ListedFile> &list, int files, int dirs);
    // Drops the current clipboard's list and any list fetch in progress (its waiters fail quietly).
    void dropList();
    // The current clipboard's files transferred here (another device's) are gone from where they
    // were written (a move paste took them): back to "not transferred", so the next paste
    // transfers again. Nothing on disk is touched.
    void unstage();

    // pruneReceived(clipboardDir, kKeptDownloads, the transfers at work) on a FileTransfer thread.
    void pruneLater(const QString &clipboardDir) const;
    // The current clipboard is replaced: its files request ends (a transfer read by a streaming
    // waiter is detached instead, see fetchFiles).
    void cancelFileFetch();
    // The Receive offered for the current clipboard's files (filesWaiting) is over: told with a
    // count of 0. Nothing when none was offered.
    void endFilesWaiting();
    QString peerName(const QByteArray &peerId) const;
    bool isMaster() const;
    QByteArray master() const { return m_presence.master ? m_presence.master() : QByteArray(); }
    int pendingItems() const { return m_current.pendingFiles + m_current.pendingDirs; }
    // Whether `peerId` can send this device files and large formats (FileTransfer: the same
    // transfer protocol). If not, and the announcement holds files or such formats, the user is
    // told (once in a while) which device to update.
    bool transfersPossible(const QByteArray &peerId, bool files, bool formats);
    // Emits notice(text, problem) unless one of the same kind went out less than intervalMs ago.
    void noticeOnce(const QString &kind, const QString &text, bool problem, qint64 intervalMs = kNoticeIntervalMs);
    // The file list of a clip announcement (fl, fc, dc), validated: whether it lists exactly the
    // announced files and folders.
    static bool parseListing(const QCborMap &msg, qint64 fileCount, qint64 dirCount, QList<ListedFile> *out);

    Node *m_node;
    FileTransfer *m_transfer;
    FolderProvider m_downloads;
    Settings *m_settings;
    Presence m_presence;
    WallClock m_wallClock;
    Content m_current;
    quint64 m_clock = 0;          // the newest plausible stamp seen (logical clock)
    QByteArray m_keyEscrowedWith; // the master that holds m_current.key (empty: none yet)
    QSet<QByteArray> m_synced;    // connected trusted peers that were sent the clipboard on connecting
    // The local clipboard changed after the current content was adopted from another device,
    // to something not shared (an unsupported format, too large, sharing off): the adopted
    // content must not be put back over it (reapply).
    bool m_localNewer = false;
    // What the owner of the local clipboard was last told (emitApply): whether anything, and
    // whether reads need a paste grant (refreshGate).
    bool m_applied = false;
    bool m_appliedGated = false;
    // The clipboard last applied here (its stamp) and when its object was last put on the local
    // clipboard (nowMs): paste evidence counts only from then on. Moved by every apply, the same
    // clipboard applied again included (refreshGate, reapply, a refused list), except one caused
    // by a paste under way (emitApply keepEvidence).
    Stamp m_appliedStamp;
    qint64 m_appliedAt = -1;
    // The device whose clipboard the master told us to expect, and when.
    QByteArray m_expectFrom;
    qint64 m_expectAt = 0;
    // In-flight fetch for an announcement not applied yet.
    Stamp m_fetchingStamp;
    quint64 m_fetchHandle = 0;
    // A files request and who waits for it: m_job for the current clipboard (from the first
    // request on: while it waits for a paste gesture, m_evidenceTimer runs from m_evidenceRef;
    // then the grant round trips, m_filesRid / m_ownAllowArid; then the transfer, handle), and the
    // transfers of replaced clipboards that streaming waiters still read (m_detachedJobs).
    struct Waiter {
        FilesCallback done;
        StartedCallback started;
        bool streaming = false;
    };
    struct FilesJob {
        Stamp stamp;
        QByteArray kid;
        QByteArray peer;    // the device the files come from
        quint64 handle = 0; // the transfer (0: not started yet)
        QString target;     // its folder, once started
        bool done = false;
        bool ok = false;
        // Why it was ended from here (cancelFilesJob), told to the waiters left.
        FetchError abortCode = FetchError::None;
        // The user asked for it (receiveFiles): the clipboard is applied again once it is here.
        bool applyWhenDone = false;
        // A move paste waits for it (a fresh request): the files are given away to be moved.
        bool moveWaiter = false;
        QMap<quint64, Waiter> waiters;
    };
    std::shared_ptr<FilesJob> m_job;
    // What a move paste's request unstaged (fetchFiles with fresh): put back if that request is
    // refused for want of a paste (refuseNoPaste), dropped once a transfer goes ahead.
    struct MoveUnstaged {
        QByteArray kid;
        QStringList files;
        QString root;
    };
    std::optional<MoveUnstaged> m_moveUnstaged;
    std::vector<std::shared_ptr<FilesJob>> m_detachedJobs;
    quint64 m_nextWaiter = 1;
    QHash<quint64, QString> m_pins; // waiter id -> the folder it reads (see fetchFiles)
    QTimer m_evidenceTimer;
    qint64 m_evidenceRef = -1;
    qint64 m_descriptorRef = -1; // when the request's reader read the list (nowMs), -1: not said
    QByteArray m_waitingToldKid; // the clipboard filesWaiting was emitted for (empty once that offer ended)
    // The clipboard whose files request was last refused for want of a paste, and when (nowMs).
    QByteArray m_noPasteKid;
    qint64 m_noPasteAt = -1;
    // The list of the current clipboard's copied files fetched from their publisher (listFiles),
    // kept apart from Content::fileList (what the announcement listed, and what this device would
    // announce), the fetch in progress and who waits for it. askedAt/answeredAt (ms, nowMs): when
    // the list last served was asked for and given, so a paste right after a list counts its
    // window from the request (the list's network time does not eat it). Set by successful answers
    // only; a request that joined a fetch in progress counts from its own ask (the latest of them).
    struct ListWaiter {
        ListCallback done;
        qint64 askedAt = -1;
    };
    struct FileListState {
        Stamp stamp;
        QByteArray kid;
        bool have = false;
        QList<ListedFile> items;
        quint64 handle = 0;
        QList<ListWaiter> waiters;
        qint64 askedAt = -1;
        qint64 answeredAt = -1;
    };
    FileListState m_list;

    QHash<QString, qint64> m_noticeAt; // notice kind -> nowMs it last went out (noticeOnce)

    // Master side: escrowed keys (oldest first), held unseal requests, pending file allowances,
    // and per (kid, device) grant latches.
    QList<QByteArray> m_escrowOrder;
    QHash<QByteArray, Escrow> m_escrow;
    QHash<QByteArray, Held> m_holds;        // device -> the clipboard it holds (see noteHeld)
    QHash<QByteArray, QByteArray> m_lastFrom; // device -> the device the cursor last came to it from
    QList<HeldUnseal> m_heldUnseals;
    QHash<quint64, PendingAllow> m_pendingAllows;
    QHash<QByteArray, qint64> m_grantUntil; // key: kid + device -> granted until (ms)
    quint64 m_nextArid = 1;
    quint64 m_ownAllowArid = 0; // the master's own files paste waiting for the publisher's clip.allowed

    // Requesting side: an in-flight key request for the current clipboard, its waiters and timer.
    // Request ids start at 1, and 0 means none: an answer without an id matches nothing.
    quint64 m_formatRid = 0;
    QList<FormatWaiter> m_formatWaiters;
    QTimer m_formatTimer;

    // A local copy of files whose folders are being walked on a worker thread (a large folder
    // takes a while, and this thread also routes the keyboard and mouse): it is published when
    // the walk ends, unless a newer copy (local or from another device) came meanwhile.
    struct Walk;
    std::vector<std::shared_ptr<Walk>> m_walks;
    quint64 m_walkSeq = 0; // the walk in progress for m_walking (0: none)
    quint64 m_walkCounter = 0;
    Content m_walking;
    // The device the clipboard was last handed off to while a walk was in progress (the hand-off
    // carried the previous clipboard): it is handed the new one once the walk ends. Ours:
    // the hand-off was ours as the master (cursorMoved), else a clipTo from the master.
    QByteArray m_walkHandOff;
    bool m_walkHandOffOurs = false;
    quint64 m_filesRid = 0;
    QTimer m_filesGrantTimer;
    quint64 m_nextRid = 1;
};

} // namespace df
