#include "filetransfer.h"

#include "crypto.h"
#include "fsutil.h"
#include "node.h"
#include "securestream.h"
#include "transferworker.h"
#include "wire.h"

#include <QCborArray>
#include <QCborValue>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QLocale>
#include <QPointer>
#include <QStorageInfo>
#include <QTimer>
#include <QVersionNumber>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(lcTransfer, "deskflower.transfer")

namespace df {

namespace {
constexpr quint8 kControl = 1;
// Data frames: kind 2 up to protocol 2 (0.1.22-0.1.30), kind 3 from protocol 3 on. An older
// receiver ignores a kind it does not know and fails with "Transfer ended early": never misread.
constexpr quint8 kOldData = 2;
constexpr quint8 kData = 3;
constexpr int kProtocol = wire::kProtocolVersion;
// Left free on the destination's drive after a transfer: a fetch that would eat into it is refused
// before anything is written.
constexpr qint64 kDiskReserve = 64LL * 1024 * 1024;

// The free space for a new file under `path` (which may not exist yet): that of its nearest
// existing ancestor's volume, -1 if unknown.
qint64 freeSpaceFor(const QString &path)
{
    QFileInfo info(path);
    while (!info.exists()) {
        const QString parent = info.absolutePath();
        if (parent == info.absoluteFilePath())
            return -1;
        info = QFileInfo(parent);
    }
    const QStorageInfo storage(info.absoluteFilePath());
    return storage.isValid() && storage.isReady() ? storage.bytesAvailable() : -1;
}
// What a get says about the receiver's protocol: "pv" from protocol 3 on; before that "v" (2 from
// 0.1.22 on, none before).
int getProtocol(const QCborMap &get)
{
    if (get.contains(QStringLiteral("pv")))
        return int(std::clamp<qint64>(get.value(QStringLiteral("pv")).toInteger(), 1, 1000));
    return get.value(QStringLiteral("v")).toInteger() >= 2 ? 2 : 1;
}

// Why a peer of protocol `theirs` cannot transfer with this device, as the user reads it.
QString mismatchText(const QString &peer, int theirs)
{
    return theirs < kProtocol
               ? QStringLiteral("%1 uses an older transfer protocol (version %2). The protocol has changed: update it.")
                     .arg(peer)
                     .arg(theirs)
               : QStringLiteral("%1 uses a newer transfer protocol (version %2). The protocol has changed: update "
                                "this device.")
                     .arg(peer)
                     .arg(theirs);
}
// Room left in a manifest frame for its envelope ({t, items, more}) besides the items themselves.
constexpr qsizetype kManifestSlack = 64;

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

QList<ListedFile> Offer::listing() const
{
    QList<ListedFile> list;
    list.reserve(dirs.size() + files.size());
    for (const QString &d : dirs)
        list.append(ListedFile{d, 0, true});
    for (const File &f : files)
        list.append(ListedFile{f.relativePath, f.size, false});
    return list;
}

namespace {
// A real link (symbolic link, junction, mount point). Not a Windows shortcut: Qt 6 reports a .lnk
// as isSymLink(), but it is an ordinary file that Explorer copies as such.
bool isRealLink(const QFileInfo &info) { return info.isSymbolicLink() || info.isJunction(); }

// The size of the file itself: for a shortcut Qt reports its target's size (and type), so the
// shortcut is opened to read its own. -1 if it cannot be read.
qint64 ownSize(const QFileInfo &info)
{
    if (!info.isShortcut())
        return info.size();
    QFile f(info.absoluteFilePath());
    return f.open(QIODevice::ReadOnly) ? f.size() : -1;
}
} // namespace

bool Offer::addPaths(Offer *offer, const QStringList &paths, qint64 maxBytes, int maxFiles, QString *error)
{
    qint64 total = offer->fileBytes();
    auto addFile = [&](const QFileInfo &info, const QString &rel) {
        const qint64 size = ownSize(info);
        if (size < 0) {
            offer->skipped.append(rel);
            return true;
        }
        offer->files.append(File{info.absoluteFilePath(), rel, size});
        total += size;
        if (total > maxBytes) {
            *error = QStringLiteral("More than %1").arg(QLocale::c().formattedDataSize(maxBytes));
            return false;
        }
        if (offer->files.size() > maxFiles) {
            *error = QStringLiteral("More than %1 files").arg(maxFiles);
            return false;
        }
        return true;
    };
    auto addDir = [&](const QString &rel) {
        offer->dirs.append(rel);
        if (offer->dirs.size() > maxFiles) { // counted apart from the files, against the same bound
            *error = QStringLiteral("More than %1 folders").arg(maxFiles);
            return false;
        }
        return true;
    };
    constexpr QDir::Filters kEntries = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        // A shortcut first: Qt says isDir() for a shortcut to a folder.
        if (info.isShortcut() || info.isFile()) {
            if (!addFile(info, info.fileName()))
                return false;
        } else if (info.isDir() && !info.fileName().isEmpty()) {
            // Walked by hand rather than with QDirIterator, which descends into junctions: every
            // link inside the folder is left out (and reported), whatever it points to.
            if (!addDir(info.fileName()))
                return false;
            QList<std::pair<QString, QString>> pending{{info.absoluteFilePath(), info.fileName()}};
            while (!pending.isEmpty()) {
                const auto [dirPath, dirRel] = pending.takeLast();
                const QFileInfoList entries = QDir(dirPath).entryInfoList(kEntries, QDir::Name);
                for (const QFileInfo &entry : entries) {
                    const QString rel = dirRel + QLatin1Char('/') + entry.fileName();
                    if (isRealLink(entry)) {
                        offer->skipped.append(rel);
                    } else if (entry.isShortcut() || entry.isFile()) {
                        if (!addFile(entry, rel))
                            return false;
                    } else if (entry.isDir()) {
                        if (!addDir(rel))
                            return false;
                        pending.append({entry.absoluteFilePath(), rel});
                    } else {
                        offer->skipped.append(rel); // a device, socket or pipe
                    }
                }
            }
        } else {
            offer->skipped.append(info.fileName().isEmpty() ? path : info.fileName()); // gone, or a whole drive
        }
    }
    return true;
}

bool peerUnderstandsFolders(const QString &version)
{
    const QVersionNumber v = QVersionNumber::fromString(version);
    return !v.isNull() && v >= QVersionNumber::fromString(QLatin1String(kFoldersVersion));
}

QCborArray listedToCbor(const QList<ListedFile> &list, bool withDirs)
{
    QCborArray out;
    for (const ListedFile &f : list) {
        if (f.dir && !withDirs)
            continue;
        out.append(QCborArray{f.path, f.dir ? qint64(-1) : f.size});
    }
    return out;
}

bool listedFromCbor(const QCborValue &value, QList<ListedFile> *out)
{
    out->clear();
    for (const QCborValue &v : value.toArray()) {
        const QCborArray item = v.toArray();
        const QString path = FileTransfer::sanitizeRelativePath(item.at(0).toString());
        const qint64 size = item.at(1).toInteger(-2);
        if (path.isEmpty() || size < -1) {
            out->clear();
            return false;
        }
        out->append(ListedFile{path, std::max<qint64>(size, 0), size == -1});
    }
    return true;
}

int listedDirCount(const QList<ListedFile> &list)
{
    return int(std::count_if(list.cbegin(), list.cend(), [](const ListedFile &f) { return f.dir; }));
}

namespace {
// Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9), on their own or with any
// extension: writing to them touches a device or silently discards the data.
bool isReservedDeviceName(const QString &component)
{
    const QString base = component.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QStringList kNames = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                       QStringLiteral("NUL")};
    if (kNames.contains(base))
        return true;
    if (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT")))
        && base.at(3) >= QLatin1Char('1') && base.at(3) <= QLatin1Char('9'))
        return true;
    return false;
}
} // namespace

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
        // Win32 strips trailing dots and spaces (so "a." collides with "a"), reserved device names
        // touch devices, control characters confuse shells, and a component over 255 UTF-16 units
        // exceeds the limit on every platform.
        if (part.size() > 255 || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))
            || isReservedDeviceName(part))
            return {};
        if (std::any_of(part.cbegin(), part.cend(), [](QChar c) { return c.unicode() < 32; }))
            return {};
        parts.append(part);
    }
    return parts.isEmpty() ? QString() : parts.join(QLatin1Char('/'));
}

// --- Serving -----------------------------------------------------------------------------------

// What the workers share with the FileTransfer, which may be gone before them: the posts back
// to it (dropped once it is gone), the fetches still at work (isActiveTarget, the free space of
// the others, the destructor's wait) and the uploads whose reader is still at work (the
// destructor's wait).
namespace {
std::atomic<int> g_defaultWriters{0}; // tests
} // namespace

struct FileTransfer::Shared {
    mutable std::mutex mutex;
    std::condition_variable exited; // a worker or a reader is done
    FileTransfer *owner = nullptr;
    std::map<quint64, std::shared_ptr<DownloadJob>> live;
    std::map<quint64, std::shared_ptr<UploadJob>> uploads;
    std::atomic<int> writeDelayMs{0}; // tests
    std::atomic<qint64> peakWriterQueued{0}; // tests
    std::atomic<int> lastWriters{0};          // tests
    // Counters for the benchmark (FileTransfer::countersForTests): cheap, always kept.
    std::atomic<qint64> dataFramesSent{0};
    std::atomic<qint64> dataFramesReceived{0};
    std::atomic<qint64> writerBusyNs{0};
    std::atomic<qint64> writerRoomWaitNs{0};
    std::atomic<qint64> readAheadFiles{0};
    std::atomic<qint64> readAheadWaitNs{0};

    void post(std::function<void(FileTransfer *)> call)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (owner)
            QMetaObject::invokeMethod(owner, [o = owner, call = std::move(call)] { call(o); }, Qt::QueuedConnection);
    }
    bool isActive(const QString &dir) const;
};

struct FileTransfer::Upload {
    QPointer<SecureStream> stream;
    std::shared_ptr<UploadJob> job; // the reader thread holds it too, until it returns
    QByteArray peer;                // kMaxUploadsPerOfferAndPeer
    quint64 offerId = 0;
    bool finished = false;          // the last frame was sent
};

FileTransfer::FileTransfer(Node *node, QObject *parent)
    : QObject(parent), m_node(node), m_impersonate([] { return std::shared_ptr<void>(std::make_shared<int>(0)); }),
      m_shared(std::make_shared<Shared>())
{
    m_shared->owner = this;
    connect(m_node, &Node::streamAccepted, this, &FileTransfer::onStreamAccepted);
}

std::shared_ptr<void> FileTransfer::impersonate() const
{
    return m_impersonate ? m_impersonate() : std::shared_ptr<void>(std::make_shared<int>(0));
}

