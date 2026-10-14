#include "clipboardsync.h"

#include "clock.h"
#include "fsutil.h"
#include "node.h"

#include <QCborArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcClipboard, "deskflower.clipboard")

namespace df {

namespace {
const QString kT = QStringLiteral("t");
const QStringList kFormats = {QStringLiteral("text/plain"), QStringLiteral("text/html"), QStringLiteral("image/png")};

ClipboardData filtered(const ClipboardData &data)
{
    ClipboardData out;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if (kFormats.contains(it.key()) && !it.value().isEmpty() && it.value().size() <= ClipboardSync::kMaxFormatBytes)
            out.insert(it.key(), it.value());
    }
    return out;
}

QStringList normalized(const QStringList &paths)
{
    QStringList out;
    for (const QString &p : paths)
        out.append(QDir::cleanPath(QDir::fromNativeSeparators(p)));
    out.sort();
    return out;
}
} // namespace

ClipboardSync::ClipboardSync(Node *node, FileTransfer *transfer, Settings *settings, Presence presence,
                             FolderProvider downloads, QObject *parent)
    : QObject(parent),
      m_node(node),
      m_transfer(transfer),
      m_downloads(std::move(downloads)),
      m_settings(settings),
      m_presence(std::move(presence))
{
    connect(m_node, &Node::messageReceived, this, &ClipboardSync::onMessage);
}

bool ClipboardSync::isEnabled() const { return m_settings->value(QStringLiteral("shareClipboard"), true).toBool(); }

void ClipboardSync::setEnabled(bool enabled)
{
    if (isEnabled() == enabled)
        return;
    m_settings->setValue(QStringLiteral("shareClipboard"), enabled);
    emit enabledChanged(enabled);
}

// --- Local changes -----------------------------------------------------------------------------

void ClipboardSync::localChanged(const ClipboardData &raw, const QStringList &rawFiles)
{
    const ClipboardData formats = filtered(raw);
    const QStringList files = normalized(rawFiles);
    if (!isEnabled() || (formats.isEmpty() && files.isEmpty()))
        return;
    if (formats == m_current.formats && files == m_current.files)
        return; // the echo of content we just applied
    qCDebug(lcClipboard) << "local change: formats" << formats.keys() << "vs" << m_current.formats.keys()
                         << "files" << files << "vs" << m_current.files;
    Content content;
    content.stamp = Stamp{std::max<quint64>(m_current.stamp.version + 1, quint64(QDateTime::currentMSecsSinceEpoch())),
                          m_node->id()};
    content.formats = formats;
    content.files = files;
    publish(&content);
    if (m_current.offer)
        m_transfer->unpublish(m_current.offer);
    m_current = content;
    cancelFileFetch();
    if (m_fetchHandle) {
        // A local copy supersedes whatever was being fetched (reset first: cancel runs the callback).
        const quint64 handle = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{};
        m_transfer->cancel(handle);
    }
}

void ClipboardSync::publish(Content *content)
{
    Offer offer;
    for (auto it = content->formats.constBegin(); it != content->formats.constEnd(); ++it) {
        if (it.value().size() > kInlineBytes)
            offer.blobs.insert(it.key(), it.value());
    }
    if (!content->files.isEmpty()) {
        Offer withFiles = offer;
        QString error;
        if (Offer::addPaths(&withFiles, content->files, kMaxFileBytes, kMaxFiles, &error)) {
            offer = withFiles;
            qCInfo(lcClipboard) << "Offering" << offer.files.size() << "copied files," << offer.fileBytes() << "bytes";
            if (content->stamp.author == m_node->id() && !offer.files.isEmpty())
                emit filesNotice(tr("%n copied file(s): move the mouse to another device to paste them there",
                                    nullptr, int(offer.files.size())),
                                 false);
        } else {
            qCInfo(lcClipboard) << "Copied files are not shared:" << error;
            emit filesNotice(tr("The copied files cannot be pasted on other devices: %1").arg(error), true);
        }
    }
    content->fileBytes = offer.fileBytes();
    content->fileCount = int(offer.files.size());
    if (!content->files.isEmpty()) { // our own copied files (not what we keep for another device)
        content->fileList.clear();
        if (offer.files.size() <= kMaxListedFiles) {
            for (const Offer::File &f : std::as_const(offer.files))
                content->fileList.append(ListedFile{f.relativePath, f.size});
        }
    }
    content->offer = (offer.blobs.isEmpty() && offer.files.isEmpty()) ? 0 : m_transfer->publish(offer);
}

