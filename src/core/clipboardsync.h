#pragma once

// One clipboard for the whole group of paired devices: whatever was copied last, anywhere, can be
// pasted everywhere. The local session helper reports clipboard changes and applies remote ones.
//
// Content: formats ("text/plain" and "text/html" in UTF-8, "image/png") and copied files/folders.
// The announcement goes over the reliable UDP channel and carries small formats inline:
//   clip {v: stamp ms, a: author id, f: {mime: bytes}, big: {mime: size}, fb: file bytes,
//         fc: file count, o: offer id, fl: [[relative path, size], ...] (up to kMaxListedFiles)}
// Large formats are fetched right away by each receiver over an encrypted TCP stream
// (FileTransfer) from the device that announced them. Copied files are not: copying only announces
// them (count and size, from the file system's metadata), and they are transferred when the user
// pastes them (fetchFiles), into <downloads>/Clipboard/<stamp>/. A device re-announces what it
// adopted with its own offer, so devices paired only through it get the formats too (not the files
// it has not fetched).
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

    using FolderProvider = std::function<QString()>;

    // downloads: where received files go (a "Clipboard" subfolder is created inside).
    ClipboardSync(Node *node, FileTransfer *transfer, Settings *settings, FolderProvider downloads,
                  QObject *parent = nullptr);

    using FilesCallback = std::function<void(bool ok, const QStringList &files, const QString &error)>;

    bool isEnabled() const;
    void setEnabled(bool enabled);

    // The local clipboard changed (reported by the session helper).
    void localChanged(const ClipboardData &formats, const QStringList &files = {});
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

    void onPeerConnected(const QByteArray &peerId);
    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    void adopt(Content content, const QByteArray &from);
    // Publishes large formats and files for peers to fetch; fills offer, fileBytes, fileCount.
    void publish(Content *content);
    QCborMap announcement() const;
    void broadcast(const QByteArray &except = {});
    void pruneDownloads(const QString &clipboardDir) const;
    // A newer clipboard replaces the current one: an unfinished file transfer is dropped.
    void cancelFileFetch();
    QString peerName(const QByteArray &peerId) const;

    Node *m_node;
    FileTransfer *m_transfer;
    FolderProvider m_downloads;
    Settings *m_settings;
    Content m_current;
    // In-flight fetch for an announcement not applied yet.
    Stamp m_fetchingStamp;
    quint64 m_fetchHandle = 0;
    // Files being transferred for the current clipboard (a paste), and who waits for them.
    quint64 m_filesHandle = 0;
    QList<FilesCallback> m_filesWaiters;
};

} // namespace df