bool FileTransfer::addOfferPaths(Offer *offer, const QStringList &paths, qint64 maxBytes, int maxFiles, QString *error)
{
    // As the console user, never as LocalSystem: it may only offer files that user can read. That
    // is the boundary: a path the user chose may go through a link (their "My Documents" junction,
    // a redirected folder) and is read with their rights all the same; links inside a chosen
    // folder are skipped by Offer::addPaths and reported.
    const auto asUser = impersonate();
    if (!asUser) {
        *error = QStringLiteral("Nobody is signed in");
        return false;
    }
    const bool ok = Offer::addPaths(offer, paths, maxBytes, maxFiles, error);
    if (!offer->skipped.isEmpty())
        qCInfo(lcTransfer) << "Left out of an offer (links or special files):" << offer->skipped.size();
    return ok;
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

void FileTransfer::allowPeer(quint64 offerId, const QByteArray &peerId, bool on)
{
    auto it = m_offers.find(offerId);
    if (it == m_offers.end())
        return;
    if (on)
        it->second.allowed.insert(peerId);
    else
        it->second.allowed.remove(peerId);
}

namespace {
// The head of a CBOR data item: its major type and its length or count (RFC 8949, 3.1).
void cborHead(QByteArray &out, quint8 major, quint64 n)
{
    const char type = char(major << 5);
    if (n < 24) {
        out.append(char(type | char(n)));
        return;
    }
    int bytes = 8;
    char info = 27;
    if (n <= 0xff) {
        bytes = 1;
        info = 24;
    } else if (n <= 0xffff) {
        bytes = 2;
        info = 25;
    } else if (n <= 0xffffffffull) {
        bytes = 4;
        info = 26;
    }
    out.append(char(type | info));
    for (int i = bytes - 1; i >= 0; --i)
        out.append(char((n >> (8 * i)) & 0xff));
}

void cborText(QByteArray &out, QLatin1String text)
{
    cborHead(out, 3, quint64(text.size()));
    out.append(text.data(), text.size());
}
} // namespace

// --- Serving, reading ahead ---------------------------------------------------------------------
//
// What a tree of small files costs the serving side is opening and closing them, one after
// another on the upload's reader thread. ReadAhead overlaps that: a few helper threads claim the
// small files (FileTransfer::kReadAheadMax bytes at most) in manifest order, open and read each
// whole into memory and close it, and the reader takes them (take) when it gets to them, in the
// same order, and packs them as it packed what it read itself. What is held (read, or claimed and
// being read, and not taken yet) is bounded by a byte budget, every file counting as at least
// kReadAheadSlot; claims go in manifest order, so the file the reader waits for is always claimed
// first: whatever is held then comes after it, and the budget never holds the reader up.
// Each helper reads as the console user exactly as the reader does (its own AsUserBatch, checked
// against the identity the upload recorded first: one IdentityAnchor), never holds the user's
// rights while it waits, and stops at its next file once the upload is cancelled or the reader has
// gone. A file is read exactly as the reader read it: its listed size, no more (a file that grew is
// cut there, as before) and no less (one that shrank is "changed"); the reader reports what went
// wrong when it gets to that file, with the error it always gave.

namespace {
class ReadAhead
{
public:
    enum class Status { Ok, OpenFailed, Changed, NoUser, UserChanged };
    struct Result {
        Status status = Status::Ok;
        QByteArray data;
    };

    ReadAhead(const QList<Offer::File> &files, qint64 budget, Impersonator impersonate,
              std::function<QString()> identity, std::shared_ptr<detail::IdentityAnchor> anchor,
              std::function<bool()> cancelled)
        : m_files(files), m_budget(budget), m_impersonate(std::move(impersonate)), m_identity(std::move(identity)),
          m_anchor(std::move(anchor)), m_cancelled(std::move(cancelled))
    {
    }
    ~ReadAhead() { stop(); }
    ReadAhead(const ReadAhead &) = delete;
    ReadAhead &operator=(const ReadAhead &) = delete;

    static bool eligible(const Offer::File &f) { return f.size <= FileTransfer::kReadAheadMax; }

    // Starts up to `threads` helpers; false when none could be had (the reader reads every file
    // itself, as without read-ahead).
    bool start(int threads)
    {
        for (int i = 0; i < threads; ++i) {
            try {
                m_threads.emplace_back([this] { loop(); });
            } catch (const std::system_error &) {
                break; // no more threads to be had: the ones started do the work
            }
        }
        return !m_threads.empty();
    }
    // File `index` (eligible), once read, waiting for it in slices (watching for a cancel);
    // nothing once the upload is cancelled.
    std::optional<Result> take(int index)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            const auto found = m_slots.find(index);
            if (found != m_slots.end() && found->second.done) {
                Result result = std::move(found->second.result);
                m_used -= found->second.cost;
                m_slots.erase(found);
                lock.unlock();
                m_room.notify_all();
                return result;
            }
            if (m_stopping || m_cancelled())
                return std::nullopt;
            m_ready.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
    // The helpers stop at their next file, and are waited for.
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_room.notify_all();
        m_ready.notify_all();
        for (std::thread &t : m_threads)
            t.join();
        m_threads.clear();
    }

private:
    struct Slot {
        bool done = false;
        qint64 cost = 0;
        Result result;
    };
    static qint64 costOf(const Offer::File &f) { return std::max(f.size, FileTransfer::kReadAheadSlot); }

    void loop()
    {
        detail::AsUserBatch batch(m_impersonate, m_identity, m_anchor);
        for (;;) {
            int index = -1;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                for (;;) {
                    while (m_next < m_files.size() && !eligible(m_files.at(m_next)))
                        ++m_next;
                    if (m_stopping || m_next >= m_files.size() || m_cancelled())
                        return;
                    const qint64 cost = costOf(m_files.at(m_next));
                    if (m_used == 0 || m_used + cost <= m_budget) {
                        index = int(m_next++);
                        m_used += cost;
                        m_slots[index].cost = cost;
                        break;
                    }
                    // Over the budget: wait for the reader, never with the user's rights.
                    lock.unlock();
                    batch.release();
                    lock.lock();
                    m_room.wait_for(lock, std::chrono::milliseconds(100));
                }
            }
            Result result = read(m_files.at(index), batch);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                Slot &slot = m_slots[index];
                slot.result = std::move(result);
                slot.done = true;
            }
            m_ready.notify_all();
        }
    }

    Result read(const Offer::File &f, detail::AsUserBatch &batch)
    {
        Result result;
        // As the console user, never as LocalSystem, like the reader's own reads (see run()).
        switch (batch.acquire()) {
        case detail::AsUserBatch::Status::Ok:
            break;
        case detail::AsUserBatch::Status::NoUser:
            result.status = Status::NoUser;
            return result;
        case detail::AsUserBatch::Status::UserChanged:
            result.status = Status::UserChanged;
            return result;
        }
        QFile file(f.absolutePath);
        if (!file.open(QIODevice::ReadOnly)) {
            result.status = Status::OpenFailed;
            return result;
        }
        result.data.resize(qsizetype(f.size));
        for (qint64 offset = 0; offset < f.size;) {
            const qint64 got = file.read(result.data.data() + offset, f.size - offset);
            if (got <= 0) {
                result.status = Status::Changed;
                result.data.clear();
                return result;
            }
            offset += got;
        }
        return result;
    }

    const QList<Offer::File> &m_files;
    const qint64 m_budget;
    const Impersonator m_impersonate;
    const std::function<QString()> m_identity;
    const std::shared_ptr<detail::IdentityAnchor> m_anchor;
    const std::function<bool()> m_cancelled;
    std::mutex m_mutex;
    std::condition_variable m_room;  // a file was taken, or stop
    std::condition_variable m_ready; // a file was read, or stop
    std::map<int, Slot> m_slots;     // claimed and not taken yet
    qsizetype m_next = 0;            // the next file to look at for a claim
    qint64 m_used = 0;               // the cost of m_slots
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
};
} // namespace

// --- Serving, the reader thread ----------------------------------------------------------------
//
// An upload is split like a fetch: its reader thread builds the manifest and reads the blobs and
// files, as the console user, into a bounded queue of frames ready to send (UploadJob); this
// object's thread sends them while the stream takes them (drainUpload), on the stream's
// bytesWritten and on the reader's (coalesced) "frames ready" post. Data frames are packed
// (protocol 3): every one holds kMaxFrame - 1 bytes of the items one after another, a frame
// often ending one file and starting the next; only the last is shorter. A Node's thread costs
// about the same per frame whatever its size, so many small files cost half the frames they did.

struct FileTransfer::UploadJob {
    enum State { Running, Finished, Cancelled };
    struct Out {
        QByteArray frame;
        bool last = false; // end or error: the stream is closed once it is sent
    };

    // Set before the reader starts, read-only afterwards.
    quint64 handle = 0;
    Offer offer;
    Impersonator impersonate;
    std::function<QString()> identity;
    std::shared_ptr<Shared> shared;
    bool readAhead = true; // small files are read ahead (ReadAhead)

    // Shared by both threads.
    detail::BoundedQueue<Out> queue{kUploadQueueHigh, kUploadQueueLow}; // its notify flag: "frames ready"
    std::atomic<int> state{Running};

    bool isCancelled() const { return state.load() == Cancelled; }
    // From any thread: the reader stops at its next file or frame (a wait for room included).
    void cancel()
    {
        int expected = Running;
        state.compare_exchange_strong(expected, Cancelled);
        queue.close();
    }

    void run();
    QList<QByteArray> manifestFrames() const;
    // Queues a frame for this object's thread, waiting for room (without the user's rights) when
    // the queue is full. False once the upload is cancelled.
    bool send(const QByteArray &frame, detail::AsUserBatch &batch, bool last = false);
    void fail(const QString &reason, detail::AsUserBatch &batch);
    bool asUser(detail::AsUserBatch &batch);
};

QList<QByteArray> FileTransfer::UploadJob::manifestFrames() const
{
    // Each item is encoded once, and the frames are put together around the encoded items.
    auto item = [](const char *kind, const QString &name, qint64 size) {
        QCborMap m;
        m.insert(QStringLiteral("k"), QString::fromLatin1(kind));
        m.insert(QStringLiteral("n"), name);
        m.insert(QStringLiteral("s"), size);
        return QCborValue(m).toCbor();
    };
    // {t: "manifest", pv: protocol, items: [...], more: bool}.
    QList<QByteArray> frames;
    QByteArray items;
    quint64 count = 0;
    auto flush = [&](bool more) {
        QByteArray frame(1, char(kControl));
        frame.reserve(items.size() + kManifestSlack);
        cborHead(frame, 5, 4);
        cborText(frame, QLatin1String("t"));
        cborText(frame, QLatin1String("manifest"));
        cborText(frame, QLatin1String("pv"));
        cborHead(frame, 0, quint64(kProtocol));
        cborText(frame, QLatin1String("items"));
        cborHead(frame, 4, count);
        frame.append(items);
        cborText(frame, QLatin1String("more"));
        frame.append(char(more ? 0xf5 : 0xf4)); // true, false
        frames.append(frame);
        items.clear();
        count = 0;
    };
    // Split over as many frames as needed (a frame holds at most kMaxFrame bytes).
    auto add = [&](const QByteArray &encoded) {
        if (count > 0 && items.size() + encoded.size() > SecureStream::kMaxFrame - kManifestSlack)
            flush(true);
        items.append(encoded);
        ++count;
    };
    for (auto b = offer.blobs.constBegin(); b != offer.blobs.constEnd(); ++b)
        add(item("blob", b.key(), b.value().size()));
    for (const QString &d : offer.dirs) // no data: the order of the data is unchanged
        add(item("dir", d, 0));
    for (const Offer::File &f : std::as_const(offer.files))
        add(item("file", f.relativePath, f.size));
    flush(false);
    return frames;
}

bool FileTransfer::UploadJob::send(const QByteArray &frame, detail::AsUserBatch &batch, bool last)
{
    if (isCancelled())
        return false;
    const qint64 bytes = frame.size();
    if (!queue.tryPush(Out{frame, last}, bytes)) {
        batch.release(); // never the user's rights while waiting
        if (!queue.push(Out{frame, last}, bytes))
            return false;
    }
    if (last) {
        int expected = Running;
        state.compare_exchange_strong(expected, Finished);
    }
    if (queue.setNotifyPending())
        shared->post([h = handle](FileTransfer *ft) { ft->drainUpload(h); });
    return true;
}

