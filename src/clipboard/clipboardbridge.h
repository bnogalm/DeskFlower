#pragma once

// The local end of clipboard sharing: watches this session's clipboard and reports changes
// (text/plain, text/html, image/png, copied files), and puts content received from the service on
// it. Used by the process that owns the user's clipboard: on Windows in service mode the agent,
// which runs as the user (the session helper runs as LocalSystem, and Windows does not give it the
// user's clipboard reliably), elsewhere the session helper.

#include <QCborArray>
#include <QCborMap>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

class ClipboardBackend;

class ClipboardBridge : public QObject
{
    Q_OBJECT
public:
    explicit ClipboardBridge(QObject *parent = nullptr);
    ~ClipboardBridge() override;

    // Transfers the files of a deferred paste and returns their local paths (empty + error if not).
    // It should give up soon once fetchesAbandoned() is true.
    using FetchFiles = std::function<QStringList(QString *error)>;
    // The error of a FetchFiles refused because the read was no paste (no key or click at this
    // device just before: a clipboard reader looking at the clipboard): nothing is wrong, nobody
    // is told, and nothing of it is remembered (the paste that follows asks again).
    static constexpr const char kNotAPaste[] = "Not a paste";
    // The application is quitting while a paste waits for its files (Windows, fetchWhilePumping):
    // a FetchFiles still blocked gives up rather than hold the process until the transfer ends.
    // Also called when the application is about to quit (Backend): streams of a streaming paste
    // still held then fail rather than read on.
    static void abandonFetches();
    static bool fetchesAbandoned();
    // Renders one sealed format (a paste): returns its plaintext bytes, or empty + error if the
    // paste was not granted. Blocking (called from inside the application's clipboard request).
    using UnsealFormat = std::function<QByteArray(const QString &mime, QString *error)>;
    // A format present on the sealed clipboard: its mime type and plaintext size.
    struct SealedFormat {
        QString mime;
        qint64 size = 0;
    };