// --- Replication -------------------------------------------------------------------------------

QCborMap ClipboardSync::announcement() const
{
    QCborMap inlineFormats;
    QCborMap big;
    for (auto it = m_current.formats.constBegin(); it != m_current.formats.constEnd(); ++it) {
        if (it.value().size() > kInlineBytes && m_current.offer)
            big.insert(it.key(), it.value().size());
        else if (it.value().size() <= kInlineBytes)
            inlineFormats.insert(it.key(), it.value());
    }
    QCborMap msg;
    msg.insert(kT, QStringLiteral("clip"));
    msg.insert(QStringLiteral("v"), qint64(m_current.stamp.version));
    msg.insert(QStringLiteral("a"), m_current.stamp.author);
    msg.insert(QStringLiteral("f"), inlineFormats);
    msg.insert(QStringLiteral("big"), big);
    msg.insert(QStringLiteral("fb"), m_current.fileBytes);
    msg.insert(QStringLiteral("fc"), m_current.fileCount);
    msg.insert(QStringLiteral("o"), qint64(m_current.offer));
    if (m_current.fileCount > 0 && !m_current.fileList.isEmpty()) {
        QCborArray list;
        for (const ListedFile &f : m_current.fileList)
            list.append(QCborArray{f.path, f.size});
        msg.insert(QStringLiteral("fl"), list);
    }
    return msg;
}

void ClipboardSync::sendTo(const QByteArray &peerId)
{
    if (!isEnabled() || m_current.stamp.version == 0 || peerId == m_node->id() || !m_node->isConnected(peerId))
        return;
    m_node->sendMessage(peerId, announcement()); // adopted only if newer than what the peer has
}

void ClipboardSync::cursorMoved(const QByteArray &from, const QByteArray &to)
{
    const QByteArray self = m_node->id();
    if (!isEnabled() || from.isEmpty() || to.isEmpty() || from == to)
        return;
    if (from == self) {
        sendTo(to);
        return;
    }
    if (to == self) {
        m_expectFrom = from;
        m_expectAt = nowMs();
    } else {
        QCborMap expect;
        expect.insert(kT, QStringLiteral("clipFrom"));
        expect.insert(QStringLiteral("s"), from);
        m_node->sendMessage(to, expect);
    }
    QCborMap handOff;
    handOff.insert(kT, QStringLiteral("clipTo"));
    handOff.insert(QStringLiteral("d"), to);
    m_node->sendMessage(from, handOff);
}