void FileTransfer::UploadJob::fail(const QString &reason, detail::AsUserBatch &batch)
{
    QCborMap err = typed(QStringLiteral("error"));
    err.insert(QStringLiteral("r"), reason);
    send(control(err), batch, true);
}

bool FileTransfer::UploadJob::asUser(detail::AsUserBatch &batch)
{
    switch (batch.acquire()) {
    case detail::AsUserBatch::Status::Ok:
        return true;
    case detail::AsUserBatch::Status::NoUser:
        fail(QStringLiteral("Nobody is signed in"), batch);
        return false;
    case detail::AsUserBatch::Status::UserChanged:
        fail(QStringLiteral("The signed-in user changed"), batch);
        return false;
    }
    return false;
}

void FileTransfer::UploadJob::run()
{
    // One identity for the reader and its read-ahead helpers: the first any of them sees.
    const auto anchor = std::make_shared<detail::IdentityAnchor>();
    detail::AsUserBatch batch(impersonate, identity, anchor);
    const QList<QByteArray> manifest = manifestFrames();
    for (const QByteArray &frame : manifest) {
        if (!send(frame, batch))
            return;
    }
    // The data frame being filled: its kind byte, then the items one after another. Built in
    // place (files are read straight into it) and sent once full.
    constexpr qsizetype kFrame = SecureStream::kMaxFrame;
    QByteArray frame;
    auto fresh = [&frame] {
        frame = QByteArray();
        frame.reserve(kFrame);
        frame.append(char(kData));
    };
    fresh();
    auto flush = [&]() {
        if (frame.size() <= 1)
            return true;
        if (!send(frame, batch))
            return false;
        shared->dataFramesSent.fetch_add(1, std::memory_order_relaxed);
        fresh();
        return true;
    };
    // Blobs first, then the files, in manifest order.
    for (auto b = offer.blobs.constBegin(); b != offer.blobs.constEnd(); ++b) {
        const QByteArray &blob = b.value();
        for (qsizetype offset = 0; offset < blob.size();) {
            const qsizetype n = std::min(kFrame - frame.size(), blob.size() - offset);
            frame.append(blob.constData() + offset, n);
            offset += n;
            if (frame.size() == kFrame && !flush())
                return;
        }
    }
    // Small files are read ahead when there are a few of them (see ReadAhead); the helpers stop
    // and are waited for whenever this returns.
    const auto small = std::count_if(offer.files.cbegin(), offer.files.cend(), ReadAhead::eligible);
    ReadAhead ahead(offer.files, kUploadQueueHigh, impersonate, identity, anchor, [this] { return isCancelled(); });
    const bool reading = readAhead && small >= 2 && ahead.start(kReadAheadThreads);
    for (qsizetype i = 0; i < offer.files.size(); ++i) {
        const Offer::File &f = offer.files.at(i); // const: the list is never detached here
        if (isCancelled())
            return;
        if (reading && ReadAhead::eligible(f)) {
            // Read by a helper (or being read): taken in manifest order, then packed like any
            // data. Its errors are the ones this reader gives for a file it reads itself.
            QElapsedTimer waited;
            waited.start();
            std::optional<ReadAhead::Result> got = ahead.take(int(i));
            shared->readAheadWaitNs.fetch_add(waited.nsecsElapsed(), std::memory_order_relaxed);
            if (!got)
                return; // cancelled
            switch (got->status) {
            case ReadAhead::Status::Ok:
                break;
            case ReadAhead::Status::OpenFailed:
                fail(QStringLiteral("A file could not be read: %1").arg(f.relativePath), batch);
                return;
            case ReadAhead::Status::Changed:
                fail(QStringLiteral("File changed while sending: %1").arg(f.relativePath), batch);
                return;
            case ReadAhead::Status::NoUser:
                fail(QStringLiteral("Nobody is signed in"), batch);
                return;
            case ReadAhead::Status::UserChanged:
                fail(QStringLiteral("The signed-in user changed"), batch);
                return;
            }
            shared->readAheadFiles.fetch_add(1, std::memory_order_relaxed);
            const QByteArray &data = got->data;
            for (qsizetype offset = 0; offset < data.size();) {
                const qsizetype n = std::min(kFrame - frame.size(), data.size() - offset);
                frame.append(data.constData() + offset, n);
                offset += n;
                if (frame.size() == kFrame && !flush())
                    return;
            }
            continue;
        }
        // Offered files are opened and read as the console user, never as LocalSystem: whatever
        // the path leads to, it is only what that user may read, so no link check is needed on
        // this side.
        if (!asUser(batch))
            return;
        QFile file(f.absolutePath);
        if (!file.open(QIODevice::ReadOnly)) {
            // Named, so the user knows which file to close or leave out.
            fail(QStringLiteral("A file could not be read: %1").arg(f.relativePath), batch);
            return;
        }
        for (qint64 offset = 0; offset < f.size;) {
            if (!asUser(batch))
                return;
            const qsizetype used = frame.size();
            const qsizetype n = qsizetype(std::min<qint64>(kFrame - used, f.size - offset));
            frame.resize(used + n);
            const qint64 got = file.read(frame.data() + used, n);
            if (got <= 0) {
                fail(QStringLiteral("File changed while sending: %1").arg(f.relativePath), batch);
                return;
            }
            frame.resize(used + qsizetype(got));
            offset += got;
            if (frame.size() == kFrame && !flush())
                return;
        }
    }
    if (!flush())
        return;
    batch.release();
    send(control(typed(QStringLiteral("end"))), batch, true);
}

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
        // Unknown, gated and not let in, or a guest not let in: all "No such offer" (a peer that
        // may not fetch it learns nothing about it either).
        if (it == m_offers.end()
            || ((it->second.gated || m_node->isGuest(stream->peerId()))
                && !it->second.allowed.contains(stream->peerId()))) {
            QCborMap err = typed(QStringLiteral("error"));
            err.insert(QStringLiteral("r"), QStringLiteral("No such offer"));
            stream->send(control(err));
            stream->close();
            return;
        }
        auto refuse = [stream](const QString &reason) {
            QCborMap err = typed(QStringLiteral("error"));
            err.insert(QStringLiteral("r"), reason);
            stream->send(control(err));
            stream->close();
        };
        const QByteArray peer = stream->peerId();
        const auto sameOffer = std::count_if(m_uploads.begin(), m_uploads.end(), [&](const auto &entry) {
            return entry.second->offerId == offerId && entry.second->peer == peer;
        });
        if (sameOffer >= kMaxUploadsPerOfferAndPeer) {
            refuse(QStringLiteral("Too many transfers of these files at once"));
            return;
        }
        // A receiver of another protocol would misread the frames: refused, with the reason as
        // its transfer error (an older one shows it as it is). Said from the receiver's side:
        // this device is the one of the other protocol.
        if (const int theirs = getProtocol(msg); theirs != kProtocol) {
            qCInfo(lcTransfer) << "Refused a transfer to a device of protocol" << theirs;
            refuse(theirs < kProtocol
                       ? QStringLiteral("%1 uses a newer transfer protocol (version %2). The protocol has changed: "
                                        "update this device.")
                             .arg(m_node->name())
                             .arg(kProtocol)
                       : QStringLiteral("%1 uses an older transfer protocol (version %2). The protocol has changed: "
                                        "update it.")
                             .arg(m_node->name())
                             .arg(kProtocol));
            return;
        }
        const quint64 handle = m_nextHandle++;
        auto job = std::make_shared<UploadJob>();
        job->handle = handle;
        job->offer = it->second; // implicitly shared: a copy of the lists, never written by the reader
        job->offer.allowed.clear();
        if (!msg.value(QStringLiteral("files")).toBool()) {
            job->offer.files.clear();
            job->offer.dirs.clear();
        }
        job->impersonate = m_impersonate;
        job->identity = m_identity;
        job->shared = m_shared;
        job->readAhead = m_readAhead;

        auto *up = new Upload;
        up->stream = stream;
        up->job = job;
        up->peer = peer;
        up->offerId = offerId;
        m_uploads[handle] = up;
        connect(stream, &SecureStream::bytesWritten, this, [this, handle] { drainUpload(handle); });
        // The receiver went away (or the stream failed): the reader stops at its next file or
        // frame, and the job lives on with it until it returns.
        connect(stream, &SecureStream::closed, this, [this, handle] {
            auto found = m_uploads.find(handle);
            if (found == m_uploads.end())
                return;
            Upload *gone = found->second;
            m_uploads.erase(found);
            gone->job->cancel();
            delete gone;
        });

        {
            std::lock_guard<std::mutex> lock(m_shared->mutex);
            m_shared->uploads[handle] = job;
        }
        try {
            std::thread([job] {
                job->run();
                const std::shared_ptr<Shared> shared = job->shared;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->uploads.erase(job->handle);
                }
                shared->exited.notify_all();
            }).detach();
        } catch (const std::system_error &) {
            // No thread to be had (resources exhausted): this upload is refused, nothing else.
            {
                std::lock_guard<std::mutex> lock(m_shared->mutex);
                m_shared->uploads.erase(handle);
            }
            m_uploads.erase(handle);
            delete up;
            refuse(QStringLiteral("The transfer could not be started"));
        }
    });
}

void FileTransfer::drainUpload(quint64 uploadHandle)
{
    auto found = m_uploads.find(uploadHandle);
    if (found == m_uploads.end())
        return;
    Upload *up = found->second;
    const std::shared_ptr<UploadJob> job = up->job;
    // Cleared before draining: a frame queued from now on is either sent below or posted again.
    job->queue.clearNotifyPending();
    const QPointer<SecureStream> stream = up->stream;
    if (!stream || up->finished)
        return;
    while (stream->pendingBytes() < kHighWater) {
        std::optional<UploadJob::Out> out = job->queue.pop(0);
        if (!out)
            return;
        if (out->last)
            up->finished = true;
        if (!stream->send(out->frame)) {
            // Not sent (the stream is no longer established, or the frame is too large): the
            // receiver would get a broken transfer. Dropped at once instead; `closed` cancels the
            // reader and deletes the upload.
            stream->abort(QStringLiteral("A frame could not be sent"));
            return;
        }
        if (!stream || !m_uploads.count(uploadHandle))
            return;
        if (out->last) {
            stream->close(); // may delete the upload too (closed)
            return;
        }
    }
}

int FileTransfer::liveUploadsForTests() const
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    return int(m_shared->uploads.size());
}

QList<QByteArray> FileTransfer::manifestFramesForTests(const Offer &offer)
{
    UploadJob job;
    job.offer = offer;
    return job.manifestFrames();
}

// --- Fetching ----------------------------------------------------------------------------------
//
// A fetch is split between this object's thread and a worker thread of its own (DownloadJob).
// This thread opens the stream and sends the get once the worker is ready, hands every received
// frame to the worker's queue (pausing the stream above kReceiveQueueHigh), runs the idle timer
// and reports progress. The worker does all the disk work, as the console user: what to make of
// each frame, the folders, the files, the free space, and the cleanup after a failure. It posts
// back to this thread: ready, heartbeats (busy on the disk: not idle), "resume reading", progress
// and the end. Either side may end a fetch: the first to switch its state from running wins. A
// worker that wins reports its end before any cleanup, and after a success waits to hear whether
// it was delivered or came too late (cancelled meanwhile): then it removes what it wrote itself.
// Until the worker returns, its fetch counts as at work (isActiveTarget).

