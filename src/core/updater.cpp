#include "updater.h"

#include "buildinfo.h"
#include "clock.h"
#include "node.h"
#include "securestream.h"

#include <QCborMap>
#include <QCborValue>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>

#include <iterator>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcUpdate, "deskflower.update")

namespace df {

namespace {
constexpr quint8 kControl = 1;
constexpr quint8 kData = 2;
const QString kT = QStringLiteral("t");

QByteArray control(const QCborMap &msg) { return char(kControl) + QCborValue(msg).toCbor(); }

QByteArray errorFrame(const QString &reason)
{
    QCborMap m;
    m.insert(kT, QStringLiteral("error"));
    m.insert(QStringLiteral("r"), reason);
    return control(m);
}

QString packageDir(const QString &storeDir) { return QDir(storeDir).filePath(QStringLiteral("package")); }
QString incomingDir(const QString &storeDir) { return QDir(storeDir).filePath(QStringLiteral("incoming")); }

QByteArray readSmallFile(const QString &path, qint64 maxBytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > maxBytes)
        return {};
    return f.readAll();
}

// Symbolic links, junctions and other reparse points could redirect what the service (LocalSystem on
// Windows) deletes or installs: none is followed inside the store.
bool isLink(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(native.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}

// Deletes a directory tree; a link is removed itself, never what it points to.
void removeTree(const QString &path)
{
    if (!isLink(path)) {
        QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QString entry = it.next();
            if (!isLink(entry) && it.fileInfo().isDir())
                removeTree(entry);
            else if (!QFile::remove(entry))
                QDir().rmdir(entry); // a directory link
        }
    }
    if (!QDir().rmdir(path))
        QFile::remove(path);
}

// Recreates <store>/<dir> empty.
bool resetStoreDir(const QString &storeDir, const QString &dir, QString *error)
{
    if (isLink(storeDir)) {
        *error = QStringLiteral("%1 is a link").arg(QDir::toNativeSeparators(storeDir));
        return false;
    }
    removeTree(dir);
    if (!QDir().mkpath(dir) || isLink(storeDir) || isLink(dir)) {
        *error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(dir));
        return false;
    }
    return true;
}

#ifdef Q_OS_WIN
QString finalPath(HANDLE handle)
{
    wchar_t buffer[4 * MAX_PATH];
    const DWORD n = GetFinalPathNameByHandleW(handle, buffer, DWORD(std::size(buffer)), FILE_NAME_NORMALIZED);
    if (n == 0 || n >= std::size(buffer))
        return {};
    return QDir::fromNativeSeparators(QString::fromWCharArray(buffer, int(n)));
}
#endif