    // Where the clipboard is read and written: QClipboard unless replaced (a Wayland helper uses
    // the portal's clipboard, see ClipboardBackend). Takes ownership.
    void setBackend(ClipboardBackend *backend);
    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }
    // Puts content on the local clipboard; its own change notification is not reported back.
    void apply(const QCborMap &formats, const QStringList &files);
    // Offers `count` files copied on another device; they are transferred (fetch, blocking) only
    // when an application asks for them, i.e. when the user pastes. Windows only.
    static bool supportsDeferredFiles();
    void applyDeferredFiles(int count, FetchFiles fetch);
    // Same, with the names and sizes known: offered as virtual files (file descriptors + contents),
    // which other programs can look at without causing the transfer; Explorer reads the contents,
    // with its own progress, only when the user pastes. relativePaths use '/'. A folder (dir) is
    // listed too, so that empty folders are created; the service sends it with size -1.
    struct ListedFile {
        QString path;
        qint64 size = 0;
        bool dir = false;
    };
    // Reads one [path, size] entry of a list from the service (size -1: a folder).
    static ListedFile listedFromCbor(const QCborArray &item)
    {
        const qint64 size = item.at(1).toInteger();
        return ListedFile{item.at(0).toString(), qMax<qint64>(size, 0), size < 0};
    }
    // One paste that copies the files while they arrive (Windows, a service that announced
    // `stream`): the agent's request for the transfer, seen by the streams it serves. Each stream
    // and the clipboard object that opened it own it; reads and requests for streams are its
    // activity, and a session nobody reads for a while gives the transfer up (PasteSession).
    class PasteSessionBase
    {
    public:
        // Running: the transfer goes on (files appear under root, each growing to its size).
        // Succeeded: every file is complete. The others end it: Failed (the transfer or the
        // connection to the service failed), Cancelled (the user cancelled it in DeskFlower),
        // Abandoned (nobody read for a while: given up).
        enum class State { Running, Succeeded, Failed, Cancelled, Abandoned };
        virtual ~PasteSessionBase() = default;
        // The folder the files arrive in, at their relative paths. It may not exist yet.
        virtual QString root() const = 0;
        virtual State state() const = 0;
        // A stream's Read starts / ends: never given up while one is in progress.
        virtual void readStarted() = 0;
        virtual void readEnded() = 0;
        // A stream was asked for (FileContents): activity too.
        virtual void touch() = 0;
        // A move paste's session (FileStreaming::move): the paste moved the files out of root, so
        // the service removes what is left there and transfers again for the next paste. Takes
        // effect when the session goes (its request tells the service before closing).
        virtual void consumed() {}
        bool terminal() const { return state() != State::Running; }
        bool succeeded() const { return state() == State::Succeeded; }
    };
    // Pasting while the files arrive (empty: not possible, the whole transfer happens at the first
    // contents request as before). list: the names and sizes (a list-only request, no contents
    // move, no paste needed), for an object announced without its list; open: starts the transfer
    // and returns once its folder is known, or null + error (refused, e.g. kNotAPaste);
    // listedAgoMs: how long ago the reader read the list (the descriptor), -1 if it did not (the
    // service also counts a key or click made around that read as the paste's). Both block
    // (called on a worker thread while the clipboard's thread keeps pumping). Windows only; other
    // platforms take it and ignore it.
    //
    // move (Windows, a 0.1.31 service that announced `move`, the "move pastes" setting on, and a
    // large paste: wantsMovePaste): the files are offered as a deferred CF_HDROP with Preferred
    // DropEffect = move instead of virtual files. The paste's read of CF_HDROP opens move (a fresh
    // transfer into a new folder, shown with its progress and Cancel in DeskFlower's window, its
    // folder kept while the session lives; null + error if refused, e.g. kNotAPaste) and waits for
    // it while the clipboard's thread keeps pumping, then names the transferred top-level items:
    // Explorer moves them (on the same volume a rename). Once the paste moved them (Explorer
    // reports it, or the items are gone) the session is told consumed(); the next paste transfers
    // again. Takes precedence over list/open.
    struct FileStreaming {
        std::function<QList<ListedFile>(QString *error)> list;
        std::function<std::shared_ptr<PasteSessionBase>(QString *error, qint64 listedAgoMs)> open;
        std::function<std::shared_ptr<PasteSessionBase>(QString *error)> move;
        bool isEmpty() const { return !open; } // no paste while the files arrive (move aside)
    };
    // The move paste is for large pastes only: at least this many entries (files and folders) or
    // bytes. Smaller ones stay virtual files (Explorer's own copy, progress and Cancel).
    static constexpr qsizetype kMovePasteMinEntries = 2000;
    static constexpr qint64 kMovePasteMinBytes = qint64(1) << 30; // 1 GB
    static bool wantsMovePaste(qsizetype entries, qint64 bytes)
    {
        return entries >= kMovePasteMinEntries || bytes >= kMovePasteMinBytes;
    }
    void applyVirtualFiles(const QList<ListedFile> &files, FetchFiles fetch, FileStreaming streaming = {});
    // Announces copied files to Linux file managers along with text/uri-list: GNOME/Nautilus read
    // x-special/gnome-copied-files ("copy" + the URLs), KDE/Dolphin application/x-kde-cutselection
    // ("0": copied, not cut).
    static QByteArray gnomeCopiedFiles(const QStringList &paths);
    // Offers sealed content, rendered lazily: text/HTML/image formats (decrypted on demand through
    // `unseal`) and `pendingFiles` copied files (`fetchFiles`, transferred on demand), in one
    // clipboard object. Nothing is decrypted or transferred until an application asks for it.
    // A rendered format is kept for the object's lifetime, unless `gated` (reading needs a paste
    // grant: this device is a guest): then a read that does not come from a paste is refused
    // without asking (Windows: no user input in the last moment), a refused render is not
    // remembered, a rendered format is kept only a few seconds, and the object asks clipboard
    // history and monitors to leave it alone. Windows: one IDataObject, not flushed, with the
    // files as virtual files when `files` lists them all (else a deferred CF_HDROP); macOS: native
    // pasteboard items instead (clipboardbridge_mac.h), one per top-level item of `files` (all of
    // them listed there, else empty), each pasted as a file URL by Finder, an empty `files` with
    // pendingFiles being one item offering every path once transferred (`gated` is not applied
    // there: the service's grant still decides); elsewhere: a lazy QMimeData with a text/uri-list
    // rendered at the paste. With copied files (pendingFiles > 0) the object carries, on every OS,
    // the markers clipboard history and monitors honour, so they do not read it on arrival
    // (Windows: ExcludeClipboardContentFromMonitorProcessing, CanIncludeInClipboardHistory = 0,
    // CanUploadToCloudClipboard = 0; X11/Wayland: x-kde-passwordManagerHint "secret"; macOS: the
    // nspasteboard.org Transient, Concealed and AutoGenerated types, pasteboard for this host only).
    // streaming (Windows, see FileStreaming): the files are offered as virtual files whose
    // contents are streams of the files while they arrive, listed through streaming.list at the
    // first descriptor read when `files` is empty.
    void applySealed(const QList<SealedFormat> &formats, const QList<ListedFile> &files, int pendingFiles,
                     UnsealFormat unseal, FetchFiles fetchFiles, bool gated = false, FileStreaming streaming = {});
    // Empties the clipboard if it still holds an object this bridge put there (sealed or deferred
    // content the service no longer knows, e.g. after it restarted); anything else is left alone.
    void clearOwned();
    // Whether what is on the clipboard is still what this bridge put there last (macOS only;
    // elsewhere false).
    bool ownsClipboard() const;

signals:
    // The user copied something: {mime: bytes} and local file paths.
    void changed(const QCborMap &formats, const QStringList &files);

private:
    void onChanged();
    void releaseDeferred();
    // Windows: while a paste of our object is being rendered (clipboardRenderInProgress), a
    // change of the clipboard is kept as `retry` and run once the render returns; a newer change
    // replaces the one waiting (and a change made once the render is over drops it). False: go on
    // now. Other platforms: always false.
    bool deferWhileRendering(std::function<void()> retry);

    bool m_enabled = false;
    ClipboardBackend *m_backend = nullptr;
    QTimer m_debounce;
    QString m_appliedSignature; // what we last put on the clipboard ourselves...
    qint64 m_suppressUntil = 0; // ...and until when its echo is ignored (m_clock)
    QElapsedTimer m_clock;
    void *m_deferred = nullptr;  // Windows: our IDataObject while it is (or was) on the clipboard
    std::function<void()> m_pendingApply; // Windows: the latest change waiting (deferWhileRendering)
    bool m_pendingApplyScheduled = false;
    bool m_ownsSealed = false;   // non-Windows: we set a lazy sealed QMimeData (ignore its echo)
#ifdef Q_OS_MACOS
    // Qt notices a change of the pasteboard only when the application is activated (never, for
    // the background helper): its change counter is polled.
    QTimer m_pasteboardPoll;
    qint64 m_lastChangeCount = -1;
    qint64 m_ownChangeCount = -1; // the change of our last write (its echo is ignored)
    // Sealed items that gave nothing for a type are written again (macclipboard::republish), once
    // per burst; m_sealedSerial tells a republish meant for an older sealed clipboard.
    quint64 m_sealedSerial = 0;
    bool m_republishPending = false;
    void republishSealed(quint64 serial);
#endif
};