namespace {
// The longest a busy worker goes without a heartbeat (or a quarter of the idle timeout, if that
// is shorter: tests).
constexpr int kBeatMs = 500;
// How long a worker waits for a frame before it looks at its state again.
constexpr int kPopWaitMs = 1000;
// A short wait for more work with the user's rights still held: input that arrives in bursts (a
// fast disk empties a queue between two frames) does not cost a guard per file. A longer wait is
// never made with the user's rights.
constexpr int kHeldWaitMs = 5;
// The slices in which a fetch's worker waits for its writers (room in their queues, their end).
constexpr int kWriterWaitMs = 100;

const QString kCancelled = QStringLiteral("Cancelled");
} // namespace

namespace detail {
// A manifest as the receiving side reads it, frame by frame (a split manifest is several): the
// one set of checks for a fetch (on its worker) and for a list fetch (FileTransfer::fetchList, on
// the owning thread). The bound on the number of items holds across the frames; every name is
// sanitised; the announced sizes are bounded by maxBytes; and, on a file system that ignores case,
// two paths that differ only in case are refused once the whole manifest is in.
struct Manifest {
    struct Item {
        bool blob = false;
        QString name;
        qint64 size = 0;
    };

    explicit Manifest(qint64 maxBytes = 0) : maxBytes(maxBytes) {}

    // One manifest frame ({items, more}). False, with the error, if it is refused; `complete` once
    // the last frame is in.
    bool add(const QCborMap &msg, QString *error);
    // The files and folders, folders first (as Offer::listing), the blobs left out.
    QList<ListedFile> listing() const;

    qint64 maxBytes = 0;
    QString peerName;     // the publisher, for the error of a protocol mismatch
    QList<Item> items;    // blobs and files, in the order of their data
    QStringList dirs;     // folders, relative
    qint64 total = 0;     // bytes announced
    int fileCount = 0;    // files announced
    qint64 fileBytes = 0; // their total size (what goes to disk; blobs stay in memory)
    bool complete = false;
    int count = 0; // items across the frames of a split manifest
};

bool Manifest::add(const QCborMap &msg, QString *error)
{
    // Every frame says its protocol: a publisher of another one (0.1.30 and before send none)
    // sends data this device would misread.
    const int protocol = msg.contains(QStringLiteral("pv"))
                             ? int(std::clamp<qint64>(msg.value(QStringLiteral("pv")).toInteger(), 1, 1000))
                             : 2;
    if (protocol != kProtocol) {
        *error = mismatchText(peerName.isEmpty() ? QStringLiteral("The other device") : peerName, protocol);
        return false;
    }
    const QCborArray list = msg.value(QStringLiteral("items")).toArray();
    // Counted across the frames of a split manifest: the bound holds for the whole of it.
    count += int(std::min<qsizetype>(list.size(), FileTransfer::kMaxItems + 1));
    if (count > FileTransfer::kMaxItems) {
        *error = QStringLiteral("Too many items in manifest");
        return false;
    }
    for (const QCborValue &v : list) {
        const QCborMap m = v.toMap();
        const QString k = m.value(QStringLiteral("k")).toString();
        if (k == QLatin1String("dir")) {
            const QString name = FileTransfer::sanitizeRelativePath(m.value(QStringLiteral("n")).toString());
            if (name.isEmpty()) {
                *error = QStringLiteral("Unsafe or invalid item in manifest");
                return false;
            }
            dirs.append(name);
            continue;
        }
        Item it;
        it.blob = k == QLatin1String("blob");
        it.name = it.blob ? m.value(QStringLiteral("n")).toString()
                          : FileTransfer::sanitizeRelativePath(m.value(QStringLiteral("n")).toString());
        it.size = m.value(QStringLiteral("s")).toInteger();
        if (it.name.isEmpty() || it.size < 0) {
            *error = QStringLiteral("Unsafe or invalid item in manifest");
            return false;
        }
        // Overflow-safe: never let the announced sizes wrap the signed sum past the limit.
        if (it.size > maxBytes - total) {
            *error = QStringLiteral("Transfer larger than allowed");
            return false;
        }
        total += it.size;
        if (!it.blob) {
            fileCount++;
            fileBytes += it.size;
        }
        items.append(it);
    }
    if (msg.value(QStringLiteral("more")).toBool())
        return true; // the rest of the manifest follows
    complete = true;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // On a file system that ignores case, the second of two paths that differ only in case
    // would truncate the first (copied from a Linux machine, for instance): refused instead.
    QHash<QString, QString> seen;
    seen.reserve(dirs.size() + fileCount);
    auto clash = [&](const QString &path) {
        const QString key = path.toCaseFolded();
        const auto found = seen.constFind(key);
        if (found == seen.constEnd()) {
            seen.insert(key, path);
            return false;
        }
        return *found != path; // the very same path twice is no clash
    };
    for (const QString &dir : std::as_const(dirs)) {
        if (clash(dir)) {
            *error = QStringLiteral("Two copied items differ only in upper and lower case: %1").arg(dir);
            return false;
        }
    }
    for (const Item &it : std::as_const(items)) {
        if (!it.blob && clash(it.name)) {
            *error = QStringLiteral("Two copied items differ only in upper and lower case: %1").arg(it.name);
            return false;
        }
    }
#endif
    return true;
}

QList<ListedFile> Manifest::listing() const
{
    QList<ListedFile> list;
    list.reserve(dirs.size() + fileCount);
    for (const QString &d : dirs)
        list.append(ListedFile{d, 0, true});
    for (const Item &it : items) {
        if (!it.blob)
            list.append(ListedFile{it.name, it.size, false});
    }
    return list;
}
} // namespace detail

// --- Receiving: the dispatcher and its writers --------------------------------------------------
//
// The fetch's worker thread (the dispatcher) takes the frames, the manifest, the free space and the
// folders, and splits the packed data between the files; the files themselves are created, written
// and closed by a few writer threads of the fetch (robocopy /MT style: what a tree of small files
// waits for is creating and closing them, which overlaps). Every operation on one file goes to one
// writer, in order, so a file's size on disk only grows; each file goes to the writer with the
// fewest bytes queued, in manifest order, which keeps a reader that follows the manifest (a paste
// served while it arrives) near the front. The writers' queues share a byte budget: above it the
// dispatcher waits (without the user's rights, beating), and its own receive queue fills and pauses
// the stream as before. Each writer works as the console user (an AsUserBatch of its own, checked
// against the identity the job recorded first: IdentityAnchor) and releases those rights before
// every wait. The first failure wins, names the file, and stops the others.

namespace {
// One step of a file for its writer: create it (path), write (data), close it (close).
struct WriteOp {
    int item = 0;    // index in the manifest's items (for the error's name)
    QString path; // non-empty: create the file first
    // Then write `length` bytes of the received frame from `offset`: the frame is shared, the data
    // is not copied on its way to the writer.
    QByteArray frame;
    qsizetype offset = 0;
    qsizetype length = 0;
    bool close = false;
};
} // namespace

struct FileTransfer::DownloadJob : std::enable_shared_from_this<DownloadJob> {
    enum State { Running, Finishing, Cancelled };
    // What became of a success the worker reported (set by this object's thread, or at shutdown).
    enum Verdict { Pending, Delivered, Discarded };
    struct Msg {
        enum Kind { Frame, Closed, Abort };
        Kind kind = Frame;
        QByteArray data; // Frame
        QString reason;  // Closed, Abort
    };
    using Item = detail::Manifest::Item;
    struct Stats {
        qint64 received = 0;
        qint64 total = 0;
        int filesDone = 0;
        int fileCount = 0;
    };

    // Set before the worker starts, read-only afterwards.
    quint64 handle = 0;
    QString targetDir;
    qint64 maxBytes = 0;
    bool visible = false; // fetched with a kind: progress is posted
    Impersonator impersonate;
    std::function<QString()> identity;
    std::shared_ptr<detail::IdentityAnchor> anchor = std::make_shared<detail::IdentityAnchor>();
    std::function<void()> prepareAsUser;
    std::function<qint64(const QString &)> freeSpace;
    int beatMs = kBeatMs;
    int finishPauseBeforeMs = 0;
    int finishPauseAfterMs = 0;
    int writersWanted = 0; // tests: the number of writers (0: chosen from the manifest)
    std::shared_ptr<Shared> shared;

    // Shared by both threads (and the writers).
    detail::BoundedQueue<Msg> queue{kReceiveQueueHigh, kReceiveQueueLow}; // its notify flag: "resume"
    std::atomic<int> state{Running};
    std::atomic<bool> shutdown{false};   // the FileTransfer is going: stop, and remove nothing
    std::atomic<bool> readPaused{false}; // the stream is paused for this queue
    std::atomic<bool> beatPending{false};
    std::atomic<bool> progressPending{false};
    std::atomic<bool> progressForced{false};
    std::atomic<qint64> diskPending{0}; // file bytes announced and not written yet
    std::atomic<bool> created{false};   // files or folders were created under targetDir
    std::atomic<qint64> received{0};    // bytes taken from the stream, enforced against maxBytes
    std::atomic<qint64> progressed{0};  // bytes written (by the writers) or kept (blobs): progress
    std::atomic<int> filesDone{0};      // files written and closed
    std::atomic<int> manifestFiles{0};  // files announced (once the manifest is in)
    std::atomic<qint64> manifestTotal{0};
    // This fetch's share of the benchmark counters (Shared), for its log line at the end.
    std::atomic<qint64> writerBusyNs{0};
    std::atomic<qint64> writerRoomWaitNs{0};
    qint64 prepareMs = -1;  // how long prepare() took (the dispatcher's own)
    QElapsedTimer started;  // since the worker started
    // The cleanup after prepareAsUser (FetchOptions::cleanupAsUser): pending until it has run (or
    // was skipped); the free space check waits for it before refusing a copy.
    std::function<void()> cleanupAsUser;
    std::mutex cleanupMutex;
    std::condition_variable cleanupDone;
    bool cleanupPending = false;
    std::mutex verdictMutex;
    std::condition_variable verdictSet;
    Verdict verdict = Pending;

    // The writers (see above): their queues, under writerMutex.
    std::mutex writerMutex;
    std::condition_variable writerWake; // an operation, the end of input, or stop
    std::condition_variable writerRoom; // bytes left a queue, a writer failed or finished
    std::vector<std::deque<WriteOp>> writerQueues;
    std::vector<qint64> writerBytes; // queued and in progress, per writer
    std::vector<std::thread> writers;
    qint64 writerQueued = 0; // the sum of writerBytes
    bool inputClosed = false;
    bool stopWriters = false;
    QString writerError; // the first failure
    std::atomic<bool> writerFailed{false};
    std::atomic<int> writersBusy{0};
    int writerTotal = 0; // the writers started (the dispatcher's, for the log)

    // The dispatcher's own.
    detail::Manifest manifest; // its folders are created relative to targetDir
    int item = 0;
    qint64 offset = 0;
    int currentWriter = -1; // the writer of the file in progress (-1: not started)
    // With several writers, the writer of every file path started: the same path twice in a
    // manifest (two files of one name copied from different folders) goes to the same writer, in
    // order, so the second replaces the first as it always did and both are never written at once.
    QHash<QString, int> pathWriters;
    FetchResult result;
    QSet<QString> tops; // result.files, for lookups
    QElapsedTimer sinceBeat;
    // targetDir and the folders made below it, held open from prepare() to finish(): no link can
    // redirect a file, and no link check is made per file.
    fs::SafeTree tree;