// Whether the locked package really is <store>/incoming/<name>, wherever the path it was opened by
// leads. While it stays open, neither it nor the directories above it can be renamed or replaced.
bool isInIncoming(QFile *locked, const QString &storeDir, const QString &name, QString *error)
{
#ifdef Q_OS_WIN
    const std::wstring store = QDir::toNativeSeparators(storeDir).toStdWString();
    HANDLE dir = CreateFileW(store.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        *error = QStringLiteral("Cannot open %1 (error %2)").arg(storeDir).arg(GetLastError());
        return false;
    }
    const QString expected = finalPath(dir) + QStringLiteral("/incoming/") + name;
    CloseHandle(dir);
    const QString actual = finalPath(HANDLE(_get_osfhandle(locked->handle())));
    if (actual.isEmpty() || actual.compare(expected, Qt::CaseInsensitive) != 0) {
        *error = QStringLiteral("The downloaded package is not where it was written (%1)").arg(actual);
        return false;
    }
#else
    Q_UNUSED(locked);
    Q_UNUSED(storeDir);
    Q_UNUSED(name);
    Q_UNUSED(error); // the store is private to the service's account
#endif
    return true;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

// Opens a file read-only so that, on Windows, nobody can write, rename or delete it while it is open:
// what was hashed through this handle is what the installer reads.
std::unique_ptr<QFile> openLocked(const QString &path, QString *error)
{
    auto file = std::make_unique<QFile>(path);
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = QStringLiteral("Cannot lock %1 (error %2)").arg(path).arg(GetLastError());
        return nullptr;
    }
    const int fd = _open_osfhandle(intptr_t(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        *error = QStringLiteral("Cannot lock %1").arg(path);
        return nullptr;
    }
    if (!file->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        _close(fd);
        *error = QStringLiteral("Cannot read %1").arg(path);
        return nullptr;
    }
#else
    // The store directory is private to the service's account; there is no mandatory locking here.
    if (!file->open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1").arg(path);
        return nullptr;
    }
#endif
    return file;
}

// Reads <package>.manifest and its signature and checks the signature.
std::optional<update::Manifest> openManifestFor(const QByteArray &publicKey, const QString &packagePath,
                                                QByteArray *json, QByteArray *signature, QString *error)
{
    *json = readSmallFile(packagePath + update::kManifestSuffix, update::kMaxManifestSize);
    *signature = update::decodeSignature(readSmallFile(packagePath + update::kSignatureSuffix, 1024));
    if (json->isEmpty() || signature->isEmpty()) {
        *error = QStringLiteral("No signed manifest next to %1").arg(QDir::toNativeSeparators(packagePath));
        return std::nullopt;
    }
    return update::open(publicKey, *json, *signature, error);
}
} // namespace

Updater::Options Updater::Options::defaults(const QString &storeDir)
{
    Options o;
    o.storeDir = storeDir;
    o.publicKey = build::updatePublicKey();
    o.version = build::version();
    o.platform = build::platform();
    return o;
}

struct Updater::Upload {
    QPointer<SecureStream> stream;
    QFile file;
    qint64 size = 0;
    qint64 offset = 0;
    bool finished = false;
};

Updater::Updater(Node *node, PackageInstaller *installer, Options options, QObject *parent)
    : QObject(parent), m_node(node), m_installer(installer), m_options(std::move(options))
{
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleTimeoutMs);
    connect(&m_idle, &QTimer::timeout, this, [this] { fail(QStringLiteral("Transfer timed out")); });
    connect(m_node, &Node::streamAccepted, this, &Updater::onStreamAccepted);
    loadServedPackage();
}

Updater::~Updater()
{
    if (m_stream) {
        m_stream->disconnect(this);
        m_stream->deleteLater();
    }
    for (auto &[handle, up] : m_uploads)
        delete up;
}

QString Updater::offeredUpdate(const PeerView &peer) const
{
    if (!peer.trusted || !peer.connected || peer.updateVersion.isEmpty() || peer.updatePlatform != m_options.platform)
        return {};
    const auto cmp = update::compareVersions(peer.updateVersion, m_options.version);
    return cmp && *cmp > 0 ? peer.updateVersion : QString();
}

// --- Store -------------------------------------------------------------------------------------