void ClipboardSync::onMessage(const QByteArray &peerId, const QCborMap &msg)
{
    const QString type = msg.value(kT).toString();
    if (!isEnabled())
        return;
    if (type == QLatin1String("clipTo")) {
        // The master moved the cursor from here to `d`: hand our clipboard on.
        if (peerId == m_presence.master() && m_presence.cursorHere(kHandOffGraceMs))
            sendTo(msg.value(QStringLiteral("d")).toByteArray());
        return;
    }
    if (type == QLatin1String("clipFrom")) {
        if (peerId == m_presence.master()) {
            m_expectFrom = msg.value(QStringLiteral("s")).toByteArray();
            m_expectAt = nowMs();
        }
        return;
    }
    if (type != QLatin1String("clip"))
        return;
    // Only the clipboard that follows the cursor here.
    const bool expected = peerId == m_presence.master()
                          || (peerId == m_expectFrom && nowMs() - m_expectAt < kExpectMs);
    if (!expected || !m_presence.cursorHere(kHandOffGraceMs)) {
        qCDebug(lcClipboard) << "Ignoring a clipboard from" << peerName(peerId) << "(the cursor did not bring it)";
        return;
    }
    if (peerId == m_expectFrom)
        m_expectFrom.clear(); // one clipboard per hand-off
    const Stamp stamp{quint64(msg.value(QStringLiteral("v")).toInteger()), msg.value(QStringLiteral("a")).toByteArray()};
    if (!stamp.newerThan(m_current.stamp) || (m_fetchHandle && !stamp.newerThan(m_fetchingStamp)))
        return;

    Content content;
    content.stamp = stamp;
    const QCborMap inlineFormats = msg.value(QStringLiteral("f")).toMap();
    for (auto it = inlineFormats.constBegin(); it != inlineFormats.constEnd(); ++it)
        content.formats.insert(it.key().toString(), it.value().toByteArray());
    content.formats = filtered(content.formats);

    qint64 bigBytes = 0;
    const QCborMap big = msg.value(QStringLiteral("big")).toMap();
    for (auto it = big.constBegin(); it != big.constEnd(); ++it)
        bigBytes += it.value().toInteger();
    const qint64 fileBytes = msg.value(QStringLiteral("fb")).toInteger();
    const qint64 fileCount = msg.value(QStringLiteral("fc")).toInteger();
    const quint64 offer = quint64(msg.value(QStringLiteral("o")).toInteger());

    if (!offer || (bigBytes == 0 && fileCount == 0)) {
        adopt(content, peerId);
        return;
    }

    // Copied files are only announced: they are transferred when pasted (fetchFiles).
    const bool lazyFiles = fileCount > 0 && fileBytes <= kMaxFileBytes && fileCount <= kMaxFiles;
    if (fileCount > 0 && !lazyFiles)
        emit filesNotice(tr("Files copied on %1 are too large to share (more than %2 files or 1 GB)")
                             .arg(peerName(peerId))
                             .arg(kMaxFiles),
                         true);
    if (lazyFiles) {
        // The listed names must be safe and complete, or none is used.
        const QCborArray list = msg.value(QStringLiteral("fl")).toArray();
        QList<ListedFile> listed;
        for (const QCborValue &v : list) {
            const QCborArray item = v.toArray();
            const QString path = FileTransfer::sanitizeRelativePath(item.at(0).toString());
            const qint64 size = item.at(1).toInteger();
            if (path.isEmpty() || size < 0) {
                listed.clear();
                break;
            }
            listed.append(ListedFile{path, size});
        }
        if (listed.size() == fileCount)
            content.fileList = listed;
        content.filesFrom = peerId;
        content.remoteOffer = offer;
        content.pendingFiles = int(fileCount);
        content.pendingBytes = fileBytes;
        content.remoteBlobBytes = bigBytes;
    }
    if (bigBytes == 0) {
        adopt(content, peerId);
        return;
    }

    // Large formats: fetch them over TCP before putting anything on the clipboard.
    if (m_fetchHandle) {
        const quint64 previous = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{}; // reset first: cancel runs the old callback
        m_transfer->cancel(previous);
    }
    const QString clipboardDir = QDir(m_downloads()).filePath(QStringLiteral("Clipboard"));
    const QString target = QDir(clipboardDir).filePath(QString::number(stamp.version));
    m_fetchingStamp = stamp;
    qCInfo(lcClipboard) << "Fetching clipboard formats:" << bigBytes << "bytes";
    const quint64 handle = m_transfer->fetch(
        peerId, offer, target, false, qint64(big.size()) * kMaxFormatBytes,
        [this, content, peerId, clipboardDir, stamp](const FetchResult &result) mutable {
            if (!(m_fetchingStamp.version == stamp.version && m_fetchingStamp.author == stamp.author))
                return; // superseded meanwhile
            m_fetchHandle = 0;
            m_fetchingStamp = Stamp{};
            if (result.ok) {
                for (auto it = result.blobs.constBegin(); it != result.blobs.constEnd(); ++it)
                    content.formats.insert(it.key(), it.value());
                content.formats = filtered(content.formats);
            } else {
                qCWarning(lcClipboard) << "Could not fetch clipboard formats:" << result.error;
            }
            if (!stamp.newerThan(m_current.stamp))
                return;
            adopt(content, peerId);
            pruneDownloads(clipboardDir);
        });
    if (m_fetchingStamp.version == stamp.version) // the callback may already have run (peer unreachable)
        m_fetchHandle = handle;
}