    bool isCancelled() const { return state.load() == Cancelled; }
    Stats statsNow() const
    {
        return Stats{progressed.load(), manifestTotal.load(), filesDone.load(), manifestFiles.load()};
    }
    void setVerdict(Verdict v)
    {
        {
            std::lock_guard<std::mutex> lock(verdictMutex);
            if (verdict == Pending)
                verdict = v;
        }
        verdictSet.notify_all();
    }
    // Waits for setVerdict (the destructor sets one for every live job, with shutdown).
    Verdict awaitVerdict()
    {
        std::unique_lock<std::mutex> lock(verdictMutex);
        verdictSet.wait(lock, [this] { return verdict != Pending || shutdown.load(); });
        return verdict;
    }

    void run();
    bool prepare(detail::AsUserBatch &batch, QString *error);
    bool receive(detail::AsUserBatch &batch, QString *error);
    bool handleFrame(const QByteArray &frame, detail::AsUserBatch &batch, bool *ended, QString *error);
    bool onManifest(const QCborMap &msg, detail::AsUserBatch &batch, QString *error);
    // Splits data between the current item(s), a packed frame between the items it holds: blobs
    // kept, files handed to their writers; also completes zero-length items. False on error.
    bool consume(const QByteArray &frame, qsizetype start, detail::AsUserBatch &batch, QString *error);
    // Creates the announced folders under the target folder (once the whole manifest is in).
    bool createDirs(detail::AsUserBatch &batch, QString *error);
    void finish(detail::AsUserBatch &batch, bool ok, const QString &error);
    static bool asUser(detail::AsUserBatch &batch, QString *error);
    void beat();
    void updateStats(bool force);
    // Waits, beating and watching for a cancel, until the cleanup after prepareAsUser is over.
    // False if the fetch was cancelled or shut down first.
    bool awaitCleanup();
    void endCleanup();

    // Writers.
    int writerCount() const;
    bool startWriters(int count, QString *error);
    // Queues `op` for writer `w`, waiting (without the user's rights) while the queues are over
    // their budget. False, with the error, once the fetch failed or was cancelled.
    bool dispatch(int w, WriteOp op, detail::AsUserBatch &batch, QString *error);
    // The end of the data: waits until every writer has written and closed its files.
    bool drainWriters(detail::AsUserBatch &batch, QString *error);
    // Stops the writers, dropping what they have not done, and waits for them (each closes the
    // file it has open). Nothing if none run.
    void joinWriters(bool drop);
    void writerLoop(int w);
    void failWriters(const QString &error);
    void dropQueuedLocked(); // under writerMutex
    // A reason to stop waiting for the writers: cancelled, shut down or failed (the error).
    bool writersStopped(QString *error);
};

bool FileTransfer::Shared::isActive(const QString &dir) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto &[handle, job] : live) {
        if (fs::isInside(job->targetDir, dir))
            return true;
    }
    return false;
}

bool FileTransfer::DownloadJob::asUser(detail::AsUserBatch &batch, QString *error)
{
    switch (batch.acquire()) {
    case detail::AsUserBatch::Status::Ok:
        return true;
    case detail::AsUserBatch::Status::NoUser:
        *error = QStringLiteral("Nobody is signed in to receive files");
        return false;
    case detail::AsUserBatch::Status::UserChanged:
        *error = QStringLiteral("The signed-in user changed");
        return false;
    }
    return false;
}

void FileTransfer::DownloadJob::beat()
{
    if (sinceBeat.elapsed() < beatMs)
        return;
    sinceBeat.start();
    if (!beatPending.exchange(true))
        shared->post([h = handle](FileTransfer *ft) { ft->onJobBeat(h); });
}

void FileTransfer::DownloadJob::updateStats(bool force)
{
    // From the dispatcher and the writers: the numbers are atomics, the post is coalesced.
    if (!visible)
        return;
    if (force)
        progressForced.store(true);
    if (!progressPending.exchange(true))
        shared->post([h = handle](FileTransfer *ft) { ft->onJobProgress(h); });
}

void FileTransfer::DownloadJob::run()
{
    detail::AsUserBatch batch(impersonate, identity, anchor);
    sinceBeat.start();
    started.start();
    QString error;
    bool ok = prepare(batch, &error);
    batch.release();
    prepareMs = started.elapsed();
    if (prepareMs >= 200)
        qCInfo(lcTransfer) << "Transfer" << handle << "prepared in" << prepareMs << "ms";
    if (ok)
        shared->post([h = handle](FileTransfer *ft) { ft->onJobReady(h); });
    // The slow part of making room, once prepareAsUser has run (also when the fetch goes no
    // further): on the pool, after the stream was asked for, alongside the transfer. Pending until
    // it has run; dropped (and so never awaited) if this object goes first, which shuts the fetch
    // down too.
    bool cleanup = false;
    {
        std::lock_guard<std::mutex> lock(cleanupMutex);
        cleanup = cleanupPending;
    }
    if (cleanup)
        shared->post([job = shared_from_this()](FileTransfer *ft) { ft->startCleanup(job); });
    if (ok)
        ok = receive(batch, &error);
    finish(batch, ok, error);
}

bool FileTransfer::DownloadJob::awaitCleanup()
{
    std::unique_lock<std::mutex> lock(cleanupMutex);
    while (cleanupPending) {
        if (isCancelled() || shutdown.load())
            return false;
        cleanupDone.wait_for(lock, std::chrono::milliseconds(kWriterWaitMs));
        lock.unlock();
        beat(); // busy making room: not idle
        lock.lock();
    }
    return true;
}

void FileTransfer::DownloadJob::endCleanup()
{
    {
        std::lock_guard<std::mutex> lock(cleanupMutex);
        cleanupPending = false;
    }
    cleanupDone.notify_all();
}

bool FileTransfer::DownloadJob::prepare(detail::AsUserBatch &batch, QString *error)
{
    // The first batch as the console user: it also records who that is.
    if (!asUser(batch, error))
        return false;
    if (prepareAsUser) {
        // Whatever it set aside is deleted by the cleanup, which follows it whatever happens next.
        if (cleanupAsUser) {
            std::lock_guard<std::mutex> lock(cleanupMutex);
            cleanupPending = true;
        }
        prepareAsUser();
    }
    batch.release();
    if (isCancelled()) {
        *error = kCancelled;
        return false;
    }
    // The service may write with more rights than the folder's owner: never through a link they
    // planted on the way. The folders of the path are checked once and held (fs::SafeTree), as
    // the console user.
    if (!asUser(batch, error))
        return false;
    QString treeError;
    if (!tree.open(targetDir, &treeError)) {
        *error = treeError;
        return false;
    }
    return true;
}

bool FileTransfer::DownloadJob::receive(detail::AsUserBatch &batch, QString *error)
{
    for (;;) {
        if (isCancelled()) {
            *error = kCancelled;
            return false;
        }
        if (writerFailed.load() && writersStopped(error))
            return false;
        std::optional<Msg> msg = queue.pop(0);
        if (!msg)
            msg = queue.pop(kHeldWaitMs);
        if (!msg) {
            batch.release(); // never the user's rights while waiting
            msg = queue.pop(kPopWaitMs);
            if (!msg)
                continue;
        }
        // Brought down to the low-water mark: the stream may read again (one post at a time).
        if (readPaused.load() && queue.isLow() && queue.setNotifyPending())
            shared->post([h = handle](FileTransfer *ft) { ft->onJobResume(h); });
        if (msg->kind != Msg::Frame) { // the stream closed, or the fetch timed out
            *error = msg->reason;
            return false;
        }
        bool ended = false;
        if (!handleFrame(msg->data, batch, &ended, error))
            return false;
        if (ended)
            return drainWriters(batch, error);
        beat();
    }
}

bool FileTransfer::DownloadJob::handleFrame(const QByteArray &frame, detail::AsUserBatch &batch, bool *ended,
                                            QString *error)
{
    if (frame.isEmpty())
        return true;
    // Every folder is created as the console user, never as LocalSystem (and every file, by the
    // writers).
    if (!asUser(batch, error))
        return false;
    if (quint8(frame[0]) == kData) {
        if (!manifest.complete) {
            *error = QStringLiteral("Data before manifest");
            return false;
        }
        shared->dataFramesReceived.fetch_add(1, std::memory_order_relaxed);
        return consume(frame, 1, batch, error);
    }
    if (quint8(frame[0]) == kOldData) {
        *error = QStringLiteral("Data of an older transfer protocol");
        return false;
    }
    if (quint8(frame[0]) != kControl)
        return true;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(QStringLiteral("t")).toString();
    if (t == QLatin1String("manifest") && !manifest.complete)
        return onManifest(msg, batch, error);
    if (t == QLatin1String("end")) {
        if (!consume({}, 0, batch, error))
            return false;
        if (!manifest.complete || item < manifest.items.size()) {
            *error = QStringLiteral("Transfer ended early");
            return false;
        }
        *ended = true;
        return true;
    }
    if (t == QLatin1String("error")) {
        *error = msg.value(QStringLiteral("r")).toString();
        return false;
    }
    return true;
}

bool FileTransfer::DownloadJob::onManifest(const QCborMap &msg, detail::AsUserBatch &batch, QString *error)
{
    if (!manifest.add(msg, error))
        return false;
    if (!manifest.complete)
        return true; // the rest of the manifest follows
    // A copy that cannot fit is refused before anything is written (a large folder would
    // otherwise fill the disk and fail halfway). What the other fetches still have to write is
    // spoken for.
    // The older copies set aside to make room may still be on their way out (cleanupAsUser):
    // a copy that does not fit now waits for them, and is checked again, before it is refused.
    if (manifest.fileBytes > 0) {
        auto usableSpace = [this](qint64 *freeBytes) {
            *freeBytes = freeSpace ? freeSpace(targetDir) : freeSpaceFor(targetDir);
            std::lock_guard<std::mutex> lock(shared->mutex);
            qint64 others = 0;
            for (const auto &[h, job] : shared->live) {
                if (job.get() != this)
                    others += job->diskPending.load();
            }
            return *freeBytes - others;
        };
        qint64 freeBytes = 0;
        qint64 usable = usableSpace(&freeBytes);
        bool fits = freeBytes < 0 || manifest.fileBytes <= usable - kDiskReserve;
        if (!fits) {
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(cleanupMutex);
                pending = cleanupPending;
            }
            if (pending) {
                batch.release(); // never the user's rights while waiting
                qCInfo(lcTransfer) << "Transfer" << handle << "waits for older copies to be deleted before its free space check";
                if (!awaitCleanup()) {
                    *error = kCancelled;
                    return false;
                }
                usable = usableSpace(&freeBytes);
                fits = freeBytes < 0 || manifest.fileBytes <= usable - kDiskReserve;
            }
        }
        if (!fits) {
            const QLocale c = QLocale::c();
            *error = QStringLiteral("Not enough free disk space (%1 needed, %2 free)")
                         .arg(c.formattedDataSize(manifest.fileBytes),
                              c.formattedDataSize(std::max<qint64>(usable, 0)));
            return false;
        }
        diskPending.store(manifest.fileBytes);
    }
    manifestFiles.store(manifest.fileCount);
    manifestTotal.store(manifest.total);
    beat();
    if (!createDirs(batch, error))
        return false;
    if (manifest.fileCount > 0 && !startWriters(writerCount(), error))
        return false;
    updateStats(true);
    return consume({}, 0, batch, error); // leading zero-length items
}

