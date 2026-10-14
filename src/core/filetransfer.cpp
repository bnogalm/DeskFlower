#include "filetransfer.h"

#include "crypto.h"
#include "node.h"
#include "securestream.h"
#include "wire.h"

#include <QCborArray>
#include <QCborValue>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <memory>

Q_LOGGING_CATEGORY(lcTransfer, "deskflower.transfer")

namespace df {

namespace {
constexpr quint8 kControl = 1;
constexpr quint8 kData = 2;
constexpr int kIdleTimeoutMs = 30000;

QByteArray control(const QCborMap &msg) { return char(kControl) + QCborValue(msg).toCbor(); }

QCborMap typed(const QString &t)
{
    QCborMap m;
    m.insert(QStringLiteral("t"), t);
    return m;
}
} // namespace

// --- Offer -------------------------------------------------------------------------------------

qint64 Offer::fileBytes() const
{
    qint64 total = 0;
    for (const File &f : files)
        total += f.size;
    return total;
}

qint64 Offer::blobBytes() const
{
    qint64 total = 0;
    for (const QByteArray &b : blobs)
        total += b.size();
    return total;
}

bool Offer::addPaths(Offer *offer, const QStringList &paths, qint64 maxBytes, int maxFiles, QString *error)
{
    qint64 total = offer->fileBytes();
    auto add = [&](const QString &abs, const QString &rel, qint64 size) {
        offer->files.append(File{abs, rel, size});
        total += size;
        if (total > maxBytes) {
            *error = QStringLiteral("More than %1 MB").arg(maxBytes / (1024 * 1024));
            return false;
        }
        if (offer->files.size() > maxFiles) {
            *error = QStringLiteral("More than %1 files").arg(maxFiles);
            return false;
        }
        return true;
    };
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isFile()) {
            if (!add(info.absoluteFilePath(), info.fileName(), info.size()))
                return false;
        } else if (info.isDir()) {
            const QDir base = info.absoluteDir(); // parent: relative paths keep the folder name
            QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QFileInfo f(it.next());
                if (!add(f.absoluteFilePath(), base.relativeFilePath(f.absoluteFilePath()), f.size()))
                    return false;
            }
        }
    }
    return true;
}

QString FileTransfer::sanitizeRelativePath(const QString &path)
{
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (p.startsWith(QLatin1Char('/')))
        return {};
    QStringList parts;
    for (const QString &part : p.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..") || part.contains(QLatin1Char(':')))
            return {};
        parts.append(part);
    }
    return parts.isEmpty() ? QString() : parts.join(QLatin1Char('/'));
}

// --- Serving -----------------------------------------------------------------------------------

struct FileTransfer::Upload {
    QPointer<SecureStream> stream;
    Offer offer;
    bool includeFiles = true;
    int item = 0; // index over blobs then files
    qint64 offset = 0;
    std::unique_ptr<QFile> file;
    bool finished = false;
};

FileTransfer::FileTransfer(Node *node, QObject *parent) : QObject(parent), m_node(node)
{
    connect(m_node, &Node::streamAccepted, this, &FileTransfer::onStreamAccepted);
}

quint64 FileTransfer::publish(const Offer &offer)
{
    quint64 id = 0;
    while (id == 0 || m_offers.count(id))
        id = wire::readU64(crypto::randomBytes(8), 0) & 0x1FFFFFFFFFFFFFull; // fits a CBOR/JSON integer
    m_offers.emplace(id, offer);
    while (int(m_offers.size()) > kKeptOffers)
        m_offers.erase(m_offers.begin()); // ids are random: drops an arbitrary old one
    return id;
}

void FileTransfer::unpublish(quint64 offerId) { m_offers.erase(offerId); }

