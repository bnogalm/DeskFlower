#pragma once

// Windows: files that live on another device, offered as virtual files (FileGroupDescriptorW with
// names, sizes and folders, FileContents with one stream per file). Reading the list costs nothing;
// the first contents request calls `fetch`, which transfers the files and returns their local
// top-level items, and every file is then served as a stream of the local copy. Data the shell sets
// on it (drag image, drop description...) is kept and returned. Used for pasting (clipboard) and
// for dragging files from another device.
//
// A paste with ClipboardBridge::FileStreaming (a service that can stream) instead opens a paste
// session at the first contents request and serves each file while it arrives: a stream that
// lives in the agent's multithreaded apartment (streamhost_win.h), whose Read waits for the bytes
// asked for, so Explorer copies with its own progress and Cancel while the transfer runs and
// nothing ever blocks the agent's thread. A small file (at most 1 MiB) that is already complete
// when it is asked for, by a reader that takes an HGLOBAL as well as a stream (Explorer asks for
// HGLOBAL | ISTREAM | ISTORAGE), is served in memory instead (read at once, never waited for).
// FileContents is still offered, and must be asked for, as TYMED_ISTREAM: a reader that takes only
// an HGLOBAL is refused in QueryGetData/GetData before any session opens, since a large or
// unfinished file could not be served to it. The descriptor is built once
// per list. Each session's costs are logged when it ends (streamhost_win.h: PasteStats). Such an
// object may also be announced without its list
// (too long for the announcement): the descriptor is then asked for (names and sizes only, no
// contents, no paste needed) when a reader first reads it. Cancel in Explorer stops its reads and
// releases the streams; the object keeps its session (Explorer releases a stream after each file),
// so the session ends, and the transfer with it, once nobody has read for a while
// (pastesession_win.h: abandon). Without FileStreaming (drags, the agent outside
// service mode, an older service, a guest's clipboard, more than 50,000 entries or a path of
// MAX_PATH or more) the first contents request transfers everything, as before.
//
// Move paste (0.1.31, FileStreaming::move: a large paste, see ClipboardBridge::wantsMovePaste):
// no descriptor or contents, but a deferred CF_HDROP with Preferred DropEffect = move. A read of
// CF_HDROP (the paste) transfers the files into a new folder of their own while the agent's thread
// keeps pumping (DeskFlower's window shows the progress, with Cancel; the service's paste rule
// applies as for any transfer), then names the transferred top-level items, which Explorer moves
// (a rename on the same volume, a copy and delete to another one): every byte is written once.
// That copy serves its paste only: a read again within a few seconds with nothing taken from it
// gets the same items, any other read is another paste and gets a new transfer. A second paste
// while a transfer runs is refused. Once Explorer reports a move (Performed DropEffect or Paste
// Succeeded, or EndOperation) or the items are gone, the copy is consumed: the service removes
// its emptied folder. A copy not consumed (a paste cancelled before or during the move) stays
// pinned in the service while this object lives, then goes with the older received copies. The
// clipboard keeps its content (no cut): the next paste transfers again.

#include "clipboardbridge.h"

#include <functional>

struct IDataObject;

// A new object with one reference. preferredEffect: DROPEFFECT_COPY or DROPEFFECT_MOVE.
// asyncDrop (drags): the object also offers IDataObjectAsyncCapability, so a drop target that
// supports it (the shell's) extracts the files on a thread of its own, with its own progress, and
// the drag loop returns at once; the thread that runs the drag must then keep its apartment alive
// (waitForAsyncDrop) until the target ends the operation.
IDataObject *createVirtualFiles(const QList<ClipboardBridge::ListedFile> &files, ClipboardBridge::FetchFiles fetch,
                                unsigned long preferredEffect = 1 /* DROPEFFECT_COPY */, bool asyncDrop = false);