bool FileTransfer::DownloadJob::consume(const QByteArray &frame, qsizetype start, detail::AsUserBatch &batch,
                                        QString *error)
{
    QByteArrayView data = QByteArrayView(frame).sliced(start);
    while (item < manifest.items.size()) {
        const Item &it = manifest.items.at(item);
        if (data.isEmpty() && it.size > offset)
            return true; // wait for more data
        const QByteArrayView part = data.first(qsizetype(std::min<qint64>(data.size(), it.size - offset)));
        data = data.sliced(part.size());
        // A running total independent of the announced sizes: the sender cannot exceed the limit.
        const qint64 total = received.fetch_add(part.size()) + part.size();
        if (total > maxBytes) {
            *error = QStringLiteral("Transfer larger than allowed");
            return false;
        }
        const bool complete = offset + part.size() == it.size;
        if (it.blob) {
            result.blobs[it.name].append(part);
            progressed.fetch_add(part.size());
        } else {
            WriteOp op;
            op.item = item;
            if (!part.isEmpty()) {
                op.frame = frame;
                op.offset = part.data() - frame.constData();
                op.length = part.size();
            }
            op.close = complete;
            if (currentWriter < 0) { // the file's first step: its folder, its writer
                if (isCancelled()) {
                    *error = kCancelled;
                    return false;
                }
                if (!asUser(batch, error))
                    return false;
                created.store(true);
                // Its folder, made (with its parents) and held open by the tree: a lookup when it
                // is held already. No folder on the way can have become a link since: held folders
                // cannot be renamed, deleted or replaced.
                const qsizetype slash = it.name.lastIndexOf(QLatin1Char('/'));
                QString treeError;
                if (!tree.ensureDir(slash < 0 ? QString() : it.name.left(slash), &treeError)) {
                    *error = treeError;
                    return false;
                }
                const QString top = QDir(targetDir).filePath(it.name.section(QLatin1Char('/'), 0, 0));
                if (!tops.contains(top)) {
                    tops.insert(top);
                    result.files.append(top);
                }
                op.path = QDir(targetDir).filePath(it.name);
                const auto same = writers.size() > 1 ? pathWriters.constFind(it.name) : pathWriters.constEnd();
                if (same != pathWriters.constEnd()) {
                    currentWriter = *same;
                } else {
                    {
                        std::lock_guard<std::mutex> lock(writerMutex);
                        currentWriter =
                            int(std::min_element(writerBytes.cbegin(), writerBytes.cend()) - writerBytes.cbegin());
                    }
                    if (writers.size() > 1)
                        pathWriters.insert(it.name, currentWriter);
                }
            }
            if (!dispatch(currentWriter, std::move(op), batch, error))
                return false;
        }
        offset += part.size();
        if (!complete) {
            updateStats(false);
            return true;
        }
        item++;
        offset = 0;
        currentWriter = -1;
        updateStats(false);
        beat();
    }
    if (!data.isEmpty()) {
        *error = QStringLiteral("More data than announced");
        return false;
    }
    return true;
}

bool FileTransfer::DownloadJob::createDirs(detail::AsUserBatch &batch, QString *error)
{
    // Parents come first in the manifest, but the tree does not depend on it. As for files: never
    // through a link on the way (fs::SafeTree checks each folder it makes and holds it), and as
    // the console user.
    for (const QString &dir : std::as_const(manifest.dirs)) {
        if (isCancelled()) {
            *error = kCancelled;
            return false;
        }
        if (!asUser(batch, error))
            return false;
        beat();
        created.store(true);
        QString treeError;
        if (!tree.ensureDir(dir, &treeError)) {
            *error = treeError;
            return false;
        }
        const QString top = QDir(targetDir).filePath(dir.section(QLatin1Char('/'), 0, 0));
        if (!tops.contains(top)) {
            tops.insert(top);
            result.files.append(top);
        }
    }
    return true;
}

int FileTransfer::DownloadJob::writerCount() const
{
    QList<qint64> sizes;
    sizes.reserve(manifest.fileCount);
    for (const Item &it : manifest.items) {
        if (!it.blob)
            sizes.append(it.size);
    }
    return writerCountFor(sizes, writersWanted);
}

int FileTransfer::writerCountFor(const QList<qint64> &fileSizes, int forced)
{
    if (forced > 0)
        return std::min(forced, kMaxWriters);
    // Many small files: several writers, as robocopy /MT does (what they wait for is creating and
    // closing files, which overlaps). Counted, not averaged: a tree of small files with one huge
    // file among them (an average far above a small file) gets them too, the huge file on one of
    // them while the others go on with the rest. A few files, or only large ones (also what a hard
    // disk wants): one writer.
    const auto small = std::count_if(fileSizes.cbegin(), fileSizes.cend(), [](qint64 size) { return size < kSmallFile; });
    if (small < kParallelSmallFiles)
        return 1;
    return std::clamp(int(std::thread::hardware_concurrency()) / 2, 2, kMaxWriters);
}

bool FileTransfer::DownloadJob::startWriters(int count, QString *error)
{
    {
        std::lock_guard<std::mutex> lock(writerMutex);
        writerQueues.resize(count);
        writerBytes.assign(count, 0);
    }
    const std::shared_ptr<DownloadJob> self = shared_from_this();
    for (int w = 0; w < count; ++w) {
        try {
            writers.emplace_back([self, w] { self->writerLoop(w); });
        } catch (const std::system_error &) {
            break; // no more threads to be had: the ones started do the work
        }
    }
    if (writers.empty()) {
        *error = QStringLiteral("The transfer could not be started");
        return false;
    }
    if (int(writers.size()) < count) {
        std::lock_guard<std::mutex> lock(writerMutex);
        writerQueues.resize(writers.size());
        writerBytes.resize(writers.size());
    }
    shared->lastWriters.store(int(writers.size()));
    writerTotal = int(writers.size());
    return true;
}

bool FileTransfer::DownloadJob::writersStopped(QString *error)
{
    if (isCancelled() || shutdown.load()) {
        *error = kCancelled;
        return true;
    }
    if (writerFailed.load()) {
        std::lock_guard<std::mutex> lock(writerMutex);
        *error = writerError;
        return true;
    }
    return false;
}

bool FileTransfer::DownloadJob::dispatch(int w, WriteOp op, detail::AsUserBatch &batch, QString *error)
{
    const qint64 bytes = op.length;
    std::unique_lock<std::mutex> lock(writerMutex);
    // Over the high mark: wait until the writers are down to the low one (hysteresis: once the
    // budget is reached, the dispatcher does not stop and go for every operation).
    const bool over = writerQueued >= kWriterQueueHigh;
    while (over && writerQueued > kWriterQueueLow) {
        // Over the budget: wait for the writers, never with the user's rights, and say the fetch
        // is busy on the disk (not idle).
        lock.unlock();
        batch.release();
        beat();
        if (writersStopped(error))
            return false;
        lock.lock();
        QElapsedTimer waited;
        waited.start();
        writerRoom.wait_for(lock, std::chrono::milliseconds(kWriterWaitMs),
                            [this] { return writerQueued <= kWriterQueueLow || stopWriters || writerFailed.load(); });
        shared->writerRoomWaitNs.fetch_add(waited.nsecsElapsed(), std::memory_order_relaxed);
        writerRoomWaitNs.fetch_add(waited.nsecsElapsed(), std::memory_order_relaxed);
        if (stopWriters || writerFailed.load()) {
            lock.unlock();
            if (!writersStopped(error))
                *error = kCancelled;
            return false;
        }
    }
    writerQueues[w].push_back(std::move(op));
    writerBytes[w] += bytes;
    writerQueued += bytes;
    const qint64 peak = writerQueued;
    lock.unlock();
    writerWake.notify_all();
    qint64 seen = shared->peakWriterQueued.load();
    while (peak > seen && !shared->peakWriterQueued.compare_exchange_weak(seen, peak)) {
    }
    return true;
}

bool FileTransfer::DownloadJob::drainWriters(detail::AsUserBatch &batch, QString *error)
{
    if (writers.empty())
        return true;
    batch.release();
    {
        std::lock_guard<std::mutex> lock(writerMutex);
        inputClosed = true;
    }
    writerWake.notify_all();
    // Every queue empty and every writer idle: all written and closed. Waited for in slices,
    // beating (the writers are busy on the disk) and watching for a cancel or a failure.
    for (;;) {
        if (writersStopped(error))
            return false;
        std::unique_lock<std::mutex> lock(writerMutex);
        const bool idle = writerRoom.wait_for(lock, std::chrono::milliseconds(kWriterWaitMs), [this] {
            return (writerQueued == 0 && writersBusy.load() == 0) || writerFailed.load() || stopWriters;
        });
        lock.unlock();
        if (idle && !writerFailed.load() && !stopWriters)
            break;
        beat();
    }
    joinWriters(false);
    if (writersStopped(error))
        return false;
    if (filesDone.load() != manifest.fileCount) {
        *error = QStringLiteral("A received file could not be written");
        return false;
    }
    return true;
}

void FileTransfer::DownloadJob::joinWriters(bool drop)
{
    if (writers.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(writerMutex);
        inputClosed = true;
        if (drop) {
            stopWriters = true;
            dropQueuedLocked();
        }
    }
    writerWake.notify_all();
    for (std::thread &t : writers)
        t.join();
    writers.clear();
}

void FileTransfer::DownloadJob::dropQueuedLocked()
{
    // What the writers have not taken yet, and its bytes (a writer counts off what it is writing).
    for (size_t w = 0; w < writerQueues.size(); ++w) {
        for (const WriteOp &op : writerQueues[w]) {
            writerBytes[w] -= op.length;
            writerQueued -= op.length;
        }
        writerQueues[w].clear();
    }
}

void FileTransfer::DownloadJob::failWriters(const QString &error)
{
    {
        std::lock_guard<std::mutex> lock(writerMutex);
        if (writerError.isEmpty())
            writerError = error;
        writerFailed.store(true);
        stopWriters = true;
        dropQueuedLocked();
    }
    writerWake.notify_all();
    writerRoom.notify_all();
}

void FileTransfer::DownloadJob::writerLoop(int w)
{
    detail::AsUserBatch batch(impersonate, identity, anchor);
    fs::WriteFile file;
    QString name; // of the file open
    for (;;) {
        WriteOp op;
        {
            std::unique_lock<std::mutex> lock(writerMutex);
            const auto ready = [&] { return !writerQueues[w].empty() || stopWriters || inputClosed; };
            if (!ready())
                writerWake.wait_for(lock, std::chrono::milliseconds(kHeldWaitMs), ready);
            if (!ready()) {
                lock.unlock();
                batch.release(); // never the user's rights while waiting
                lock.lock();
                writerWake.wait(lock, ready);
            }
            if (stopWriters || writerQueues[w].empty())
                break;
            op = std::move(writerQueues[w].front());
            writerQueues[w].pop_front();
            writersBusy.fetch_add(1);
        }
        const qint64 bytes = op.length;
        const QString itemName = manifest.items.at(op.item).name;
        QElapsedTimer busy;
        busy.start();
        auto done = [&] {
            shared->writerBusyNs.fetch_add(busy.nsecsElapsed(), std::memory_order_relaxed);
            writerBusyNs.fetch_add(busy.nsecsElapsed(), std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(writerMutex);
                writerBytes[w] -= bytes;
                writerQueued -= bytes;
                writersBusy.fetch_sub(1);
            }
            writerRoom.notify_all();
        };
        QString error;
        if (isCancelled() || shutdown.load()) {
            done();
            break;
        }
        // Every file is created and written as the console user, never as LocalSystem, and as the
        // user the job started with.
        if (!asUser(batch, &error)) {
            failWriters(error);
            done();
            break;
        }
        if (!op.path.isEmpty()) {
            name = itemName;
            if (!file.open(op.path)) {
                failWriters(QStringLiteral("A received file could not be written: %1").arg(name));
                done();
                break;
            }
        }
        if (op.length > 0) {
            if (const int delay = shared->writeDelayMs.load(); delay > 0) {
                batch.release(); // a slow disk: not with the user's rights
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                if (!asUser(batch, &error)) {
                    failWriters(error);
                    done();
                    break;
                }
            }
            if (!file.write(op.frame.constData() + op.offset, op.length)) {
                failWriters(QStringLiteral("Disk full or write error: %1").arg(name));
                done();
                break;
            }
            diskPending.fetch_sub(op.length);
            progressed.fetch_add(op.length);
            updateStats(false);
        }
        if (op.close) {
            if (!file.close()) {
                failWriters(QStringLiteral("Disk full or write error: %1").arg(name));
                done();
                break;
            }
            filesDone.fetch_add(1);
            updateStats(false);
        }
        done();
    }
    file.close(); // the one left open by a stop, if any
}