void FileTransfer::onStreamAccepted(SecureStream *stream)
{
    // The first control frame says what the peer wants.
    // Only the first frame says whether this stream is a download (other services, such as the
    // remote view, share the same streams).
    auto first = std::make_shared<QMetaObject::Connection>();
    *first = connect(stream, &SecureStream::received, this, [this, stream, first](const QByteArray &frame) {
        disconnect(*first);
        if (frame.isEmpty() || quint8(frame[0]) != kControl)
            return;
        const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
        if (msg.value(QStringLiteral("t")).toString() != QLatin1String("get"))
            return;
        const quint64 offerId = quint64(msg.value(QStringLiteral("o")).toInteger());
        auto it = m_offers.find(offerId);
        if (it == m_offers.end()) {
            QCborMap err = typed(QStringLiteral("error"));
            err.insert(QStringLiteral("r"), QStringLiteral("No such offer"));
            stream->send(control(err));
            stream->close();
            return;
        }
        auto *up = new Upload;
        up->stream = stream;
        up->offer = it->second;
        up->includeFiles = msg.value(QStringLiteral("files")).toBool();
        if (!up->includeFiles)
            up->offer.files.clear();

        QCborArray items;
        for (auto b = up->offer.blobs.constBegin(); b != up->offer.blobs.constEnd(); ++b) {
            QCborMap item;
            item.insert(QStringLiteral("k"), QStringLiteral("blob"));
            item.insert(QStringLiteral("n"), b.key());
            item.insert(QStringLiteral("s"), b.value().size());
            items.append(item);
        }
        for (const Offer::File &f : up->offer.files) {
            QCborMap item;
            item.insert(QStringLiteral("k"), QStringLiteral("file"));
            item.insert(QStringLiteral("n"), f.relativePath);
            item.insert(QStringLiteral("s"), f.size);
            items.append(item);
        }
        QCborMap manifest = typed(QStringLiteral("manifest"));
        manifest.insert(QStringLiteral("items"), items);
        stream->send(control(manifest));

        const quint64 handle = m_nextHandle++;
        m_uploads[handle] = up;
        connect(stream, &SecureStream::bytesWritten, this, [this, handle] { pump(handle); });
        connect(stream, &SecureStream::closed, this, [this, handle] {
            auto found = m_uploads.find(handle);
            if (found != m_uploads.end()) {
                delete found->second;
                m_uploads.erase(found);
            }
        });
        pump(handle);
    });
}

void FileTransfer::pump(quint64 uploadHandle)
{
    auto found = m_uploads.find(uploadHandle);
    if (found == m_uploads.end())
        return;
    Upload *up = found->second;
    SecureStream *stream = up->stream;
    if (!stream || up->finished)
        return;
    const int blobCount = up->offer.blobs.size();
    const int total = blobCount + up->offer.files.size();
    while (stream->pendingBytes() < kHighWater) {
        if (up->item >= total) {
            up->finished = true;
            stream->send(control(typed(QStringLiteral("end"))));
            stream->close();
            return;
        }
        QByteArray chunk;
        qint64 size = 0;
        if (up->item < blobCount) {
            const QByteArray &blob = std::next(up->offer.blobs.constBegin(), up->item).value();
            size = blob.size();
            chunk = blob.mid(up->offset, kChunk);
        } else {
            const Offer::File &f = up->offer.files.at(up->item - blobCount);
            size = f.size;
            if (!up->file) {
                up->file = std::make_unique<QFile>(f.absolutePath);
                if (!up->file->open(QIODevice::ReadOnly)) {
                    QCborMap err = typed(QStringLiteral("error"));
                    err.insert(QStringLiteral("r"), QStringLiteral("Cannot read %1").arg(f.relativePath));
                    stream->send(control(err));
                    up->finished = true;
                    stream->close();
                    return;
                }
            }
            chunk = up->file->read(qMin<qint64>(kChunk, size - up->offset));
        }
        if (size > 0 && chunk.isEmpty()) {
            QCborMap err = typed(QStringLiteral("error"));
            err.insert(QStringLiteral("r"), QStringLiteral("File changed while sending"));
            stream->send(control(err));
            up->finished = true;
            stream->close();
            return;
        }
        if (!chunk.isEmpty()) {
            stream->send(char(kData) + chunk);
            up->offset += chunk.size();
        }
        if (up->offset >= size) {
            up->item++;
            up->offset = 0;
            up->file.reset();
        }
    }
}

// --- Fetching ----------------------------------------------------------------------------------

struct FileTransfer::Download {
    SecureStream *stream = nullptr;
    QString targetDir;
    qint64 maxBytes = 0;
    Callback done;
    struct Item {
        bool blob = false;
        QString name;
        qint64 size = 0;
    };
    QList<Item> items;
    bool haveManifest = false;
    int item = 0;
    qint64 offset = 0;
    std::unique_ptr<QFile> file;
    FetchResult result;
    QTimer idle;
};

quint64 FileTransfer::fetch(const QByteArray &peerId, quint64 offerId, const QString &targetDir, bool includeFiles,
                            qint64 maxBytes, Callback done)
{
    const quint64 handle = m_nextHandle++;
    SecureStream *stream = m_node->openStream(peerId);
    if (!stream) {
        QTimer::singleShot(0, this, [done] { done(FetchResult{false, QStringLiteral("Peer not reachable"), {}, {}}); });
        return handle;
    }
    auto *dl = new Download;
    dl->stream = stream;
    dl->targetDir = targetDir;
    dl->maxBytes = maxBytes;
    dl->done = std::move(done);
    m_downloads[handle] = dl;

    dl->idle.setSingleShot(true);
    dl->idle.setInterval(kIdleTimeoutMs);
    connect(&dl->idle, &QTimer::timeout, this, [this, handle] {
        finishDownload(handle, FetchResult{false, QStringLiteral("Transfer timed out"), {}, {}});
    });
    dl->idle.start();

    connect(stream, &SecureStream::established, this, [stream, offerId, includeFiles] {
        QCborMap get = typed(QStringLiteral("get"));
        get.insert(QStringLiteral("o"), qint64(offerId));
        get.insert(QStringLiteral("files"), includeFiles);
        stream->send(control(get));
    });
    connect(stream, &SecureStream::closed, this, [this, handle](const QString &reason) {
        finishDownload(handle, FetchResult{false, reason, {}, {}});
    });
    connect(stream, &SecureStream::received, this,
            [this, handle](const QByteArray &frame) { onDownloadFrame(handle, frame); });
    return handle;
}