bool Updater::importPackage(const Options &options, const QString &packagePath, QString *error)
{
    if (options.publicKey.size() != update::kPublicKeySize) {
        *error = QStringLiteral("Updates are not configured in this build");
        return false;
    }
    const QFileInfo info(packagePath);
    QByteArray json, signature;
    const auto manifest = openManifestFor(options.publicKey, info.absoluteFilePath(), &json, &signature, error);
    if (!manifest)
        return false;
    if (manifest->file != info.fileName() || manifest->platform != options.platform
        || update::compareVersions(manifest->version, options.version) != 0) {
        *error = QStringLiteral("The manifest describes %1 %2 for %3, not this installation (%4 %5)")
                     .arg(manifest->file, manifest->version, manifest->platform, options.version, options.platform);
        return false;
    }
    std::unique_ptr<QFile> source = openLocked(info.absoluteFilePath(), error);
    if (!source || !update::matchesContent(*manifest, source.get(), error))
        return false;

    const QString dir = packageDir(options.storeDir);
    const QString target = QDir(dir).filePath(manifest->file);
    if (QFileInfo(target).canonicalFilePath() == info.canonicalFilePath())
        return true; // already the stored package
    if (!resetStoreDir(options.storeDir, dir, error))
        return false;
    // Copied from the locked handle: the stored bytes are the verified ones.
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly) || !source->seek(0)) {
        *error = QStringLiteral("Cannot write %1").arg(target);
        return false;
    }
    while (!source->atEnd()) {
        const QByteArray chunk = source->read(256 * 1024);
        if (chunk.isEmpty() || out.write(chunk) != chunk.size()) {
            *error = QStringLiteral("Cannot write %1").arg(target);
            out.remove();
            return false;
        }
    }
    out.close();
    if (!writeFile(target + update::kManifestSuffix, json)
        || !writeFile(target + update::kSignatureSuffix, update::encodeSignature(signature))) {
        *error = QStringLiteral("Cannot write the manifest into %1").arg(dir);
        return false;
    }
    return true;
}

void Updater::loadServedPackage()
{
    m_served.reset();
    if (!isConfigured())
        return;
    const QDir dir(packageDir(m_options.storeDir));
    for (const QString &name : dir.entryList({QLatin1Char('*') + update::kManifestSuffix}, QDir::Files)) {
        const QString path = dir.filePath(name.chopped(update::kManifestSuffix.size()));
        QByteArray json, signature;
        QString error;
        auto manifest = openManifestFor(m_options.publicKey, path, &json, &signature, &error);
        if (manifest && (manifest->platform != m_options.platform || dir.filePath(manifest->file) != path)) {
            error = QStringLiteral("Not a package for this device");
            manifest.reset();
        }
        QFile file(path);
        if (manifest && !file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot read the package");
            manifest.reset();
        }
        if (manifest && !update::matchesContent(*manifest, &file, &error))
            manifest.reset();
        if (!manifest) {
            qCWarning(lcUpdate).noquote() << "Ignoring stored update package" << name << ":" << error;
            continue;
        }
        if (m_served && update::compareVersions(manifest->version, m_served->version) <= 0)
            continue;
        m_served = manifest;
        m_servedJson = json;
        m_servedSignature = signature;
        m_servedPath = path;
    }
    if (m_served)
        qCInfo(lcUpdate) << "Serving update package" << m_served->file << m_served->version;
    m_node->setUpdateOffer(m_served ? m_served->version : QString(), m_served ? m_served->platform : QString());
}

// --- Serving -----------------------------------------------------------------------------------

void Updater::onStreamAccepted(SecureStream *stream)
{
    // Only the first frame says whether this stream asks for the update package.
    auto first = std::make_shared<QMetaObject::Connection>();
    *first = connect(stream, &SecureStream::received, this, [this, stream, first](const QByteArray &frame) {
        disconnect(*first);
        if (frame.isEmpty() || quint8(frame[0]) != kControl)
            return;
        if (QCborValue::fromCbor(frame.mid(1)).toMap().value(kT).toString() == QLatin1String("update"))
            serve(stream);
    });
}