// The same object, extended with sealed text/HTML/image formats rendered lazily: CF_UNICODETEXT,
// "HTML Format" (the CF_HTML header built at render time), CF_DIB and "PNG", each decoded from the
// plaintext `unseal` returns for text/plain, text/html and image/png. `files` may be empty: with
// pendingFiles > 0 and no list (too many files to list), the files are offered as a deferred
// CF_HDROP instead, transferred at the first request; with neither, a text-only sealed clipboard.
// Nothing is decrypted until an application asks for a format; a rendered format is then kept for
// the object's lifetime. gated (a guest's clipboard, see ClipboardBridge::applySealed): reads with
// no user input just before are refused without calling `unseal`, a refusal is remembered only
// for a moment, and a rendered format only a few seconds (the paste grant's latch). A gated
// object, and any object with files (listed or not), carries the formats that keep it out of the
// clipboard history, the cloud clipboard and clipboard monitors
// (ExcludeClipboardContentFromMonitorProcessing, CanIncludeInClipboardHistory = 0,
// CanUploadToCloudClipboard = 0); an ungated text-only object does not.
// streaming (see ClipboardBridge::FileStreaming): listed files, or with pendingFiles > 0 and no
// list files listed through streaming.list at the first descriptor read (offered from creation,
// never as a deferred CF_HDROP), whose contents stream while they arrive; with streaming.move
// (listed or not) the move paste instead (see the top of this file), whatever preferredEffect.
IDataObject *createSealedData(const QList<ClipboardBridge::SealedFormat> &formats, ClipboardBridge::UnsealFormat unseal,
                              const QList<ClipboardBridge::ListedFile> &files, int pendingFiles,
                              ClipboardBridge::FetchFiles fetchFiles, bool gated = false,
                              unsigned long preferredEffect = 1 /* DROPEFFECT_COPY */,
                              ClipboardBridge::FileStreaming streaming = {});

// What a drop target reported back through a data object it took, once done: for virtual files in
// async mode the end of the operation (EndOperation: its result and effect), otherwise "Performed
// DropEffect" and "Paste Succeeded" (set with SetData). The only evidence that the target kept the
// files: a drop target may accept a drop and then fail, or read the files and keep nothing.
// "Logical Performed DropEffect" is not used: the shell sets it as it starts, before reading
// anything, even when the transfer then fails.
struct DropReport {
    bool asyncPending = false; // virtual files still being extracted on the target's thread
    bool asyncFailed = false;  // the async operation ended with an error
    unsigned long effect = 0;  // the effect the target reported (DROPEFFECT_*), 0 if none
    bool moved() const { return !asyncFailed && (effect & 2 /* DROPEFFECT_MOVE */); }
    bool copied() const { return !asyncFailed && (effect & 1 /* DROPEFFECT_COPY */); }
};
DropReport dropReport(IDataObject *data);

// Pumps this thread's messages (the target's extraction thread calls back into the object on it)
// until the async operation of `data` ends, `abort` returns true, or timeoutMs pass. Returns at
// once if no async operation was started. True if none is pending any more.
bool waitForAsyncDrop(IDataObject *data, unsigned long timeoutMs, const std::function<bool()> &abort = {});

// Transfers files for a paste or a drop (`fetch`, which blocks) on a worker thread while the
// calling thread keeps processing its messages: the agent's own window, its connection to the
// service (transfer progress, prompts) and further calls into the clipboard object stay alive
// during a transfer that may take minutes. Returns what `fetch` returned. If the application quits
// meanwhile, the fetch is abandoned (ClipboardBridge::abandonFetches) and waited for only until it
// notices.
QStringList fetchWhilePumping(const ClipboardBridge::FetchFiles &fetch, QString *error);
// The same for any blocking work (a list, opening a paste session). timeoutMs >= 0 bounds the
// wait: false if it passed first, or if the application quits meanwhile; the worker then finishes
// on its own, so `work` must own everything it uses. With no bound, a quit abandons the fetches
// (as above) and waits for `work` to return.
bool runWhilePumping(std::function<void()> work, long long timeoutMs = -1);
// Whether such a transfer is being waited for: replacing the clipboard then is deferred until it
// returns (the object being rendered must not be released under it).
bool clipboardRenderInProgress();

// For tests: how long a move paste's copy serves further reads of its paste (2 s by default).
void setMoveReuseWindowForTests(int ms);
// For tests: how long the copies of a move paste object that went stay held while their items are
// still there (30 min by default; 0: let go at once).
void setMoveLingerForTests(int ms);