void ClipboardSync::adopt(Content content, const QByteArray &from)
{
    if (content.formats.isEmpty() && content.files.isEmpty() && content.pendingFiles == 0)
        return;
    publish(&content); // so devices paired only through us can fetch it from us
    if (m_current.offer)
        m_transfer->unpublish(m_current.offer);
    m_current = content;
    cancelFileFetch();
    if (content.pendingFiles > 0)
        emit filesNotice(tr("%n file(s) copied on %1: paste to transfer them", nullptr, content.pendingFiles)
                             .arg(peerName(from)),
                         false);
    emit applyLocally(m_current.formats, m_current.files, m_current.pendingFiles);
}

void ClipboardSync::fetchFiles(FilesCallback done)
{
    if (m_current.pendingFiles == 0) {
        if (m_current.files.isEmpty())
            done(false, {}, tr("There are no copied files"));
        else
            done(true, m_current.files, {});
        return;
    }
    m_filesWaiters.append(std::move(done));
    if (m_filesHandle)
        return; // already on its way
    const Stamp stamp = m_current.stamp;
    const QByteArray peer = m_current.filesFrom;
    const QString clipboardDir = QDir(m_downloads()).filePath(QStringLiteral("Clipboard"));
    const QString target = QDir(clipboardDir).filePath(QString::number(stamp.version));
    qCInfo(lcClipboard) << "Pasting" << m_current.pendingFiles << "files," << m_current.pendingBytes << "bytes";
    emit filesNotice(tr("Receiving %n copied file(s) from %1…", nullptr, m_current.pendingFiles).arg(peerName(peer)),
                     false);
    const quint64 handle = m_transfer->fetch(
        peer, m_current.remoteOffer, target, true, m_current.pendingBytes + m_current.remoteBlobBytes,
        [this, stamp, peer, clipboardDir](const FetchResult &result) {
            m_filesHandle = 0;
            const QList<FilesCallback> waiters = std::exchange(m_filesWaiters, {});
            const bool current = m_current.stamp.version == stamp.version && m_current.stamp.author == stamp.author;
            QString error;
            if (!current)
                error = tr("The clipboard changed meanwhile");
            else if (!result.ok)
                error = result.error;
            if (!error.isEmpty()) {
                qCWarning(lcClipboard) << "Could not fetch the copied files:" << error;
                if (current)
                    emit filesNotice(tr("Could not get the copied files from %1: %2").arg(peerName(peer), error), true);
                for (const FilesCallback &w : waiters)
                    w(false, {}, error);
                return;
            }
            m_current.files = normalized(result.files);
            m_current.pendingFiles = 0;
            qCInfo(lcClipboard) << "Fetched" << m_current.files.size() << "copied files";
            emit filesNotice(tr("%n file(s) received from %1", nullptr, int(m_current.files.size())).arg(peerName(peer)),
                             false);
            pruneDownloads(clipboardDir);
            for (const FilesCallback &w : waiters)
                w(true, m_current.files, {});
        });
    if (!m_filesWaiters.isEmpty()) // otherwise the callback already ran (peer unreachable)
        m_filesHandle = handle;
}

void ClipboardSync::cancelFileFetch()
{
    if (!m_filesHandle)
        return;
    // The callback runs (and fails the waiters: the stamp no longer matches).
    m_transfer->cancel(std::exchange(m_filesHandle, 0));
}

QString ClipboardSync::peerName(const QByteArray &peerId) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == peerId)
            return p.name;
    }
    return QString::fromLatin1(peerId.toHex().left(8));
}

void ClipboardSync::pruneDownloads(const QString &clipboardDir) const
{
    QDir dir(clipboardDir);
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    // Folder names are stamps (milliseconds): the lexicographic order of equal-length numbers is
    // chronological, so sort numerically to be safe.
    std::sort(entries.begin(), entries.end(),
              [](const QString &a, const QString &b) { return a.toLongLong() < b.toLongLong(); });
    while (entries.size() > kKeptDownloads)
        fs::removeTree(dir.filePath(entries.takeFirst()));
}

} // namespace df
