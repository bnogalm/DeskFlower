#pragma once

// The clipboard follows the cursor: what was copied on a device goes to the device the shared
// mouse moves to next, and to no other. The local session helper reports clipboard changes and
// applies remote ones.
//
// Hand-off: the master (whose keyboard and mouse are shared) sees every crossing from X to Y. If X
// is the master itself it sends its clipboard to Y; otherwise it tells Y to expect X's clipboard
// (clipFrom {s: X}) and X to send it to Y (clipTo {d: Y}). X obeys only the master, and only when the
// cursor has just left it; Y accepts a clipboard only while the cursor is on it, and only from the
// master or the device it was told to expect. Nothing is broadcast or relayed, so a password copied
// on one device reaches only the device the user moves to, and no paired device can replace the
// clipboard of another one at will.
//
// Content: formats ("text/plain" and "text/html" in UTF-8, "image/png") and copied files/folders.
// The announcement goes over the reliable UDP channel and carries small formats inline:
//   clip {v: stamp ms, a: author id, f: {mime: bytes}, big: {mime: size}, fb: file bytes,
//         fc: file count, o: offer id, fl: [[relative path, size], ...] (up to kMaxListedFiles)}
// Large formats are fetched right away by each receiver over an encrypted TCP stream
// (FileTransfer) from the device that announced them. Copied files are not: copying only announces
// them (count and size, from the file system's metadata), and they are transferred when the user
// pastes them (fetchFiles), into <downloads>/Clipboard/<stamp>/. A device hands on what it adopted
// with its own offer: the formats, not files it has not fetched.
//
// Last-writer-wins, stamped with wall-clock milliseconds (an old clipboard never overwrites a
// newer one after a reconnection). Not persisted: the clipboard often holds passwords.

#include "filetransfer.h"
#include "settings.h"
#include "workspace.h"

#include <QMap>
#include <QObject>
#include <functional>

namespace df {

class Node;

using ClipboardData = QMap<QString, QByteArray>;

class ClipboardSync : public QObject
{
    Q_OBJECT
public:
    static constexpr qint64 kInlineBytes = 64 * 1024;             // per format, sent with the announcement
    static constexpr qint64 kMaxFormatBytes = 64 * 1024 * 1024;   // per format
    static constexpr qint64 kMaxFileBytes = 1024LL * 1024 * 1024; // copied files, in total
    static constexpr int kMaxFiles = 10000;
    static constexpr int kKeptDownloads = 3;
    // Copied files are listed (names and sizes) in the announcement up to this many, so a paste can
    // show them before transferring anything.
    static constexpr int kMaxListedFiles = 2000;

    struct ListedFile {
        QString path; // relative, '/' separated, top-level item first ("photos/2024/a.jpg")
        qint64 size = 0;
    };

    // How long after the cursor left a device it still hands its clipboard on, and a device the
    // cursor just reached still takes one.
    static constexpr qint64 kHandOffGraceMs = 2000;
    // How long a device waits for the clipboard it was told to expect.
    static constexpr qint64 kExpectMs = 10000;

    using FolderProvider = std::function<QString()>;

    // Where the shared cursor is, from this device's point of view.
    struct Presence {
        std::function<bool(qint64 graceMs)> cursorHere; // on this device, or left it this recently
        std::function<QByteArray()> master;             // device sharing its keyboard and mouse
    };

    // downloads: where received files go (a "Clipboard" subfolder is created inside).
    ClipboardSync(Node *node, FileTransfer *transfer, Settings *settings, Presence presence, FolderProvider downloads,
                  QObject *parent = nullptr);

    using FilesCallback = std::function<void(bool ok, const QStringList &files, const QString &error)>;

    bool isEnabled() const;
    void setEnabled(bool enabled);

    // The local clipboard changed (reported by the session helper).
    void localChanged(const ClipboardData &formats, const QStringList &files = {});
    // Master side: the cursor went from `from` to `to` (InputController::cursorMoved).
    void cursorMoved(const QByteArray &from, const QByteArray &to);
    ClipboardData current() const { return m_current.formats; }
    QStringList currentFiles() const { return m_current.files; }
    // The files of the current clipboard as local paths, transferring them first from the device
    // they were copied on (when the user pastes). `done` runs on this thread, once; a newer
    // clipboard makes it fail.
    void fetchFiles(FilesCallback done);
    // Names and sizes of the files waiting to be pasted (empty if not listed).
    QList<ListedFile> pendingFileList() const { return m_current.pendingFiles > 0 ? m_current.fileList : QList<ListedFile>(); }

signals:
    // The local clipboard should get this content. pendingFiles: files copied on another device,
    // offered on the clipboard and transferred when pasted (fetchFiles).
    void applyLocally(const df::ClipboardData &formats, const QStringList &files, int pendingFiles);
    void enabledChanged(bool enabled);
    // What happens to copied files, for the user (shared, not shared, received, failed).
    void filesNotice(const QString &text, bool problem);

private:
    struct Content {
        Stamp stamp;
        ClipboardData formats;
        QStringList files;
        quint64 offer = 0;
        qint64 fileBytes = 0;
        int fileCount = 0;
        QList<ListedFile> fileList; // own files: what we offer; another device's: what it offers
        // Files copied on another device and not transferred yet.
        QByteArray filesFrom;
        quint64 remoteOffer = 0;
        int pendingFiles = 0;
        qint64 pendingBytes = 0;
        qint64 remoteBlobBytes = 0;
    };

    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    void adopt(Content content, const QByteArray &from);
    // Publishes large formats and files for peers to fetch; fills offer, fileBytes, fileCount.
    void publish(Content *content);
    QCborMap announcement() const;
    // Sends the current clipboard to `peerId` (if there is one and the peer is connected).
    void sendTo(const QByteArray &peerId);
    void pruneDownloads(const QString &clipboardDir) const;
    // A newer clipboard replaces the current one: an unfinished file transfer is dropped.
    void cancelFileFetch();
    QString peerName(const QByteArray &peerId) const;

    Node *m_node;
    FileTransfer *m_transfer;
    FolderProvider m_downloads;
    Settings *m_settings;
    Presence m_presence;
    Content m_current;
    // The device whose clipboard the master told us to expect, and when.
    QByteArray m_expectFrom;
    qint64 m_expectAt = 0;
    // In-flight fetch for an announcement not applied yet.
    Stamp m_fetchingStamp;
    quint64 m_fetchHandle = 0;
    // Files being transferred for the current clipboard (a paste), and who waits for them.
    quint64 m_filesHandle = 0;
    QList<FilesCallback> m_filesWaiters;
};

} // namespace df