void Updater::serve(SecureStream *stream)
{
    const QByteArray peer = stream->peerId();
    auto refuse = [stream](const QString &reason) {
        qCInfo(lcUpdate).noquote() << "Update request refused:" << reason;
        stream->send(errorFrame(reason));
        stream->close();
    };
    // The stream may predate an unpair (accepted streams outlive it).
    if (!m_node->isTrusted(peer))
        return refuse(QStringLiteral("Not paired"));
    if (!m_served)
        return refuse(QStringLiteral("This device has no update package"));
    const qint64 now = nowMs();
    auto last = m_lastRequest.find(peer);
    if (last != m_lastRequest.end() && now - last->second < m_options.minRequestIntervalMs)
        return refuse(QStringLiteral("Too many update requests, try again in a moment"));
    if (!m_uploads.empty())
        return refuse(QStringLiteral("Busy sending the update to another device"));
    m_lastRequest[peer] = now;

    auto *up = new Upload;
    up->stream = stream;
    up->size = m_served->size;
    up->file.setFileName(m_servedPath);
    if (!up->file.open(QIODevice::ReadOnly)) {
        delete up;
        return refuse(QStringLiteral("Cannot read the update package"));
    }
    QCborMap package;
    package.insert(kT, QStringLiteral("package"));
    package.insert(QStringLiteral("m"), m_servedJson);
    package.insert(QStringLiteral("s"), m_servedSignature);
    stream->send(control(package));
    qCInfo(lcUpdate) << "Sending update package" << m_served->version << "to" << peer.toHex();

    const quint64 handle = m_nextUpload++;
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
}

void Updater::pump(quint64 handle)
{
    auto found = m_uploads.find(handle);
    if (found == m_uploads.end())
        return;
    Upload *up = found->second;
    SecureStream *stream = up->stream;
    if (!stream || up->finished)
        return;
    while (stream->pendingBytes() < kHighWater) {
        if (up->offset >= up->size) {
            up->finished = true;
            QCborMap end;
            end.insert(kT, QStringLiteral("end"));
            stream->send(control(end));
            stream->close();
            return;
        }
        const QByteArray chunk = up->file.read(qMin<qint64>(kChunk, up->size - up->offset));
        if (chunk.isEmpty()) {
            up->finished = true;
            stream->send(errorFrame(QStringLiteral("The update package changed while sending")));
            stream->close();
            return;
        }
        stream->send(char(kData) + chunk);
        up->offset += chunk.size();
    }
}

// --- Receiving ---------------------------------------------------------------------------------

bool Updater::update(const QByteArray &peerId, QString *error)
{
    QString why;
    if (!isConfigured())
        why = QStringLiteral("Updates are not configured in this build");
    else if (!isSupported())
        why = QStringLiteral("Installing updates is not supported on this platform yet");
    else if (m_state != State::Idle)
        why = QStringLiteral("An update is already in progress");
    else if (!m_node->isTrusted(peerId))
        why = QStringLiteral("That device is not paired");
    else if (!m_node->isConnected(peerId))
        why = QStringLiteral("That device is not connected");
    if (!why.isEmpty()) {
        *error = why;
        return false;
    }

    if (!resetStoreDir(m_options.storeDir, incomingDir(m_options.storeDir), error))
        return false;
    SecureStream *stream = m_node->openStream(peerId);
    if (!stream) {
        *error = QStringLiteral("That device is not reachable");
        return false;
    }
    m_peer = peerId;
    m_stream = stream;
    m_received = 0;
    m_manifest.reset();
    connect(stream, &SecureStream::established, this, [stream] {
        QCborMap request;
        request.insert(kT, QStringLiteral("update"));
        stream->send(control(request));
    });
    connect(stream, &SecureStream::received, this, &Updater::onFrame);
    connect(stream, &SecureStream::closed, this, [this](const QString &reason) {
        fail(reason.isEmpty() ? QStringLiteral("Connection closed") : reason);
    });
    m_idle.start();
    setState(State::Downloading);
    return true;
}

void Updater::onFrame(const QByteArray &frame)
{
    if (m_state != State::Downloading || frame.isEmpty())
        return;
    m_idle.start();
    if (quint8(frame[0]) == kData) {
        const qint64 n = frame.size() - 1;
        if (!m_manifest)
            return fail(QStringLiteral("Data before the manifest"));
        if (m_received + n > m_manifest->size)
            return fail(QStringLiteral("More data than the manifest announced"));
        if (m_file.write(frame.constData() + 1, n) != n)
            return fail(QStringLiteral("Disk full or write error"));
        m_received += n;
        emit stateChanged();
        return;
    }
    if (quint8(frame[0]) != kControl)
        return;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(kT).toString();
    if (t == QLatin1String("package") && !m_manifest) {
        onPackage(msg);
    } else if (t == QLatin1String("end")) {
        if (!m_manifest || m_received != m_manifest->size)
            return fail(QStringLiteral("The transfer ended early"));
        onComplete();
    } else if (t == QLatin1String("error")) {
        fail(msg.value(QStringLiteral("r")).toString().left(200));
    }
}

