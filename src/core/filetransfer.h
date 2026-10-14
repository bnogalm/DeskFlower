#pragma once

// Bulk transfers over encrypted TCP streams (SecureStream).
//
// A device publishes an "offer": named blobs (e.g. a large clipboard image) and files/folders.
// Peers fetch it by id; the publisher only ever serves what it offered. Protocol, one stream per
// fetch, every frame starting with a kind byte (1 = CBOR control, 2 = data):
//   client: get {o: offer id, files: bool}
//   server: manifest {items: [{k: "blob"|"file", n: name or relative path, s: size}]}
//           data frames, items concatenated in manifest order
//           end {}            or   error {r}
// The receiver sanitises relative paths and writes files under its target directory.

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <functional>
#include <map>

namespace df {

class Node;
class SecureStream;

struct Offer {
    struct File {
        QString absolutePath;
        QString relativePath; // includes the top-level item name ("photos/2024/a.jpg")
        qint64 size = 0;
    };
    QMap<QString, QByteArray> blobs;
    QList<File> files;

    qint64 fileBytes() const;
    qint64 blobBytes() const;
    // Expands files and folders (recursively). Fails if the total exceeds maxBytes or maxFiles.
    static bool addPaths(Offer *offer, const QStringList &paths, qint64 maxBytes, int maxFiles, QString *error);
};

struct FetchResult {
    bool ok = false;
    QString error;
    QMap<QString, QByteArray> blobs;
    QStringList files; // absolute local paths of the top-level items
};

class FileTransfer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kChunk = 60 * 1024;
    static constexpr qint64 kHighWater = 1024 * 1024; // pause sending above this many queued bytes
    static constexpr int kKeptOffers = 4;

    explicit FileTransfer(Node *node, QObject *parent = nullptr);
    ~FileTransfer() override; // pending fetches are dropped without calling their callbacks

    quint64 publish(const Offer &offer);
    void unpublish(quint64 offerId);

    using Callback = std::function<void(const FetchResult &)>;
    // Downloads offer `offerId` from `peerId`. Files go under targetDir. Returns a handle for cancel().
    quint64 fetch(const QByteArray &peerId, quint64 offerId, const QString &targetDir, bool includeFiles,
                  qint64 maxBytes, Callback done);
    void cancel(quint64 handle);

    // "a/../b", absolute paths and drive letters are rejected; returns empty if unsafe.
    static QString sanitizeRelativePath(const QString &path);

private:
    struct Upload;
    struct Download;
    void onStreamAccepted(SecureStream *stream);
    void pump(quint64 uploadHandle);
    void onDownloadFrame(quint64 handle, const QByteArray &frame);
    // Writes data into the current item(s); also completes zero-length items. False on error.
    bool consume(Download *d, QByteArray data, QString *error);
    void finishDownload(quint64 handle, FetchResult result);

    Node *m_node;
    std::map<quint64, Offer> m_offers;
    std::map<quint64, Upload *> m_uploads;
    std::map<quint64, Download *> m_downloads;
    quint64 m_nextHandle = 1;
};

} // namespace df