void FileTransfer::DownloadJob::finish(detail::AsUserBatch &batch, bool ok, const QString &error)
{
    queue.close(); // nothing more is read
    // The writers stop (what they have not done is dropped) and close their files; a success
    // waited for them already.
    joinWriters(true);
    // Released before the end is reported (whoever takes the files may move them) and before any
    // cleanup (held folders cannot be deleted).
    tree.close();
    if (finishPauseBeforeMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(finishPauseBeforeMs));
    int expected = Running;
    const bool won = state.compare_exchange_strong(expected, Finishing);
    if (won && finishPauseAfterMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(finishPauseAfterMs));
    batch.release();
    // One line per fetch: where its time went (the preparation, the writers' work and the
    // dispatcher's waits for room in their queues), for a slow transfer reported by a user.
    if (manifest.complete) {
        const qint64 elapsed = std::max<qint64>(started.elapsed(), 1);
        qCInfo(lcTransfer).nospace() << "Transfer " << handle << (ok && won ? " received" : " ended") << ": "
                                     << manifest.fileCount << " files, " << manifest.dirs.size() << " folders, "
                                     << progressed.load() << " of " << manifest.total << " bytes in " << elapsed
                                     << " ms (prepared in " << prepareMs << " ms); " << writerTotal
                                     << " writer(s) busy " << writerBusyNs.load() / 1000000 << " ms in all, "
                                     << "waited for room " << writerRoomWaitNs.load() / 1000000 << " ms";
    }
    bool removeWritten = !won || !ok; // !won: cancel() reported it already
    if (won) {
        // Reported before any cleanup, which may take minutes for a large tree: the user and
        // whoever waits for the files hear of a failure at once. The job stays live (and its
        // folder protected, isActiveTarget) until the cleanup is over.
        FetchResult r;
        if (ok) {
            r = std::move(result);
            r.ok = true;
        } else {
            r.error = error;
        }
        shared->post([job = shared_from_this(), r = std::move(r)](FileTransfer *ft) mutable {
            ft->onJobFinished(job, std::move(r));
        });
        // A success cancelled on this object's thread before it got there is nobody's now.
        if (ok)
            removeWritten = awaitVerdict() == Discarded;
    }
    // A failed or cancelled transfer leaves no half-written files behind: removed as the console
    // user who received them (fs::removeTree never follows links), but not as somebody else after
    // a change of user (seen by any of the job's threads), nor while the service shuts down.
    if (removeWritten && created.load() && !shutdown.load() && !anchor->changed.load()) {
        if (batch.acquire() == detail::AsUserBatch::Status::Ok)
            fs::removeTree(targetDir, [this] { return shutdown.load(); });
        batch.release();
    }
}

struct FileTransfer::Download {
    quint64 handle = 0;
    SecureStream *stream = nullptr;
    QByteArray peer;
    QString kind; // non-empty: the user sees this fetch (progress, cancel)
    quint64 offerId = 0;
    bool includeFiles = false;
    Callback done;
    std::shared_ptr<DownloadJob> job;
    bool established = false; // the stream is
    bool ready = false;       // the worker is
    bool getSent = false;
    bool paused = false; // the stream, for the worker's queue
    QElapsedTimer sinceProgress; // rate limit of transferProgress
    QTimer idle;
};

quint64 FileTransfer::fetch(const QByteArray &peerId, quint64 offerId, const QString &targetDir, bool includeFiles,
                            qint64 maxBytes, Callback done, const QString &kind)
{
    return fetch(peerId, offerId, targetDir, includeFiles, maxBytes, std::move(done), FetchOptions{kind, {}, {}});
}

quint64 FileTransfer::fetch(const QByteArray &peerId, quint64 offerId, const QString &targetDir, bool includeFiles,
                            qint64 maxBytes, Callback done, FetchOptions options)
{
    const quint64 handle = m_nextHandle++;
    auto job = std::make_shared<DownloadJob>();
    job->handle = handle;
    job->targetDir = targetDir;
    job->maxBytes = maxBytes;
    job->manifest.maxBytes = maxBytes;
    job->visible = !options.kind.isEmpty();
    job->impersonate = m_impersonate;
    job->identity = m_identity;
    job->prepareAsUser = std::move(options.prepareAsUser);
    job->cleanupAsUser = std::move(options.cleanupAsUser);
    job->freeSpace = m_freeSpace;
    job->beatMs = std::clamp(m_idleTimeoutMs / 4, 1, kBeatMs);
    job->finishPauseBeforeMs = m_finishPauseBeforeMs;
    job->finishPauseAfterMs = m_finishPauseAfterMs;
    job->writersWanted = m_writerThreads > 0 ? m_writerThreads : g_defaultWriters.load();
    job->shared = m_shared;
    job->manifest.peerName = peerName(peerId);

    auto *dl = new Download;
    dl->handle = handle;
    dl->peer = peerId;
    dl->kind = options.kind;
    dl->offerId = offerId;
    dl->includeFiles = includeFiles;
    dl->done = std::move(done);
    dl->job = job;
    m_downloads[handle] = dl;

    // Started with the get: before it, the worker may take its time (making room).
    dl->idle.setSingleShot(true);
    dl->idle.setInterval(m_idleTimeoutMs);
    connect(&dl->idle, &QTimer::timeout, this, [job] {
        job->queue.pushAlways(DownloadJob::Msg{DownloadJob::Msg::Abort, {}, QStringLiteral("Transfer timed out")}, 0);
    });
    reportProgress(dl, true); // shown at once (connecting), before the manifest says how much

    // A peer of another protocol: refused at once, with no stream and no worker (later, never
    // inside fetch).
    if (const QString reason = incompatibleReason(peerId); !reason.isEmpty()) {
        QMetaObject::invokeMethod(
            this, [this, handle, reason] { endDownload(handle, FetchResult{false, reason, {}, {}}, QStringLiteral("failed")); },
            Qt::QueuedConnection);
        return handle;
    }
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->live[handle] = job;
    }
    std::thread([job] {
        job->run();
        const std::shared_ptr<Shared> shared = job->shared;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->live.erase(job->handle);
        }
        shared->exited.notify_all();
    }).detach();
    return handle;
}

void FileTransfer::maybeSendGet(Download *d)
{
    if (!d->established || !d->ready || d->getSent || !d->stream)
        return;
    QCborMap get = typed(QStringLiteral("get"));
    get.insert(QStringLiteral("o"), qint64(d->offerId));
    get.insert(QStringLiteral("files"), d->includeFiles);
    get.insert(QStringLiteral("v"), 3);          // what 0.1.22-0.1.30 read (folders, split manifests)
    get.insert(QStringLiteral("pv"), kProtocol); // what a publisher of protocol 3 on checks
    d->stream->send(control(get));
    d->getSent = true;
    d->idle.start();
}

void FileTransfer::onReceived(quint64 handle, const QByteArray &frame)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    if (d->getSent)
        d->idle.start();
    DownloadJob &job = *d->job;
    const DownloadJob::Msg msg{DownloadJob::Msg::Frame, frame, {}};
    // Refused only past the high-water mark, which the pause below keeps it from passing by more
    // than this frame (the one being delivered when the stream was paused).
    if (!job.queue.tryPush(msg, frame.size()))
        job.queue.pushAlways(msg, frame.size());
    m_peakQueued = std::max(m_peakQueued, job.queue.bytes());
    if (!d->paused && job.queue.isFull() && d->stream) {
        d->paused = true;
        job.readPaused.store(true);
        ++m_readPauses;
        d->stream->setReadPaused(true);
        // The worker may have drained the queue before it could see the pause: then it posts no
        // resume, so the check is made here once more.
        if (job.queue.isLow()) {
            d->paused = false;
            job.readPaused.store(false);
            d->stream->setReadPaused(false);
        }
    }
}

void FileTransfer::onJobReady(quint64 handle)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    d->ready = true;
    // Opened only now: the publisher waits for the get under its handshake timeout
    // (SecureStream::kHandshakeTimeoutMs, from the first byte), which the worker's preparation
    // (deleting a large older copy) could outlast.
    const std::shared_ptr<DownloadJob> job = d->job;
    SecureStream *stream = m_node->openStream(d->peer);
    if (!stream) {
        job->queue.pushAlways(DownloadJob::Msg{DownloadJob::Msg::Abort, {}, QStringLiteral("Peer not reachable")}, 0);
        return;
    }
    d->stream = stream;
    connect(stream, &SecureStream::established, this, [this, handle] {
        auto found = m_downloads.find(handle);
        if (found == m_downloads.end())
            return;
        found->second->established = true;
        maybeSendGet(found->second);
    });
    // The worker ends the fetch once it has handled every frame before the end of the stream.
    connect(stream, &SecureStream::closed, this, [job](const QString &reason) {
        job->queue.pushAlways(DownloadJob::Msg{DownloadJob::Msg::Closed, {}, reason}, 0);
    });
    connect(stream, &SecureStream::received, this,
            [this, handle](const QByteArray &frame) { onReceived(handle, frame); });
    d->established = stream->isEstablished();
    maybeSendGet(d);
}

void FileTransfer::onJobBeat(quint64 handle)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    d->job->beatPending.store(false);
    if (d->getSent)
        d->idle.start(); // busy on the disk is not idle
}

void FileTransfer::onJobResume(quint64 handle)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    d->job->queue.clearNotifyPending();
    if (!d->paused)
        return;
    d->paused = false;
    d->job->readPaused.store(false);
    if (d->stream)
        d->stream->setReadPaused(false);
}

void FileTransfer::onJobProgress(quint64 handle)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    d->job->progressPending.store(false);
    reportProgress(d, d->job->progressForced.exchange(false));
}

void FileTransfer::reportProgress(Download *d, bool force)
{
    if (d->kind.isEmpty())
        return;
    if (!force && d->sinceProgress.isValid() && d->sinceProgress.elapsed() < kProgressIntervalMs)
        return;
    d->sinceProgress.start();
    const DownloadJob::Stats s = d->job->statsNow();
    emit transferProgress(d->handle, d->peer, d->kind, s.received, s.total, s.filesDone, s.fileCount,
                          d->job->targetDir);
}

void FileTransfer::onJobFinished(const std::shared_ptr<DownloadJob> &job, FetchResult result)
{
    auto found = m_downloads.find(job->handle);
    if (found == m_downloads.end() || found->second->job != job) {
        // Cancelled while its worker was settling a success: what it wrote is nobody's now, and
        // the worker removes it (as the user it recorded, still counted as at work).
        job->setVerdict(DownloadJob::Discarded);
        return;
    }
    job->setVerdict(DownloadJob::Delivered);
    const QString state = result.ok ? QStringLiteral("done") : QStringLiteral("failed");
    endDownload(job->handle, result, state);
}