void FileTransfer::onDownloadFrame(quint64 handle, const QByteArray &frame)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end() || frame.isEmpty())
        return;
    Download *d = found->second;
    d->idle.start();
    QString error;
    auto fail = [&](const QString &why) { finishDownload(handle, FetchResult{false, why, {}, {}}); };

    if (quint8(frame[0]) == kData) {
        if (!d->haveManifest)
            return fail(QStringLiteral("Data before manifest"));
        if (!consume(d, frame.mid(1), &error))
            fail(error);
        return;
    }
    if (quint8(frame[0]) != kControl)
        return;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(QStringLiteral("t")).toString();
    if (t == QLatin1String("manifest") && !d->haveManifest) {
        qint64 total = 0;
        for (const QCborValue &v : msg.value(QStringLiteral("items")).toArray()) {
            const QCborMap m = v.toMap();
            Download::Item item;
            item.blob = m.value(QStringLiteral("k")).toString() == QLatin1String("blob");
            item.name = item.blob ? m.value(QStringLiteral("n")).toString()
                                  : sanitizeRelativePath(m.value(QStringLiteral("n")).toString());
            item.size = m.value(QStringLiteral("s")).toInteger();
            if (item.name.isEmpty() || item.size < 0)
                return fail(QStringLiteral("Unsafe or invalid item in manifest"));
            total += item.size;
            d->items.append(item);
        }
        if (total > d->maxBytes)
            return fail(QStringLiteral("Transfer larger than allowed (%1 bytes)").arg(total));
        d->haveManifest = true;
        if (!consume(d, {}, &error)) // leading zero-length items
            fail(error);
    } else if (t == QLatin1String("end")) {
        if (!consume(d, {}, &error))
            return fail(error);
        if (!d->haveManifest || d->item < d->items.size())
            return fail(QStringLiteral("Transfer ended early"));
        FetchResult result = std::move(d->result);
        result.ok = true;
        finishDownload(handle, std::move(result));
    } else if (t == QLatin1String("error")) {
        fail(msg.value(QStringLiteral("r")).toString());
    }
}

bool FileTransfer::consume(Download *d, QByteArray data, QString *error)
{
    while (d->item < d->items.size()) {
        Download::Item &item = d->items[d->item];
        if (data.isEmpty() && item.size > d->offset)
            return true; // wait for more data
        const QByteArray part = data.left(item.size - d->offset);
        data.remove(0, part.size());
        if (item.blob) {
            d->result.blobs[item.name].append(part);
        } else {
            if (!d->file) {
                const QString path = QDir(d->targetDir).filePath(item.name);
                QDir().mkpath(QFileInfo(path).absolutePath());
                d->file = std::make_unique<QFile>(path);
                if (!d->file->open(QIODevice::WriteOnly)) {
                    *error = QStringLiteral("Cannot write %1").arg(path);
                    return false;
                }
                const QString top = QDir(d->targetDir).filePath(item.name.section(QLatin1Char('/'), 0, 0));
                if (!d->result.files.contains(top))
                    d->result.files.append(top);
            }
            if (d->file->write(part) != part.size()) {
                *error = QStringLiteral("Disk full or write error");
                return false;
            }
        }
        d->offset += part.size();
        if (d->offset < item.size)
            return true;
        d->file.reset();
        d->item++;
        d->offset = 0;
    }
    if (!data.isEmpty()) {
        *error = QStringLiteral("More data than announced");
        return false;
    }
    return true;
}

void FileTransfer::cancel(quint64 handle)
{
    finishDownload(handle, FetchResult{false, QStringLiteral("Cancelled"), {}, {}});
}

void FileTransfer::finishDownload(quint64 handle, FetchResult result)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    m_downloads.erase(found);
    d->file.reset();
    d->idle.stop();
    if (d->stream) {
        d->stream->disconnect(this);
        d->stream->close();
        d->stream->deleteLater();
    }
    if (!result.ok)
        qCInfo(lcTransfer) << "Transfer failed:" << result.error;
    Callback done = std::move(d->done);
    delete d;
    if (done)
        done(result);
}

FileTransfer::~FileTransfer()
{
    for (auto &[handle, d] : m_downloads) {
        if (d->stream) {
            d->stream->disconnect(this);
            d->stream->deleteLater();
        }
        delete d;
    }
    for (auto &[handle, up] : m_uploads)
        delete up;
}

} // namespace df