void Updater::onPackage(const QCborMap &msg)
{
    const QByteArray json = msg.value(QStringLiteral("m")).toByteArray();
    const QByteArray signature = msg.value(QStringLiteral("s")).toByteArray();
    QString error;
    // Everything is checked before accepting a single byte of the package.
    const auto manifest = update::open(m_options.publicKey, json, signature, &error);
    if (!manifest)
        return fail(error);
    if (!update::isApplicable(*manifest, m_options.version, m_options.platform, &error))
        return fail(error);
    if (manifest->size > m_options.maxPackageBytes)
        return fail(QStringLiteral("The package is larger than allowed (%1 MB)")
                        .arg(m_options.maxPackageBytes / (1024 * 1024)));
    m_path = QDir(incomingDir(m_options.storeDir)).filePath(manifest->file);
    // The manifest goes next to the package: the installer imports both into the store.
    m_file.setFileName(m_path);
    if (!writeFile(m_path + update::kManifestSuffix, json)
        || !writeFile(m_path + update::kSignatureSuffix, update::encodeSignature(signature))
        || !m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Cannot write %1").arg(m_path));
    m_manifest = manifest;
    qCInfo(lcUpdate) << "Downloading update" << manifest->version << manifest->size << "bytes from" << m_peer.toHex();
    emit stateChanged();
}

void Updater::onComplete()
{
    m_idle.stop();
    m_stream->disconnect(this);
    m_stream->close();
    m_stream->deleteLater();
    m_stream = nullptr;
    m_file.close();
    setState(State::Verifying);

    // Verified again, on the exact bytes the installer will read.
    QString error;
    std::unique_ptr<QFile> locked = openLocked(m_path, &error);
    if (!locked || !isInIncoming(locked.get(), m_options.storeDir, m_manifest->file, &error)
        || !update::matchesContent(*m_manifest, locked.get(), &error)) {
        locked.reset();
        return fail(error);
    }
    setState(State::Installing);
    emit aboutToInstall();
    QPointer<Updater> self(this);
    auto onExit = [self](bool ok, const QString &why) {
        if (self)
            self->onInstallerExit(ok, why);
    };
    if (!m_installer->launch(m_path, onExit, &error)) {
        locked.reset();
        return fail(error);
    }
    qCInfo(lcUpdate) << "Installing update" << m_manifest->version;
    // Installing until this process exits or the installer fails: the package stays locked and in
    // place, and update() refuses to start another one.
    m_locked = std::move(locked);
    emit finished(m_peer, true, QString(), m_path);
}

void Updater::onInstallerExit(bool ok, const QString &error)
{
    if (m_state != State::Installing)
        return;
    if (ok) {
        qCInfo(lcUpdate) << "The installer finished; the update takes effect when DeskFlower restarts";
        return;
    }
    m_locked.reset();
    fail(error);
}

void Updater::fail(const QString &error)
{
    if (m_state == State::Idle)
        return;
    qCWarning(lcUpdate).noquote() << "Update failed:" << error;
    const QByteArray peer = m_peer;
    resetDownload();
    removeTree(incomingDir(m_options.storeDir));
    emit finished(peer, false, error, QString());
}

void Updater::resetDownload()
{
    m_idle.stop();
    if (m_stream) {
        m_stream->disconnect(this);
        m_stream->close();
        m_stream->deleteLater();
        m_stream = nullptr;
    }
    m_file.close();
    m_manifest.reset();
    m_received = 0;
    m_peer.clear();
    setState(State::Idle);
}

void Updater::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

} // namespace df