void FileTransfer::cancel(quint64 handle)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    DownloadJob &job = *found->second->job;
    int expected = DownloadJob::Running;
    job.state.compare_exchange_strong(expected, DownloadJob::Cancelled); // or its end is on its way
    job.queue.close();
    FetchResult result{false, kCancelled, {}, {}};
    result.cancelled = true;
    endDownload(handle, result, QStringLiteral("cancelled"));
}

// --- Listing -----------------------------------------------------------------------------------
//
// A list fetch is a get like any other (files, v 2: every 0.1.22+ publisher serves it, and an
// older one ignores v and sends its single manifest frame) whose stream is dropped once the
// manifest is in. Nothing is written, so it needs no worker: the manifest is read on this thread,
// with the very checks a fetch makes (detail::Manifest). The publisher, which reports nothing
// about uploads, sees its receiver go and stops; it may have read up to kHighWater (plus what the
// sockets hold) of data meanwhile.

struct FileTransfer::ListFetch {
    quint64 handle = 0;
    QPointer<SecureStream> stream;
    quint64 offerId = 0;
    detail::Manifest manifest;
    ListCallback done;
    bool getSent = false;
    QTimer idle;
};

quint64 FileTransfer::fetchList(const QByteArray &peerId, quint64 offerId, qint64 maxBytes, ListCallback done)
{
    const quint64 handle = m_nextHandle++;
    auto *l = new ListFetch;
    l->handle = handle;
    l->offerId = offerId;
    l->manifest.maxBytes = maxBytes;
    l->manifest.peerName = peerName(peerId);
    l->done = std::move(done);
    m_lists[handle] = l;
    l->idle.setSingleShot(true);
    l->idle.setInterval(m_idleTimeoutMs);
    connect(&l->idle, &QTimer::timeout, this,
            [this, handle] { endList(handle, false, false, QStringLiteral("Transfer timed out"), {}); });
    l->idle.start();

    if (const QString reason = incompatibleReason(peerId); !reason.isEmpty()) {
        // As below: the same manifest would be refused, so it counts as refused.
        QMetaObject::invokeMethod(
            this, [this, handle, reason] { endList(handle, false, true, reason, {}); }, Qt::QueuedConnection);
        return handle;
    }
    SecureStream *stream = m_node->openStream(peerId);
    if (!stream) {
        // Never inside fetchList: the caller may not be ready for its callback yet.
        QMetaObject::invokeMethod(
            this, [this, handle] { endList(handle, false, false, QStringLiteral("Peer not reachable"), {}); },
            Qt::QueuedConnection);
        return handle;
    }
    l->stream = stream;
    auto sendGet = [this, handle] {
        auto found = m_lists.find(handle);
        if (found == m_lists.end() || found->second->getSent || !found->second->stream)
            return;
        ListFetch *l = found->second;
        QCborMap get = typed(QStringLiteral("get"));
        get.insert(QStringLiteral("o"), qint64(l->offerId));
        get.insert(QStringLiteral("files"), true);
        get.insert(QStringLiteral("v"), 3);
        get.insert(QStringLiteral("pv"), kProtocol);
        l->stream->send(control(get));
        l->getSent = true;
        l->idle.start();
    };
    connect(stream, &SecureStream::established, this, sendGet);
    connect(stream, &SecureStream::closed, this, [this, handle](const QString &reason) {
        endList(handle, false, false, reason.isEmpty() ? QStringLiteral("Connection closed") : reason, {});
    });
    connect(stream, &SecureStream::received, this,
            [this, handle](const QByteArray &frame) { onListFrame(handle, frame); });
    if (stream->isEstablished())
        sendGet();
    return handle;
}

void FileTransfer::onListFrame(quint64 handle, const QByteArray &frame)
{
    auto found = m_lists.find(handle);
    if (found == m_lists.end() || frame.isEmpty())
        return;
    ListFetch *l = found->second;
    l->idle.start();
    if (quint8(frame[0]) == kData || quint8(frame[0]) == kOldData) {
        // The manifest comes first and whole: data before its last frame is a broken sender.
        endList(handle, false, true, QStringLiteral("Data before manifest"), {});
        return;
    }
    if (quint8(frame[0]) != kControl)
        return;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(QStringLiteral("t")).toString();
    if (t == QLatin1String("manifest")) {
        QString error;
        if (!l->manifest.add(msg, &error)) {
            endList(handle, false, true, error, {}); // the same manifest would be refused again
            return;
        }
        if (l->manifest.complete)
            endList(handle, true, false, {}, l->manifest.listing());
        return;
    }
    if (t == QLatin1String("end")) {
        endList(handle, false, false, QStringLiteral("Transfer ended early"), {});
        return;
    }
    if (t == QLatin1String("error"))
        endList(handle, false, false, msg.value(QStringLiteral("r")).toString(), {});
}

void FileTransfer::endList(quint64 handle, bool ok, bool refused, const QString &error,
                           const QList<ListedFile> &items)
{
    auto found = m_lists.find(handle);
    if (found == m_lists.end())
        return;
    ListFetch *l = found->second;
    m_lists.erase(found);
    l->idle.stop();
    if (l->stream) {
        // Dropped rather than closed: the publisher's data that follows the manifest is not wanted.
        l->stream->disconnect(this);
        l->stream->abort(QStringLiteral("Listed"));
        l->stream->deleteLater();
    }
    if (ok)
        qCInfo(lcTransfer) << "Listed" << items.size() << "files and folders";
    else
        qCInfo(lcTransfer) << "Listing failed:" << error;
    ListCallback done = std::move(l->done);
    delete l;
    if (done)
        done(ok, refused, error, items);
}

void FileTransfer::cancelList(quint64 handle)
{
    auto found = m_lists.find(handle);
    if (found == m_lists.end())
        return;
    found->second->done = nullptr;
    endList(handle, false, false, kCancelled, {});
}

QString FileTransfer::peerName(const QByteArray &peerId) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == peerId)
            return p.name.isEmpty() ? QStringLiteral("The other device") : p.name;
    }
    return QStringLiteral("The other device");
}

QString FileTransfer::incompatibleReason(const QByteArray &peerId) const
{
    if (m_node->transfersCompatible(peerId))
        return {};
    return mismatchText(peerName(peerId), m_node->peerProtocol(peerId));
}

bool FileTransfer::isVisibleFetch(quint64 handle) const
{
    auto found = m_downloads.find(handle);
    return found != m_downloads.end() && !found->second->kind.isEmpty();
}

void FileTransfer::setWriteDelayForTests(int ms) { m_shared->writeDelayMs.store(ms); }

qint64 FileTransfer::peakWriterQueuedForTests() const { return m_shared->peakWriterQueued.load(); }

int FileTransfer::lastWriterCountForTests() const { return m_shared->lastWriters.load(); }

FileTransfer::Counters FileTransfer::countersForTests() const
{
    return Counters{m_shared->dataFramesSent.load(), m_shared->dataFramesReceived.load(),
                    m_shared->writerBusyNs.load(),   m_shared->writerRoomWaitNs.load(),
                    m_shared->readAheadFiles.load(), m_shared->readAheadWaitNs.load()};
}

void FileTransfer::setDefaultWriterThreadsForTests(int writers) { g_defaultWriters.store(writers); }

bool FileTransfer::isActiveTarget(const QString &dir) const { return m_shared->isActive(dir); }

std::function<bool(const QString &)> FileTransfer::activeTargetCheck() const
{
    return [shared = m_shared](const QString &dir) { return shared->isActive(dir); };
}

void FileTransfer::runAsUser(std::function<void()> work, std::function<void()> done)
{
    if (!m_pool)
        m_pool = std::make_unique<detail::TaskPool>(2);
    m_pool->post([work = std::move(work), done = std::move(done), shared = m_shared, impersonate = m_impersonate] {
        {
            const std::shared_ptr<void> asUser =
                impersonate ? impersonate() : std::shared_ptr<void>(std::make_shared<int>(0));
            if (asUser)
                work();
        }
        if (done)
            shared->post([done](FileTransfer *) { done(); });
    });
}

void FileTransfer::startCleanup(const std::shared_ptr<DownloadJob> &job)
{
    if (!m_pool)
        m_pool = std::make_unique<detail::TaskPool>(2);
    // As the console user the fetch recorded (its identity anchor): never as somebody else who
    // signed in meanwhile, and not at all when nobody is signed in. Pending until it has run or
    // been skipped; the pool drops it when this object goes (the fetch is shut down then).
    m_pool->post([job] {
        {
            detail::AsUserBatch batch(job->impersonate, job->identity, job->anchor);
            if (batch.acquire() == detail::AsUserBatch::Status::Ok) {
                QElapsedTimer took;
                took.start();
                job->cleanupAsUser();
                qCInfo(lcTransfer) << "Transfer" << job->handle << "cleaned up after its preparation in"
                                   << took.elapsed() << "ms";
            } else {
                qCInfo(lcTransfer) << "Transfer" << job->handle << "skipped its cleanup: nobody is signed in, or somebody else";
            }
        }
        job->endCleanup();
    });
}

void FileTransfer::endDownload(quint64 handle, const FetchResult &result, const QString &state)
{
    auto found = m_downloads.find(handle);
    if (found == m_downloads.end())
        return;
    Download *d = found->second;
    m_downloads.erase(found);
    d->idle.stop();
    if (d->stream) {
        d->stream->disconnect(this);
        d->stream->close();
        d->stream->deleteLater();
    }
    if (!result.ok)
        qCInfo(lcTransfer) << "Transfer failed:" << result.error;
    if (!d->kind.isEmpty()) {
        if (result.ok) {
            const DownloadJob::Stats s = d->job->statsNow();
            emit transferProgress(handle, d->peer, d->kind, s.received, s.total, s.filesDone, s.fileCount,
                                  d->job->targetDir);
        }
        emit transferFinished(handle, d->peer, d->kind, state, result.ok ? QString() : result.error,
                              d->job->targetDir);
    }
    Callback done = std::move(d->done);
    delete d;
    if (done)
        done(result);
}

FileTransfer::~FileTransfer()
{
    // No more posts from the workers; every one of them stops, and removes nothing.
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->owner = nullptr;
        for (auto &[handle, job] : m_shared->live) {
            job->shutdown.store(true);
            int expected = DownloadJob::Running;
            job->state.compare_exchange_strong(expected, DownloadJob::Cancelled);
            job->queue.close();
            job->setVerdict(DownloadJob::Discarded); // a success waiting to be delivered: never now
        }
    }
    for (auto &[handle, d] : m_downloads) {
        if (d->stream) {
            d->stream->disconnect(this);
            d->stream->deleteLater();
        }
        delete d;
    }
    m_downloads.clear();
    for (auto &[handle, l] : m_lists) {
        if (l->stream) { // dropped, as endList drops it; the callback never runs
            l->stream->disconnect(this);
            l->stream->abort(QStringLiteral("Listed"));
            l->stream->deleteLater();
        }
        delete l;
    }
    m_lists.clear();
    for (auto &[handle, up] : m_uploads) {
        up->job->cancel();
        delete up;
    }
    m_uploads.clear();
    // Bounded: a worker still busy after that (a slow disk, a long prepareAsUser) ends alone, and
    // so does a reader stuck in a slow read.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kShutdownWaitMs);
    {
        std::unique_lock<std::mutex> lock(m_shared->mutex);
        m_shared->exited.wait_until(lock, deadline,
                                    [this] { return m_shared->live.empty() && m_shared->uploads.empty(); });
    }
    if (m_pool)
        m_pool->shutdown(deadline);
}

} // namespace df
