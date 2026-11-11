#include "clipboardsync.h"

#include "clock.h"
#include "fsutil.h"
#include "node.h"

#include <QCborArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QLoggingCategory>
#include <QVersionNumber>
#include <QRandomGenerator>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tuple>
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

// Binds a ciphertext to its id and format, so one cannot be passed off as another.
QByteArray sealAd(const QByteArray &kid, const QString &mime) { return kid + mime.toUtf8(); }
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
    // A master that was unreachable when we copied gets the key once it connects; one that comes
    // back gets it again (a restart lost its escrow, and whatever was queued for it went with the
    // old boot's channel).
    connect(m_node, &Node::peerConnected, this, [this](const QByteArray &peerId) {
        if (peerId == m_keyEscrowedWith)
            m_keyEscrowedWith.clear();
        masterChanged();
    });
    connect(m_node, &Node::peerDisconnected, this, [this](const QByteArray &peerId) {
        if (peerId == m_keyEscrowedWith)
            m_keyEscrowedWith.clear();
        m_synced.remove(peerId);
    });
    // A trusted device is sent the current clipboard once its hello said what it runs.
    connect(m_node, &Node::peersChanged, this, &ClipboardSync::onPeersChanged);
    m_formatTimer.setSingleShot(true);
    connect(&m_formatTimer, &QTimer::timeout, this,
            [this] { failFormatWaiters(tr("The device holding the clipboard did not answer")); });
    m_filesGrantTimer.setSingleShot(true);
    connect(&m_filesGrantTimer, &QTimer::timeout, this, [this] {
        if (m_ownAllowArid) {
            // The master's own paste of files adopted from a peer: the publisher never allowed it.
            m_pendingAllows.remove(std::exchange(m_ownAllowArid, 0));
            failFilesWaiters(tr("The device with the files did not answer"));
            return;
        }
        if (!m_filesRid)
            return;
        m_filesRid = 0;
        failFilesWaiters(tr("The device holding the clipboard did not answer"));
    });
    // A files request waiting for a paste gesture (fetchFiles): its grace is over.
    m_evidenceTimer.setSingleShot(true);
    connect(&m_evidenceTimer, &QTimer::timeout, this, [this] {
        if (!m_job || m_job->handle)
            return;
        if (jobHasPasteEvidence())
            proceedFetchFiles();
        else
            refuseNoPaste();
    });
}

struct ClipboardSync::Walk {
    std::thread thread;
    std::atomic_bool finished = false;
};

ClipboardSync::~ClipboardSync()
{
    if (m_list.handle) // its callback must not outlive this object
        m_transfer->cancelList(std::exchange(m_list.handle, 0));
    // Nor must those of the files transfers (each callback checks `done` first).
    std::vector<std::shared_ptr<FilesJob>> jobs = std::exchange(m_detachedJobs, {});
    if (m_job)
        jobs.push_back(std::exchange(m_job, {}));
    for (const auto &job : jobs) {
        job->done = true;
        if (job->handle)
            m_transfer->cancel(std::exchange(job->handle, 0));
    }
    for (const auto &walk : m_walks) {
        if (walk->thread.joinable())
            walk->thread.join();
    }
    wipeContentKeys(&m_walking);
    wipeContentKeys(&m_current);
    for (auto it = m_escrow.begin(); it != m_escrow.end(); ++it)
        crypto::wipe(it->key);
    for (auto it = m_pendingAllows.begin(); it != m_pendingAllows.end(); ++it)
        crypto::wipe(it->key);
}

bool ClipboardSync::isEnabled() const { return m_settings->value(QStringLiteral("shareClipboard"), true).toBool(); }

void ClipboardSync::setEnabled(bool enabled)
{
    if (isEnabled() == enabled)
        return;
    m_settings->setValue(QStringLiteral("shareClipboard"), enabled);
    emit enabledChanged(enabled);
    if (enabled)
        sendToTrusted(); // the trusted devices connected meanwhile were sent nothing
}

bool ClipboardSync::isMaster() const { return m_presence.master && master() == m_node->id(); }

void ClipboardSync::wipeContentKeys(Content *content)
{
    crypto::wipe(content->key);
    for (auto it = content->plainCache.begin(); it != content->plainCache.end(); ++it)
        crypto::wipe(it.value());
    content->plainCache.clear();
    content->grantedAt = -1;
    for (auto it = content->formats.begin(); it != content->formats.end(); ++it)
        crypto::wipe(it.value()); // our own plaintext (the ciphertext may stay)
    content->formats.clear();
}

void ClipboardSync::replaceCurrent(Content content)
{
    for (const quint64 offer : {m_current.offer, m_current.fileOffer, m_current.legacyOffer}) {
        if (offer)
            m_transfer->unpublish(offer);
    }
    wipeContentKeys(&m_current);
    m_current = std::move(content); // moved: no second copy of the key outlives the wipe
    m_localNewer = false;
    m_applied = false;
    endFilesWaiting(); // a Receive offered for the previous content is over
    cancelFileFetch();
    dropList();
    failFormatWaiters(tr("The clipboard changed"), false); // replaced: nothing went wrong
}

void ClipboardSync::endFilesWaiting()
{
    if (m_waitingToldKid.isEmpty())
        return;
    const QByteArray kid = std::exchange(m_waitingToldKid, QByteArray());
    emit filesWaiting(kid, 0, {});
}

// --- The logical clock -------------------------------------------------------------------------

void ClipboardSync::noteStamp(quint64 version)
{
    if (qint64(version) <= wallNow() + kMaxClockAheadMs)
        m_clock = std::max(m_clock, version);
}

quint64 ClipboardSync::nextStamp() const
{
    // After everything seen, whatever the local clock says: a copy made after another one wins
    // over it even if this device's clock is behind.
    return std::max({m_current.stamp.version + 1, m_clock + 1, quint64(std::max<qint64>(wallNow(), 0))});
}

// --- Local changes -----------------------------------------------------------------------------

void ClipboardSync::localChanged(const ClipboardData &raw, const QStringList &rawFiles)
{
    const ClipboardData formats = filtered(raw);
    const QStringList files = normalized(rawFiles);
    // Something was copied here: even if it is not shared, it is newer than a clipboard adopted
    // from another device, which must not be put back over it (reapply).
    if (!raw.isEmpty() || !rawFiles.isEmpty())
        m_localNewer = m_current.stamp.author != m_node->id();
    if (!isEnabled())
        return;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
        if (kFormats.contains(it.key()) && it.value().size() > kMaxFormatBytes) {
            qCInfo(lcClipboard) << "Not sharing" << it.key() << "of" << it.value().size() << "bytes (too large)";
            noticeOnce(QStringLiteral("tooBig"),
                       tr("The copied content is too large to share with other devices (more than %1 MB)")
                           .arg(kMaxFormatBytes / (1024 * 1024)),
                       true);
        }
    }
    if (formats.isEmpty() && files.isEmpty())
        return;
    if (m_walkSeq && formats == m_walking.formats && files == m_walking.files)
        return; // the same copy reported again while its folders are walked
    // Anything else copied here supersedes the copy whose files are still being listed, even the
    // current content copied again (which is then no echo: the clipboard holds it once more).
    cancelWalk();
    if (formats == m_current.formats && files == m_current.files && !m_current.formats.isEmpty())
        return; // the echo of content we just applied
    qCDebug(lcClipboard) << "local change: formats" << formats.keys() << "files" << files.size();
    Content content;
    content.stamp = Stamp{nextStamp(), m_node->id()};
    content.formats = formats;
    content.files = files;
    content.sealedHere = true;
    sealContent(&content);
    if (files.isEmpty()) {
        finishLocalChange(std::move(content));
        return;
    }

    // The copied folders are walked on a worker thread, as the console user (FileTransfer::
    // addOfferPaths): thousands of files take a while, and this thread routes input meanwhile.
    m_walks.erase(std::remove_if(m_walks.begin(), m_walks.end(),
                                 [](const std::shared_ptr<Walk> &w) {
                                     if (!w->finished)
                                         return false;
                                     w->thread.join();
                                     return true;
                                 }),
                  m_walks.end());
    if (m_fetchHandle) { // a local copy supersedes whatever announcement was being fetched
        const quint64 handle = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{};
        m_transfer->cancel(handle);
    }
    const quint64 seq = ++m_walkCounter;
    m_walkSeq = seq;
    m_walkHandOff.clear();
    m_walking = std::move(content);
    auto walk = std::make_shared<Walk>();
    const Impersonator impersonate = m_transfer->impersonator();
    QElapsedTimer started;
    started.start();
    walk->thread = std::thread([this, walk, impersonate, files, seq, started] {
        auto offer = std::make_shared<Offer>();
        offer->gated = true;
        QString error;
        bool ok = false;
        {
            const std::shared_ptr<void> asUser = impersonate ? impersonate() : std::make_shared<int>(0);
            if (!asUser)
                error = QStringLiteral("Nobody is signed in");
            else
                ok = Offer::addPaths(offer.get(), files, kMaxFileBytes, kMaxFiles, &error);
        }
        // The destructor joins this thread, so `this` outlives the call; the queued call is
        // dropped if the object is gone by then.
        QMetaObject::invokeMethod(this, [this, offer, ok, error, seq, started] {
            qCInfo(lcClipboard) << "Listed the copied files in" << started.elapsed() << "ms:" << offer->files.size()
                                << "files," << offer->dirs.size() << "folders," << offer->skipped.size()
                                << "left out" << (ok ? "" : "(refused)");
            if (seq != m_walkSeq)
                return; // a newer local copy replaced it (or the clipboard was reset)
            m_walkSeq = 0;
            Content content = std::exchange(m_walking, Content{});
            if (!isEnabled()) { // sharing was turned off meanwhile
                wipeContentKeys(&content);
                return;
            }
            publishFiles(&content, std::move(*offer), ok, error);
            finishLocalChange(std::move(content));
            if (const QByteArray to = std::exchange(m_walkHandOff, {}); !to.isEmpty()) {
                if (m_walkHandOffOurs)
                    cursorMoved(m_node->id(), to); // records what `to` holds now, and sends it
                else
                    sendTo(to);
            }
        });
        walk->finished = true;
    });
    m_walks.push_back(walk);
}

void ClipboardSync::cancelWalk()
{
    if (!m_walkSeq)
        return;
    m_walkSeq = 0; // its thread runs to its end; the result is dropped
    m_walkHandOff.clear();
    wipeContentKeys(&m_walking);
    m_walking = Content{};
}

Stamp ClipboardSync::latestStamp() const
{
    return m_walkSeq && m_walking.stamp.newerThan(m_current.stamp) ? m_walking.stamp : m_current.stamp;
}

void ClipboardSync::finishLocalChange(Content content)
{
    publishBlobs(&content);
    if (m_fetchHandle) { // a local copy supersedes whatever announcement was being fetched
        const quint64 handle = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{};
        m_transfer->cancel(handle);
    }
    replaceCurrent(std::move(content));
    escrowCurrentKey();
    sendToTrusted();
}

QString ClipboardSync::fetchErrorName(FetchError code)
{
    switch (code) {
    case FetchError::None:
        return {};
    case FetchError::NoPaste:
        return QStringLiteral("noPaste");
    case FetchError::Cancelled:
        return QStringLiteral("cancelled");
    case FetchError::Changed:
        return QStringLiteral("changed");
    case FetchError::Other:
        break;
    }
    return QStringLiteral("other");
}

void ClipboardSync::reset()
{
    if (m_fetchHandle) {
        const quint64 handle = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{};
        m_transfer->cancel(handle);
    }
    cancelWalk(); // a copy still being listed belongs to the previous user
    replaceCurrent(Content{});
    // Every transfer goes, those streaming pastes still read included: they act for the previous
    // user (its own checks stop a transfer whose user changed before this runs).
    const std::vector<std::shared_ptr<FilesJob>> detached = m_detachedJobs;
    for (const auto &job : detached)
        cancelFilesJob(job, FetchError::Changed);
    m_detachedJobs.clear();
    m_keyEscrowedWith.clear();
    m_expectFrom.clear();
    m_expectAt = 0;
    for (auto it = m_escrow.begin(); it != m_escrow.end(); ++it)
        crypto::wipe(it->key);
    m_escrow.clear();
    m_escrowOrder.clear();
    m_holds.clear();
    m_lastFrom.clear();
    m_heldUnseals.clear();
    for (auto it = m_pendingAllows.begin(); it != m_pendingAllows.end(); ++it)
        crypto::wipe(it->key);
    m_pendingAllows.clear();
    m_ownAllowArid = 0;
    m_grantUntil.clear();
    m_appliedStamp = Stamp{};
    m_appliedAt = -1;
    m_waitingToldKid.clear();
    m_noPasteKid.clear();
    m_noPasteAt = -1;
}

bool ClipboardSync::reapply()
{
    // Content copied here is on the local clipboard already (the application that copied it owns
    // it); only what came from another device would be lost with the owner that went away.
    if (!isEnabled() || m_current.stamp.version == 0 || m_current.stamp.author == m_node->id())
        return false;
    if (m_localNewer)
        return false; // the user copied something here since (not shared): it stays
    if (m_current.sealed.isEmpty() && m_current.files.isEmpty() && pendingItems() == 0)
        return false;
    emitApply();
    return true;
}

void ClipboardSync::sealContent(Content *content)
{
    content->kid = crypto::randomBytes(crypto::kDeviceIdSize);
    content->key = crypto::sealKey();
    for (auto it = content->formats.constBegin(); it != content->formats.constEnd(); ++it) {
        content->sealed.insert(it.key(), crypto::seal(content->key, sealAd(content->kid, it.key()), it.value()));
        content->plainSizes.insert(it.key(), it.value().size());
    }
}

void ClipboardSync::escrowCurrentKey()
{
    // The key is held by the master (escrowed) so a guest (or a device that got the ciphertext
    // without the key) can ask it for a paste; the device that sealed keeps it too. The escrow is
    // filed under the sealer, not the stamp's author: an older peer's plaintext is re-sealed by
    // whoever received it, and the master accepts a key only from its sender.
    if (m_current.key.isEmpty() || !m_current.sealedHere)
        return;
    const QByteArray master = this->master();
    if (master.isEmpty() || m_node->isGuest(master)) {
        m_keyEscrowedWith.clear();
        return;
    }
    if (master == m_node->id()) {
        escrowKey(m_current.kid, m_current.key, m_node->id(), m_current.stamp);
        m_keyEscrowedWith = master;
        return;
    }
    if (peerKnownOlder(master)) {
        // An older master knows no escrow: a paste that needs it fails fast and says so.
        qCInfo(lcClipboard) << "The master" << peerName(master) << "runs an older version: it cannot hold the key";
        noticeOnce(QStringLiteral("oldMaster:") + QString::fromLatin1(master.toHex()),
                   tr("%1 shares its keyboard and mouse but runs an older DeskFlower: update it there to paste "
                      "this clipboard on guest devices")
                       .arg(peerName(master)),
                   true, 60000);
        m_keyEscrowedWith.clear();
        return;
    }
    QCborMap m;
    m.insert(kT, QStringLiteral("clip.key"));
    m.insert(QStringLiteral("kid"), m_current.kid);
    m.insert(QStringLiteral("k"), m_current.key);
    m.insert(QStringLiteral("v"), qint64(m_current.stamp.version));
    m.insert(QStringLiteral("a"), m_node->id());
    m_keyEscrowedWith = m_node->sendMessage(master, m) ? master : QByteArray();
}

void ClipboardSync::masterChanged()
{
    refreshGate(); // a new master may treat this device otherwise (guest or trusted)
    if (m_current.key.isEmpty() || !m_current.sealedHere || !isEnabled())
        return;
    const QByteArray master = this->master();
    if (master.isEmpty() || master == m_keyEscrowedWith)
        return;
    escrowCurrentKey();
}

bool ClipboardSync::peerRunsAtLeast(const QByteArray &peerId, const char *version) const
{
    const QVersionNumber v = QVersionNumber::fromString(peerVersion(peerId));
    return !v.isNull() && v >= QVersionNumber::fromString(QLatin1String(version));
}

bool ClipboardSync::peerKnownOlder(const QByteArray &peerId) const
{
    // Only a version it declared counts: right after a connection its hello may still be on the
    // way.
    const QVersionNumber v = QVersionNumber::fromString(peerVersion(peerId));
    return !v.isNull() && v < QVersionNumber::fromString(QLatin1String(kSealingVersion));
}

QString ClipboardSync::peerVersion(const QByteArray &peerId) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == peerId)
            return p.version;
    }
    return {};
}

bool ClipboardSync::sharesEveryCopy(const QByteArray &peerId) const
{
    return peerId != m_node->id() && m_node->isConnected(peerId) && !m_node->isGuest(peerId)
           && peerRunsAtLeast(peerId, kSharedClipboardVersion);
}

bool ClipboardSync::transfersPossible(const QByteArray &peerId, bool files, bool formats)
{
    const QString reason = m_transfer->incompatibleReason(peerId);
    if (reason.isEmpty())
        return true;
    if (files || formats) {
        qCInfo(lcClipboard) << "Not taking the copied files or large formats of" << peerName(peerId)
                            << "(another transfer protocol)";
        noticeOnce(QStringLiteral("protocol:") + QString::fromLatin1(peerId.toHex()),
                   files ? tr("Files copied on %1 cannot be pasted here: %2").arg(peerName(peerId), reason)
                         : tr("Part of the clipboard of %1 could not be received: %2").arg(peerName(peerId), reason),
                   true);
    }
    return false;
}

void ClipboardSync::noticeOnce(const QString &kind, const QString &text, bool problem, qint64 intervalMs)
{
    const qint64 now = nowMs();
    const auto it = m_noticeAt.constFind(kind);
    if (it != m_noticeAt.constEnd() && now - it.value() < intervalMs)
        return;
    m_noticeAt.insert(kind, now);
    emit notice(text, problem);
}

bool ClipboardSync::parseListing(const QCborMap &msg, qint64 fileCount, qint64 dirCount, QList<ListedFile> *out)
{
    QList<ListedFile> listed;
    if (!listedFromCbor(msg.value(QStringLiteral("fl")), &listed))
        return false;
    const qint64 dirs = listedDirCount(listed);
    if (dirs != dirCount || listed.size() - dirs != fileCount)
        return false;
    *out = listed;
    return true;
}

void ClipboardSync::publish(Content *content)
{
    publishBlobs(content);
    if (!content->files.isEmpty()) {
        Offer files;
        files.gated = true;
        QString error;
        const bool ok = m_transfer->addOfferPaths(&files, content->files, kMaxFileBytes, kMaxFiles, &error);
        publishFiles(content, std::move(files), ok, error);
    }
}

void ClipboardSync::publishBlobs(Content *content)
{
    // Sealed ciphertext too big to inline: a gated offer, harmless to serve (no key), that each
    // recipient is let into when the clipboard is sent to it.
    Offer blobs;
    blobs.gated = true;
    for (auto it = content->sealed.constBegin(); it != content->sealed.constEnd(); ++it) {
        if (it.value().size() > kInlineBytes)
            blobs.blobs.insert(it.key(), it.value());
    }
    content->offer = blobs.blobs.isEmpty() ? 0 : m_transfer->publish(blobs);
}

void ClipboardSync::publishFiles(Content *content, Offer files, bool ok, const QString &error)
{
    // Copied files (plaintext): a separate gated offer, let in for trusted recipients when sent,
    // for a guest only on a paste grant.
    {
        if (ok) {
            qCInfo(lcClipboard) << "Offering" << files.files.size() << "copied files and" << files.dirs.size()
                                << "folders," << files.fileBytes() << "bytes";
            content->fileBytes = files.fileBytes();
            content->fileCount = int(files.files.size());
            content->dirCount = int(files.dirs.size());
            content->fileList.clear();
            if (files.files.size() + files.dirs.size() <= kMaxListedFiles)
                content->fileList = files.listing();
            // Short paths (sp): a receiver may paste them before their contents arrive.
            qsizetype longest = 0;
            for (const Offer::File &f : std::as_const(files.files))
                longest = std::max(longest, f.relativePath.size());
            for (const QString &d : std::as_const(files.dirs))
                longest = std::max(longest, d.size());
            content->shortPaths = files.hasFiles() && longest <= kMaxVirtualPath;
            // A copy of folders alone (empty ones included) is a valid copy too.
            content->fileOffer = files.hasFiles() ? m_transfer->publish(files) : 0;
            content->ownFiles = files;
            const bool own = content->stamp.author == m_node->id();
            if (own && !files.skipped.isEmpty())
                emit filesNotice(tr("%n copied item(s) cannot be pasted on other devices (links or special files)",
                                    nullptr, int(files.skipped.size())),
                                 true);
            if (own && !files.files.isEmpty())
                emit filesNotice(tr("%n copied file(s): paste them on another device to transfer them", nullptr,
                                    int(files.files.size())),
                                 false);
            else if (own && !files.dirs.isEmpty())
                emit filesNotice(tr("%n copied folder(s): paste them on another device to transfer them", nullptr,
                                    int(files.dirs.size())),
                                 false);
        } else {
            qCInfo(lcClipboard) << "Copied files are not shared:" << error;
            emit filesNotice(tr("The copied files cannot be pasted on other devices: %1").arg(error), true);
        }
    }
}

// --- Replication -------------------------------------------------------------------------------

QCborMap ClipboardSync::announcement() const
{
    QCborMap inlineFormats;
    QCborMap big;
    for (auto it = m_current.sealed.constBegin(); it != m_current.sealed.constEnd(); ++it) {
        if (it.value().size() > kInlineBytes && m_current.offer)
            big.insert(it.key(), it.value().size());
        else if (it.value().size() <= kInlineBytes)
            inlineFormats.insert(it.key(), it.value());
    }
    QCborMap msg;
    msg.insert(kT, QStringLiteral("clip"));
    msg.insert(QStringLiteral("v"), qint64(m_current.stamp.version));
    msg.insert(QStringLiteral("a"), m_current.stamp.author);
    msg.insert(QStringLiteral("kid"), m_current.kid);
    msg.insert(QStringLiteral("sf"), inlineFormats);
    msg.insert(QStringLiteral("big"), big);
    msg.insert(QStringLiteral("fb"), m_current.fileBytes);
    msg.insert(QStringLiteral("fc"), m_current.fileCount);
    if (m_current.dirCount > 0)
        msg.insert(QStringLiteral("dc"), m_current.dirCount);
    msg.insert(QStringLiteral("o"), qint64(m_current.offer));
    msg.insert(QStringLiteral("fo"), qint64(m_current.fileOffer));
    if (m_current.fileCount + m_current.dirCount > 0 && !m_current.fileList.isEmpty())
        msg.insert(QStringLiteral("fl"), listedToCbor(m_current.fileList));
    // Sent only when true: an older receiver ignores it, and its absence means "unknown".
    if (m_current.fileCount + m_current.dirCount > 0 && m_current.shortPaths)
        msg.insert(QStringLiteral("sp"), true);
    return msg;
}

void ClipboardSync::sendToTrusted()
{
    for (const PeerView &p : m_node->peers()) {
        if (!sharesEveryCopy(p.id))
            continue;
        m_synced.insert(p.id);
        sendTo(p.id, true);
    }
}

void ClipboardSync::onPeersChanged()
{
    // A trusted device that just connected (its hello arrived, so its version is known) is sent
    // the current clipboard: it adopts it only if newer than its own, and sends its own back if
    // that is newer. One that disconnects is sent it again at its next connection.
    for (const PeerView &p : m_node->peers()) {
        if (!p.connected) {
            m_synced.remove(p.id);
            continue;
        }
        if (m_synced.contains(p.id) || !sharesEveryCopy(p.id) || !isEnabled() || m_current.stamp.version == 0)
            continue; // nothing sent: a later copy, or turning sharing on, sends it (sendToTrusted)
        m_synced.insert(p.id);
        sendTo(p.id, true);
    }
    refreshGate(); // the master's declared trust arrives with its hello
}

void ClipboardSync::sendTo(const QByteArray &peerId, bool everyCopy)
{
    if (!isEnabled() || m_current.stamp.version == 0 || peerId == m_node->id() || !m_node->isConnected(peerId))
        return;
    if (peerKnownOlder(peerId)) {
        sendLegacy(peerId);
        return;
    }
    if (!peerRunsAtLeast(peerId, kSealingVersion)) {
        // Its hello has not arrived yet: a trusted device gets the clipboard once it does
        // (onPeersChanged).
        qCInfo(lcClipboard) << "Not sending the clipboard to" << peerName(peerId) << "yet (version unknown)";
        return;
    }
    QCborMap msg = announcement();
    if (m_node->isGuest(peerId)) {
        // A guest gets the ciphertext without the key (it pastes only with the master's grant), and
        // not even the copied files' names: only their count and size. Nor how long their paths
        // are (sp): a guest never pastes them before their contents arrive.
        msg.remove(QStringLiteral("fl"));
        msg.remove(QStringLiteral("sp"));
    } else {
        if (!peerUnderstandsFolders(peerVersion(peerId)) && m_current.dirCount > 0) {
            // A peer that knows no folder entries would drop the whole list over one: it gets the
            // files alone (folders holding files still arrive through their paths).
            msg.remove(QStringLiteral("dc"));
            if (!m_current.fileList.isEmpty())
                msg.insert(QStringLiteral("fl"), listedToCbor(m_current.fileList, false));
        }
        // A trusted device decrypts when the user pastes there, with no further question, and
        // fetches the copied files from us directly.
        if (!m_current.key.isEmpty())
            msg.insert(QStringLiteral("k"), m_current.key);
        if (m_current.fileOffer) {
            m_transfer->allowPeer(m_current.fileOffer, peerId, true);
            msg.insert(QStringLiteral("fa"), true);
        }
    }
    if (everyCopy)
        msg.insert(QStringLiteral("bc"), true);
    m_node->sendMessage(peerId, msg); // adopted only if newer than what the peer has
    // Let the recipient fetch the sealed ciphertext from us (useless without the key).
    if (m_current.offer)
        m_transfer->allowPeer(m_current.offer, peerId, true);
}

void ClipboardSync::sendLegacy(const QByteArray &peerId)
{
    // A peer from before sealing would take ciphertext for content: it gets plaintext, as 0.1.19
    // sends it, but only if it is not a guest (a guest pastes only with a grant, which it knows
    // nothing of) and we can decrypt the content (it may have come here without the key).
    const QString name = peerName(peerId);
    const QString kind = QStringLiteral("older:") + QString::fromLatin1(peerId.toHex());
    if (m_node->isGuest(peerId)) {
        qCInfo(lcClipboard) << "Not sending the clipboard to" << name << "(an older guest)";
        noticeOnce(kind, tr("%1 runs an older DeskFlower: as a guest it gets no clipboard until it is updated").arg(name),
                   true, 60000);
        return;
    }
    if (m_current.key.isEmpty() && !m_current.sealed.isEmpty()) {
        qCInfo(lcClipboard) << "Cannot send the clipboard to" << name << "(older version, no key here)";
        noticeOnce(kind,
                   tr("The clipboard could not be passed on to %1, which runs an older DeskFlower: update it there")
                       .arg(name),
                   true, 60000);
        return;
    }
    ClipboardData plain;
    for (auto it = m_current.sealed.constBegin(); it != m_current.sealed.constEnd(); ++it) {
        if (auto p = crypto::unseal(m_current.key, sealAd(m_current.kid, it.key()), it.value()))
            plain.insert(it.key(), *p);
    }
    QCborMap inlineFormats;
    QCborMap big;
    QMap<QString, QByteArray> blobs;
    for (auto it = plain.constBegin(); it != plain.constEnd(); ++it) {
        if (it.value().size() > kInlineBytes) {
            big.insert(it.key(), it.value().size());
            blobs.insert(it.key(), it.value());
        } else {
            inlineFormats.insert(it.key(), it.value());
        }
    }
    if (!m_current.legacyOffer && (!blobs.isEmpty() || m_current.ownFiles.hasFiles())) {
        // One offer with the large formats and the files, as 0.1.19 expects; made once per
        // clipboard, and opened to each older peer it is sent to.
        Offer legacy = m_current.ownFiles;
        legacy.blobs = blobs;
        legacy.allowed.clear();
        legacy.gated = true;
        m_current.legacyOffer = m_transfer->publish(legacy);
    }
    for (auto it = plain.begin(); it != plain.end(); ++it)
        crypto::wipe(it.value());
    if (m_current.legacyOffer)
        m_transfer->allowPeer(m_current.legacyOffer, peerId, true);
    QCborMap msg;
    msg.insert(kT, QStringLiteral("clip"));
    msg.insert(QStringLiteral("v"), qint64(m_current.stamp.version));
    msg.insert(QStringLiteral("a"), m_current.stamp.author);
    msg.insert(QStringLiteral("f"), inlineFormats);
    msg.insert(QStringLiteral("big"), big);
    msg.insert(QStringLiteral("fb"), m_current.fileBytes);
    msg.insert(QStringLiteral("fc"), m_current.fileCount);
    msg.insert(QStringLiteral("o"), qint64(m_current.legacyOffer));
    if (m_current.fileCount > 0 && !m_current.fileList.isEmpty())
        msg.insert(QStringLiteral("fl"), listedToCbor(m_current.fileList, false));
    qCInfo(lcClipboard) << "Sending the clipboard to" << name << "as an older version understands it";
    m_node->sendMessage(peerId, msg);
}

void ClipboardSync::cursorMoved(const QByteArray &from, const QByteArray &to)
{
    const QByteArray self = m_node->id();
    if (!isEnabled() || from.isEmpty() || to.isEmpty() || from == to)
        return;
    // What `to` will hold once the hand-off goes through (it adopts only a newer clipboard, as
    // noteHeld records), so that a guest is granted that clipboard and no other; and where it
    // came from, for a key still on its way (guestMayUnseal).
    if (to != self) {
        const Held held = from == self ? Held{m_current.kid, m_current.stamp} : m_holds.value(from);
        if (!held.kid.isEmpty())
            noteHeld(to, held.kid, held.stamp);
        m_lastFrom.insert(to, from);
    }
    if (from == self) {
        if (m_walkSeq) { // the copy being listed goes too, once listed
            m_walkHandOff = to;
            m_walkHandOffOurs = true;
        }
        sendTo(to);
        return;
    }
    if (m_walkHandOffOurs && !m_walkHandOff.isEmpty() && from == m_walkHandOff)
        m_walkHandOff = to == self ? QByteArray() : to; // the copy being listed follows the cursor
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
    if (!type.startsWith(QLatin1String("clip")))
        return;
    if (!isEnabled())
        return;
    // A guest is never this device's master (even if a third device relays it as one).
    const bool fromMaster = m_presence.master && peerId == master() && !m_node->isGuest(peerId);

    if (type == QLatin1String("clip.key")) {
        // Master side: the device that sealed escrows the key with us, under its own identity (so
        // a third device cannot file a key as someone else's).
        if (!isMaster())
            return;
        const QByteArray kid = msg.value(QStringLiteral("kid")).toByteArray();
        const QByteArray sealer = msg.value(QStringLiteral("a")).toByteArray();
        const QByteArray key = msg.value(QStringLiteral("k")).toByteArray();
        if (kid.isEmpty() || sealer != peerId || key.size() != crypto::kKeySize)
            return;
        const Stamp stamp{quint64(msg.value(QStringLiteral("v")).toInteger()), sealer};
        if (!m_node->isGuest(peerId))
            noteStamp(stamp.version);
        escrowKey(kid, key, sealer, stamp);
        noteHeld(sealer, kid, stamp); // what it sealed is what it holds
        if (kid == m_current.kid && m_current.key.isEmpty())
            m_current.key = key; // the master holds every key: it renders and hands on ungated
        // Flush any unseal held for a key that had not arrived.
        for (auto it = m_heldUnseals.begin(); it != m_heldUnseals.end();) {
            if (it->msg.value(QStringLiteral("kid")).toByteArray() == kid) {
                const HeldUnseal held = *it;
                it = m_heldUnseals.erase(it);
                handleUnsealRequest(held.peer, held.msg);
            } else {
                ++it;
            }
        }
        return;
    }
    if (type == QLatin1String("clip.unseal")) {
        if (isMaster())
            handleUnsealRequest(peerId, msg);
        return;
    }
    if (type == QLatin1String("clip.unsealed")) {
        // Our request's answer, from the master.
        if (!fromMaster)
            return;
        const quint64 rid = quint64(msg.value(QStringLiteral("rid")).toInteger());
        const bool ok = msg.value(QStringLiteral("ok")).toBool();
        const bool keep = msg.value(QStringLiteral("keep")).toBool();
        const QByteArray key = msg.value(QStringLiteral("k")).toByteArray();
        if (rid == 0)
            return;
        if (rid == m_formatRid) {
            m_formatTimer.stop();
            m_formatRid = 0;
            if (ok)
                onUnsealed(key, keep);
            else
                failFormatWaiters(msg.value(QStringLiteral("r")).toString());
        } else if (rid == m_filesRid) {
            m_filesGrantTimer.stop();
            m_filesRid = 0;
            if (ok && keep && m_current.key.isEmpty() && keyOpens(m_current, key))
                m_current.key = key;
            refreshGate();
            if (ok)
                doFetchFiles();
            else
                failFilesWaiters(msg.value(QStringLiteral("r")).toString());
        }
        return;
    }
    if (type == QLatin1String("clip.allow")) {
        // Publisher side: the master tells us to let a device fetch our copied files for a paste.
        if (!fromMaster)
            return;
        // Only our current clipboard's files, and only for a paste of that very clipboard: a
        // grant for another clipboard (one the device holds legitimately) must not open ours.
        const quint64 offer = quint64(msg.value(QStringLiteral("o")).toInteger());
        const QByteArray device = msg.value(QStringLiteral("d")).toByteArray();
        const bool ok = offer && offer == m_current.fileOffer
                        && msg.value(QStringLiteral("kid")).toByteArray() == m_current.kid;
        if (ok)
            m_transfer->allowPeer(offer, device, true);
        QCborMap r;
        r.insert(kT, QStringLiteral("clip.allowed"));
        r.insert(QStringLiteral("arid"), msg.value(QStringLiteral("arid")));
        r.insert(QStringLiteral("ok"), ok);
        m_node->sendMessage(peerId, r);
        return;
    }
    if (type == QLatin1String("clip.allowed")) {
        // Master side: the publisher let the device in; now hand it the key.
        if (!isMaster())
            return;
        const quint64 arid = quint64(msg.value(QStringLiteral("arid")).toInteger());
        const bool ok = msg.value(QStringLiteral("ok")).toBool();
        auto it = m_pendingAllows.find(arid);
        if (it == m_pendingAllows.end() || it->publisher != peerId)
            return; // only the publisher we asked may answer (ids are sequential)
        PendingAllow p = m_pendingAllows.take(arid);
        if (p.peer == m_node->id()) {
            // Our own files paste (the master adopted them from a peer): fetch now.
            if (arid == m_ownAllowArid)
                m_ownAllowArid = 0;
            m_filesGrantTimer.stop();
            if (ok)
                doFetchFiles();
            else
                failFilesWaiters(tr("The device with the files refused"));
        } else {
            answerUnseal(p.peer, p.rid, ok, p.key, ok ? QString() : tr("Not allowed"), p.keep);
        }
        crypto::wipe(p.key);
        return;
    }

    if (type == QLatin1String("clipTo")) {
        if (fromMaster && m_presence.cursorLeftRecently && m_presence.cursorLeftRecently(kHandOffGraceMs)) {
            const QByteArray to = msg.value(QStringLiteral("d")).toByteArray();
            if (m_walkSeq) { // the copy being listed goes too, once listed
                m_walkHandOff = to;
                m_walkHandOffOurs = false;
            }
            sendTo(to);
        }
        return;
    }
    if (type == QLatin1String("clipFrom")) {
        if (fromMaster) {
            m_walkHandOff.clear(); // the cursor came back here: nothing to hand on after the walk
            m_expectFrom = msg.value(QStringLiteral("s")).toByteArray();
            m_expectAt = nowMs();
        }
        return;
    }
    if (type != QLatin1String("clip"))
        return;

    // A device we trust shares one clipboard with us: whatever it sends is taken if newer. A guest
    // replaces our clipboard only through a hand-off the master announced (clipFrom), or as the
    // master whose cursor is on this device (a guest is never our master, see fromMaster).
    const bool trusted = !m_node->isGuest(peerId);
    const bool expectHandoff = peerId == m_expectFrom && nowMs() - m_expectAt < kExpectMs;
    if (!(trusted || (fromMaster && m_presence.cursorArrived && m_presence.cursorArrived()) || expectHandoff)) {
        qCInfo(lcClipboard) << "Ignoring a clipboard from" << peerName(peerId) << "(a guest the cursor did not bring)";
        if (!msg.value(QStringLiteral("bc")).toBool())
            noticeOnce(QStringLiteral("ignored:") + QString::fromLatin1(peerId.toHex()),
                       tr("The clipboard of %1 was not taken: it is a guest here and the shared mouse did not bring it")
                           .arg(peerName(peerId)),
                       true, 30000);
        return;
    }
    const quint64 version = quint64(msg.value(QStringLiteral("v")).toInteger());
    if (qint64(version) > wallNow() + kMaxClockAheadMs) {
        qCWarning(lcClipboard) << "Ignoring a clipboard from" << peerName(peerId) << "stamped far in the future";
        noticeOnce(QStringLiteral("clock:") + QString::fromLatin1(peerId.toHex()),
                   tr("The clipboard of %1 was not taken: its date and time are wrong").arg(peerName(peerId)), true,
                   60000);
        return;
    }
    noteStamp(version);
    if (peerId == m_expectFrom)
        m_expectFrom.clear(); // one clipboard per hand-off
    onClip(peerId, msg);
}

void ClipboardSync::onClip(const QByteArray &peerId, const QCborMap &msg)
{
    const Stamp stamp{quint64(msg.value(QStringLiteral("v")).toInteger()), msg.value(QStringLiteral("a")).toByteArray()};
    if (!stamp.newerThan(latestStamp()) || (m_fetchHandle && !stamp.newerThan(m_fetchingStamp))) {
        if (stamp.version < m_current.stamp.version)
            qCInfo(lcClipboard) << "Ignoring an older clipboard from" << peerName(peerId) << "(stamp" << stamp.version
                                << "<" << m_current.stamp.version << ")";
        return; // otherwise the same one again (sent to every trusted device and at hand-offs)
    }

    const QByteArray kid = msg.value(QStringLiteral("kid")).toByteArray();
    if (kid.isEmpty()) {
        // An older peer sends plaintext (clip {f}); understood, and sent back to such peers only.
        onLegacyClip(peerId, msg, stamp);
        return;
    }

    Content content;
    content.stamp = stamp;
    content.kid = kid;
    const QCborMap sealedInline = msg.value(QStringLiteral("sf")).toMap();
    for (auto it = sealedInline.constBegin(); it != sealedInline.constEnd(); ++it) {
        if (kFormats.contains(it.key().toString()))
            content.sealed.insert(it.key().toString(), it.value().toByteArray());
    }
    // The key a trusted sender sent along: checked once every format is here (adopt).
    QByteArray key = msg.value(QStringLiteral("k")).toByteArray();
    if (!key.isEmpty() && key.size() != crypto::kKeySize)
        key.clear();
    content.key = key;

    // Only the formats we would keep count, each ciphertext at most kMaxFormatBytes + overhead.
    const qint64 kMaxSealed = kMaxFormatBytes + crypto::kSealOverhead;
    const qint64 kBlobCap = qint64(kFormats.size()) * kMaxSealed;
    qint64 bigBytes = 0;
    const QCborMap big = msg.value(QStringLiteral("big")).toMap();
    for (auto it = big.constBegin(); it != big.constEnd(); ++it) {
        if (!kFormats.contains(it.key().toString()))
            continue;
        bigBytes += std::clamp<qint64>(it.value().toInteger(), 0, kMaxSealed);
    }
    bigBytes = std::min(bigBytes, kBlobCap);
    const qint64 fileBytes = msg.value(QStringLiteral("fb")).toInteger();
    const qint64 fileCount = msg.value(QStringLiteral("fc")).toInteger();
    const qint64 dirCount = msg.value(QStringLiteral("dc")).toInteger();
    const quint64 offer = quint64(msg.value(QStringLiteral("o")).toInteger());
    const quint64 fileOffer = quint64(msg.value(QStringLiteral("fo")).toInteger());

    // Copied files and folders are only announced: they are transferred when pasted. Folders are
    // counted apart, against the same bound as the files. A peer of another transfer protocol
    // cannot send them (nor the large formats): what came inline is applied, the rest is left
    // out, and the user is told which device to update.
    bool anyFiles = fileCount > 0 || dirCount > 0;
    if (!transfersPossible(peerId, anyFiles && fileOffer, bigBytes > 0 && offer)) {
        anyFiles = false;
        bigBytes = 0;
    }
    const bool lazyFiles = anyFiles && fileCount >= 0 && dirCount >= 0 && fileBytes >= 0 && fileBytes <= kMaxFileBytes
                           && fileCount <= kMaxFiles && dirCount <= kMaxFiles;
    if (anyFiles && !lazyFiles)
        emit filesNotice(tr("Files copied on %1 are too many or too large to share (more than %2 files or %3)")
                             .arg(peerName(peerId))
                             .arg(kMaxFiles)
                             .arg(QLocale().formattedDataSize(kMaxFileBytes)),
                         true);
    if (lazyFiles && fileOffer) {
        QList<ListedFile> listed;
        if (parseListing(msg, fileCount, dirCount, &listed))
            content.fileList = listed;
        content.filesFrom = peerId;
        content.remoteFileOffer = fileOffer;
        content.filesAllowed = msg.value(QStringLiteral("fa")).toBool();
        content.shortPaths = msg.value(QStringLiteral("sp")).toBool(); // checked again when listed
        // A listed copy is checked against the bounds of a streaming paste right away (sp or not:
        // the paths are here), so that it is applied as streamable only when it is.
        if (!content.fileList.isEmpty()) {
            const QString violation = listViolation(content.fileList, -1, -1);
            if (!violation.isEmpty()) {
                qCInfo(lcClipboard) << "The copied files will be pasted once transferred:" << violation;
                content.listRefused = true;
            }
        }
        content.pendingFiles = int(fileCount);
        content.pendingDirs = int(dirCount);
        content.pendingBytes = std::min(fileBytes, kMaxFileBytes);
    }
    if (bigBytes == 0 || !offer) {
        adopt(std::move(content), peerId);
        return;
    }

    // Large sealed formats: fetch the ciphertext over TCP (we were let in when it was sent).
    if (m_fetchHandle) {
        const quint64 previous = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{};
        m_transfer->cancel(previous);
    }
    const QString clipboardDir = QDir(m_downloads()).filePath(QStringLiteral("Clipboard"));
    const QString target = QDir(clipboardDir).filePath(QString::number(stamp.version));
    m_fetchingStamp = stamp;
    qCInfo(lcClipboard) << "Fetching sealed clipboard formats:" << bigBytes << "bytes";
    const quint64 handle = m_transfer->fetch(
        peerId, offer, target, false, kBlobCap,
        [this, content, peerId, clipboardDir, stamp](const FetchResult &result) mutable {
            if (!(m_fetchingStamp.version == stamp.version && m_fetchingStamp.author == stamp.author))
                return;
            m_fetchHandle = 0;
            m_fetchingStamp = Stamp{};
            if (result.ok) {
                for (auto it = result.blobs.constBegin(); it != result.blobs.constEnd(); ++it) {
                    if (kFormats.contains(it.key()))
                        content.sealed.insert(it.key(), it.value()); // ciphertext, not decrypted
                }
            } else {
                qCWarning(lcClipboard) << "Could not fetch sealed clipboard formats:" << result.error;
                noticeOnce(QStringLiteral("formatsFetch"),
                           tr("Part of the clipboard of %1 could not be received: %2").arg(peerName(peerId), result.error),
                           true);
            }
            if (!stamp.newerThan(latestStamp()))
                return;
            adopt(std::move(content), peerId);
            pruneLater(clipboardDir);
        });
    if (m_fetchingStamp.version == stamp.version)
        m_fetchHandle = handle;
}

void ClipboardSync::onLegacyClip(const QByteArray &peerId, const QCborMap &msg, const Stamp &stamp)
{
    // Accept what an older peer sends: plaintext inline (f), larger formats to fetch (big) and
    // copied files announced (fc, fb, fl), both in one offer open to us (o). The plaintext is
    // re-sealed locally so it is pasteable here and sealed if it is handed on; the files stay on
    // the old peer until pasted, as usual.
    Content content;
    content.stamp = stamp;
    const QCborMap inlineFormats = msg.value(QStringLiteral("f")).toMap();
    ClipboardData plain;
    for (auto it = inlineFormats.constBegin(); it != inlineFormats.constEnd(); ++it)
        plain.insert(it.key().toString(), it.value().toByteArray());
    plain = filtered(plain);

    const qint64 kBlobCap = qint64(kFormats.size()) * kMaxFormatBytes;
    qint64 bigBytes = 0;
    const QCborMap big = msg.value(QStringLiteral("big")).toMap();
    for (auto it = big.constBegin(); it != big.constEnd(); ++it) {
        if (kFormats.contains(it.key().toString()))
            bigBytes += std::clamp<qint64>(it.value().toInteger(), 0, kMaxFormatBytes);
    }
    bigBytes = std::min(bigBytes, kBlobCap);
    const qint64 fileBytes = msg.value(QStringLiteral("fb")).toInteger();
    qint64 fileCount = msg.value(QStringLiteral("fc")).toInteger();
    const quint64 offer = quint64(msg.value(QStringLiteral("o")).toInteger());
    // As in onClip: an older peer's transfers use another protocol.
    if (!transfersPossible(peerId, fileCount > 0 && offer, bigBytes > 0 && offer)) {
        fileCount = 0;
        bigBytes = 0;
    }

    const bool lazyFiles = fileCount > 0 && fileBytes >= 0 && fileBytes <= kMaxFileBytes && fileCount <= kMaxFiles;
    if (fileCount > 0 && !lazyFiles)
        emit filesNotice(tr("Files copied on %1 are too many or too large to share (more than %2 files or %3)")
                             .arg(peerName(peerId))
                             .arg(kMaxFiles)
                             .arg(QLocale().formattedDataSize(kMaxFileBytes)),
                         true);
    if (lazyFiles && offer) {
        QList<ListedFile> listed;
        if (parseListing(msg, fileCount, 0, &listed)) // an older peer lists no folders
            content.fileList = listed;
        content.filesFrom = peerId;
        content.remoteFileOffer = offer;
        content.filesAllowed = true; // an older peer's offer is open to the devices it sends to
        content.pendingFiles = int(fileCount);
        // The one offer serves the blobs with the files: the paste's cap allows for both.
        content.pendingBytes = std::min(fileBytes, kMaxFileBytes) + bigBytes;
    }
    if (bigBytes == 0 || !offer) {
        adoptLegacy(std::move(content), std::move(plain), peerId);
        return;
    }

    // Large plaintext formats: fetched over TCP from the old peer (its offer is open to us).
    if (m_fetchHandle) {
        const quint64 previous = std::exchange(m_fetchHandle, 0);
        m_fetchingStamp = Stamp{};
        m_transfer->cancel(previous);
    }
    const QString clipboardDir = QDir(m_downloads()).filePath(QStringLiteral("Clipboard"));
    const QString target = QDir(clipboardDir).filePath(QString::number(stamp.version));
    m_fetchingStamp = stamp;
    qCInfo(lcClipboard) << "Fetching clipboard formats from an older peer:" << bigBytes << "bytes";
    const quint64 handle = m_transfer->fetch(
        peerId, offer, target, false, kBlobCap,
        [this, content, plain, peerId, clipboardDir, stamp](const FetchResult &result) mutable {
            if (!(m_fetchingStamp.version == stamp.version && m_fetchingStamp.author == stamp.author))
                return;
            m_fetchHandle = 0;
            m_fetchingStamp = Stamp{};
            if (result.ok) {
                for (auto it = result.blobs.constBegin(); it != result.blobs.constEnd(); ++it)
                    plain.insert(it.key(), it.value());
                plain = filtered(plain);
            } else {
                qCWarning(lcClipboard) << "Could not fetch clipboard formats:" << result.error;
                noticeOnce(QStringLiteral("formatsFetch"),
                           tr("Part of the clipboard of %1 could not be received: %2").arg(peerName(peerId), result.error),
                           true);
            }
            if (!stamp.newerThan(latestStamp()))
                return;
            adoptLegacy(std::move(content), std::move(plain), peerId);
            pruneLater(clipboardDir);
        });
    if (m_fetchingStamp.version == stamp.version)
        m_fetchHandle = handle;
}

void ClipboardSync::adoptLegacy(Content content, ClipboardData plain, const QByteArray &from)
{
    if (!plain.isEmpty()) {
        content.formats = std::move(plain);
        sealContent(&content);
        content.sealedHere = true; // escrowed under our identity, as for our own copies
        for (auto it = content.formats.begin(); it != content.formats.end(); ++it)
            crypto::wipe(it.value());
        content.formats.clear(); // treat like adopted content from here on
    }
    adopt(std::move(content), from);
}

void ClipboardSync::adopt(Content content, const QByteArray &from)
{
    if (content.sealed.isEmpty() && content.files.isEmpty() && content.pendingFiles + content.pendingDirs == 0)
        return;
    if (!content.sealedHere && !content.key.isEmpty() && !keyOpens(content, content.key)) {
        // A wrong key would make every paste fail; without one the master is asked.
        qCWarning(lcClipboard) << "The key sent by" << peerName(from) << "does not open its clipboard";
        crypto::wipe(content.key);
        content.key.clear();
    }
    if (content.key.isEmpty() && isMaster())
        content.key = keyFor(content.kid); // the master holds every key escrowed with it
    cancelWalk(); // newer than the copy being listed (latestStamp), and about to replace it here
    publish(&content); // so devices paired only through us can fetch the ciphertext from us
    const int pendingFiles = content.pendingFiles;
    const int pendingDirs = content.pendingDirs;
    replaceCurrent(std::move(content));
    escrowCurrentKey(); // re-sealed legacy plaintext: we hold the key, the master gets it
    if (pendingFiles > 0)
        emit filesNotice(tr("%n file(s) copied on %1: paste to transfer them", nullptr, pendingFiles)
                             .arg(peerName(from)),
                         false);
    else if (pendingDirs > 0)
        emit filesNotice(tr("%n folder(s) copied on %1: paste to transfer them", nullptr, pendingDirs)
                             .arg(peerName(from)),
                         false);
    emitApply();
}

void ClipboardSync::emitApply(bool keepEvidence)
{
    QVariantMap formats;
    // The formats present and their plaintext sizes (the ciphertext size when the size is unknown,
    // e.g. a format fetched from another device): enough for the owner to enumerate them.
    for (auto it = m_current.sealed.constBegin(); it != m_current.sealed.constEnd(); ++it) {
        const qint64 size = m_current.plainSizes.value(it.key(), it.value().size());
        formats.insert(it.key(), qlonglong(size));
    }
    m_applied = true;
    m_appliedGated = needsPasteEvidence();
    const bool same = m_appliedAt >= 0 && m_appliedStamp.version == m_current.stamp.version
        && m_appliedStamp.author == m_current.stamp.author;
    if (!same || !keepEvidence) {
        // A new object on the local clipboard (the same clipboard applied again is one too:
        // clipboard readers and monitors read it again): paste evidence counts from now on, so a
        // press made before it does not let such a read move the files. Tracked by stamp, as the
        // files of an older peer's clipboard have no kid.
        m_appliedStamp = m_current.stamp;
        m_appliedAt = nowMs();
    }
    emit applyLocally(m_current.kid, formats, m_current.files, pendingItems(), m_appliedGated);
}

void ClipboardSync::refreshGate(bool keepEvidence)
{
    if (m_applied && isEnabled() && needsPasteEvidence() != m_appliedGated)
        emitApply(keepEvidence);
}

bool ClipboardSync::needsPasteEvidence() const
{
    if (m_current.stamp.version == 0)
        return false;
    const QByteArray master = this->master();
    if (master.isEmpty() || master == m_node->id())
        return false;
    // Only a master that declared this device a guest asks for evidence; any other grants at once.
    const std::optional<TrustLevel> declared = m_node->declaredTrust(master);
    if (!declared || *declared != TrustLevel::Guest)
        return false;
    const bool formatsLocked = !m_current.sealed.isEmpty() && m_current.key.isEmpty();
    const bool filesLocked = pendingItems() > 0 && !m_current.filesAllowed;
    return formatsLocked || filesLocked;
}

// --- Escrow and the paste grant (master side) --------------------------------------------------

void ClipboardSync::escrowKey(const QByteArray &kid, const QByteArray &key, const QByteArray &author,
                              const Stamp &stamp)
{
    // Keep only the newest kid per author (a new copy makes the old key useless), capped overall.
    for (auto it = m_escrow.begin(); it != m_escrow.end();) {
        if (it->author == author && it.key() != kid) {
            crypto::wipe(it->key);
            m_escrowOrder.removeAll(it.key());
            it = m_escrow.erase(it);
        } else {
            ++it;
        }
    }
    if (!m_escrow.contains(kid))
        m_escrowOrder.append(kid);
    m_escrow.insert(kid, Escrow{key, author, stamp});
    while (m_escrowOrder.size() > kEscrowKids) {
        // A guest's key goes first (the oldest one): copying on a guest must not push the keys of
        // trusted devices out of the escrow.
        qsizetype victim = 0;
        for (qsizetype i = 0; i < m_escrowOrder.size(); ++i) {
            if (m_node->isGuest(m_escrow.value(m_escrowOrder.at(i)).author)) {
                victim = i;
                break;
            }
        }
        const QByteArray old = m_escrowOrder.takeAt(victim);
        if (auto it = m_escrow.find(old); it != m_escrow.end()) {
            crypto::wipe(it->key);
            m_escrow.erase(it);
        }
    }
}

QByteArray ClipboardSync::keyFor(const QByteArray &kid) const
{
    if (kid.isEmpty())
        return {};
    if (auto it = m_escrow.constFind(kid); it != m_escrow.constEnd())
        return it->key;
    return kid == m_current.kid ? m_current.key : QByteArray();
}

void ClipboardSync::noteHeld(const QByteArray &device, const QByteArray &kid, const Stamp &stamp)
{
    auto it = m_holds.find(device);
    if (it != m_holds.end() && !stamp.newerThan(it->stamp))
        return; // it keeps the newer clipboard it has (as adopt does)
    m_holds.insert(device, Held{kid, stamp});
}

void ClipboardSync::expireHeldUnseals()
{
    const qint64 now = nowMs();
    for (auto it = m_heldUnseals.begin(); it != m_heldUnseals.end();) {
        if (now - it->at >= kKeyWaitMs) {
            answerUnseal(it->peer, quint64(it->msg.value(QStringLiteral("rid")).toInteger()), false, {},
                         tr("Copy again"));
            it = m_heldUnseals.erase(it);
        } else {
            ++it;
        }
    }
}

void ClipboardSync::handleUnsealRequest(const QByteArray &peer, const QCborMap &msg)
{
    const QByteArray kid = msg.value(QStringLiteral("kid")).toByteArray();
    const quint64 rid = quint64(msg.value(QStringLiteral("rid")).toInteger());
    const bool files = msg.value(QStringLiteral("files")).toBool();
    if (rid == 0)
        return; // nobody could match the answer
    const QByteArray key = keyFor(kid);
    if (key.isEmpty()) {
        // The author's key may still be on its way: hold the request briefly rather than refuse,
        // a bounded number of them (a peer asking for keys nobody sent must not grow the list).
        expireHeldUnseals();
        if (m_heldUnseals.size() >= kMaxHeldUnseals) {
            answerUnseal(peer, rid, false, {}, tr("Copy again"));
            return;
        }
        m_heldUnseals.append(HeldUnseal{peer, msg, nowMs()});
        QTimer::singleShot(kKeyWaitMs + 20, this, &ClipboardSync::expireHeldUnseals);
        return;
    }
    // A trusted device is granted at once, and keeps the key (its paste just decrypts); a guest
    // needs the paste chord, every time once the latch is over.
    const bool guest = m_node->isGuest(peer);
    if (guest) {
        if (!guestMayUnseal(kid, peer)) {
            answerUnseal(peer, rid, false, {}, tr("No paste on that device"));
            return;
        }
        grantLatched(kid, peer);
    }
    const bool keep = !guest;
    if (!files) {
        answerUnseal(peer, rid, true, key, {}, keep);
        return;
    }
    // A files paste: let the publisher serve them to this device, then hand over the key.
    const quint64 offer = quint64(msg.value(QStringLiteral("o")).toInteger());
    const QByteArray publisher = msg.value(QStringLiteral("p")).toByteArray();
    if (publisher == m_node->id()) {
        // Our own files: the requester does not choose the offer, only our current clipboard's
        // copied files open, and only for a paste of that clipboard.
        if (kid != m_current.kid || !m_current.fileOffer) {
            answerUnseal(peer, rid, false, {}, tr("The clipboard changed"));
            return;
        }
        m_transfer->allowPeer(m_current.fileOffer, peer, true);
        answerUnseal(peer, rid, true, key, {}, keep);
        return;
    }
    if (publisher.isEmpty() || !m_node->isConnected(publisher)) {
        answerUnseal(peer, rid, false, {}, tr("The device with the files is not reachable"));
        return;
    }
    if (peerKnownOlder(publisher)) {
        // An older peer's offer is open to the devices it sent it to and it knows no clip.allow:
        // the grant is the key alone.
        answerUnseal(peer, rid, true, key, {}, keep);
        return;
    }
    const quint64 arid = m_nextArid++;
    m_pendingAllows.insert(arid, PendingAllow{peer, rid, key, publisher, keep});
    QCborMap m;
    m.insert(kT, QStringLiteral("clip.allow"));
    m.insert(QStringLiteral("arid"), qint64(arid));
    m.insert(QStringLiteral("kid"), kid);
    m.insert(QStringLiteral("o"), qint64(offer));
    m.insert(QStringLiteral("d"), peer);
    m_node->sendMessage(publisher, m);
    QTimer::singleShot(kUnsealTimeoutMs, this, [this, arid] {
        if (!m_pendingAllows.contains(arid))
            return;
        PendingAllow p = m_pendingAllows.take(arid);
        answerUnseal(p.peer, p.rid, false, {}, tr("The device with the files did not answer"));
        crypto::wipe(p.key);
    });
}

bool ClipboardSync::guestMayUnseal(const QByteArray &kid, const QByteArray &guest)
{
    const QByteArray latch = kid + guest;
    if (m_grantUntil.value(latch, -1) > nowMs())
        return true; // latched by a first grant (applications ask several times per paste)
    // Only the clipboard the guest holds (see noteHeld): one it relayed earlier may not be asked
    // for on the strength of a paste of this one. The author's key and the hand-off travel at
    // once, so the clipboard the device the cursor came from holds, or authored, counts as held.
    if (m_holds.value(guest).kid != kid) {
        const QByteArray from = m_lastFrom.value(guest);
        const auto escrow = m_escrow.constFind(kid);
        const bool fromHolds = !from.isEmpty()
                               && (from == m_node->id()
                                       ? m_current.kid == kid
                                       : m_holds.value(from).kid == kid
                                             || (escrow != m_escrow.constEnd() && escrow->author == from));
        if (!fromHolds)
            return false;
        if (escrow != m_escrow.constEnd())
            m_holds.insert(guest, Held{kid, escrow->stamp});
    }
    // An explicit paste chord; a context-menu paste cannot grant it.
    return m_presence.pasteOrderedOn && m_presence.pasteOrderedOn(guest, kChordWindowMs);
}

void ClipboardSync::grantLatched(const QByteArray &kid, const QByteArray &requester)
{
    const qint64 now = nowMs();
    for (auto it = m_grantUntil.begin(); it != m_grantUntil.end();) { // expired latches go
        if (it.value() <= now)
            it = m_grantUntil.erase(it);
        else
            ++it;
    }
    m_grantUntil.insert(kid + requester, now + kGrantLatchMs);
}

void ClipboardSync::answerUnseal(const QByteArray &peer, quint64 rid, bool ok, const QByteArray &key,
                                 const QString &reason, bool keep)
{
    if (peer == m_node->id())
        return; // the master's own render does not go over the wire
    QCborMap m;
    m.insert(kT, QStringLiteral("clip.unsealed"));
    m.insert(QStringLiteral("rid"), qint64(rid));
    m.insert(QStringLiteral("ok"), ok);
    if (ok) {
        m.insert(QStringLiteral("k"), key);
        if (keep)
            m.insert(QStringLiteral("keep"), true);
    } else {
        m.insert(QStringLiteral("r"), reason);
    }
    m_node->sendMessage(peer, m);
}

// --- Unsealing for a paste (any device) --------------------------------------------------------

void ClipboardSync::unseal(const QByteArray &kid, const QString &mime, UnsealCallback done)
{
    if (kid != m_current.kid || kid.isEmpty()) {
        // A stale clipboard object renders nothing (e.g. one put there before the service
        // restarted): the user is told to copy again rather than getting an empty paste.
        noticeOnce(QStringLiteral("stale"), tr("This clipboard is no longer available: copy it again"), true);
        done(false, {}, tr("The clipboard changed"));
        return;
    }
    if (!m_current.sealed.contains(mime)) {
        failPaste(done, tr("No such clipboard format"));
        return;
    }
    if (m_current.key.isEmpty() && isMaster())
        m_current.key = keyFor(kid); // escrowed with us after the content arrived
    if (!m_current.key.isEmpty()) {
        // We hold the key (our own copy, or a trusted device's): the paste just decrypts. The
        // plaintext stays cached until the clipboard changes (applications ask many times).
        applyKey(m_current.key);
        if (m_current.plainCache.contains(mime))
            done(true, m_current.plainCache.value(mime), {});
        else
            failPaste(done, tr("The clipboard content could not be read"));
        return;
    }
    if (isMaster()) {
        // The master holds every key it was sent: without one, nobody escrowed it.
        noticeOnce(QStringLiteral("noKey"), tr("This clipboard cannot be pasted here: copy it again"), true);
        done(false, {}, tr("Copy again"));
        return;
    }
    // A guest's granted plaintext serves only while the grant latches; after that it is wiped,
    // so a clipboard manager polling later gets nothing and the next paste needs a fresh grant.
    if (!m_current.plainCache.isEmpty()) {
        if (m_current.grantedAt >= 0 && nowMs() - m_current.grantedAt < kGrantLatchMs) {
            if (m_current.plainCache.contains(mime))
                done(true, m_current.plainCache.value(mime), {});
            else
                failPaste(done, tr("The clipboard content could not be read"));
            return;
        }
        for (auto it = m_current.plainCache.begin(); it != m_current.plainCache.end(); ++it)
            crypto::wipe(it.value());
        m_current.plainCache.clear();
        m_current.grantedAt = -1;
    }
    // Ask the master for the key. A guest asks only if the user did something here just now, so
    // a clipboard manager reading on arrival does not even cause a request. A request already in
    // flight serves later formats too (one grant, one key).
    const bool sawInput = m_presence.inputArrived && m_presence.inputArrived(kInputWindowMs);
    if (!m_formatRid && needsPasteEvidence() && !sawInput) {
        done(false, {}, tr("Not allowed"));
        return;
    }
    m_formatWaiters.append(FormatWaiter{mime, std::move(done)});
    requestUnseal();
}

void ClipboardSync::requestUnseal()
{
    if (m_formatRid)
        return; // one request serves every format of this content
    const QByteArray master = this->master();
    if (master.isEmpty() || master == m_node->id() || !m_node->isConnected(master) || m_node->isGuest(master)) {
        failFormatWaiters(tr("The device holding the clipboard is not reachable"));
        return;
    }
    if (peerKnownOlder(master)) {
        // It would ignore the request: fail at once rather than block the pasting application.
        failFormatWaiters(tr("Update DeskFlower on %1 to paste the shared clipboard").arg(peerName(master)));
        return;
    }
    m_formatRid = m_nextRid++;
    QCborMap m;
    m.insert(kT, QStringLiteral("clip.unseal"));
    m.insert(QStringLiteral("rid"), qint64(m_formatRid));
    m.insert(QStringLiteral("kid"), m_current.kid);
    m.insert(QStringLiteral("files"), false);
    if (!m_node->sendMessage(master, m)) {
        failFormatWaiters(tr("The device holding the clipboard is not reachable"));
        return;
    }
    m_formatTimer.start(kUnsealTimeoutMs);
}

void ClipboardSync::requestFilesGrant()
{
    const QByteArray master = this->master();
    if (master.isEmpty() || master == m_node->id() || !m_node->isConnected(master) || m_node->isGuest(master)) {
        failFilesWaiters(tr("The device holding the clipboard is not reachable"));
        return;
    }
    if (peerKnownOlder(master)) {
        failFilesWaiters(tr("Update DeskFlower on %1 to paste the shared clipboard").arg(peerName(master)));
        return;
    }
    m_filesRid = m_nextRid++;
    QCborMap m;
    m.insert(kT, QStringLiteral("clip.unseal"));
    m.insert(QStringLiteral("rid"), qint64(m_filesRid));
    m.insert(QStringLiteral("kid"), m_current.kid);
    m.insert(QStringLiteral("files"), true);
    m.insert(QStringLiteral("o"), qint64(m_current.remoteFileOffer));
    m.insert(QStringLiteral("p"), m_current.filesFrom);
    if (!m_node->sendMessage(master, m)) {
        m_filesRid = 0;
        failFilesWaiters(tr("The device holding the clipboard is not reachable"));
        return;
    }
    m_filesGrantTimer.start(kUnsealTimeoutMs);
}

void ClipboardSync::applyKey(const QByteArray &key)
{
    for (auto it = m_current.sealed.constBegin(); it != m_current.sealed.constEnd(); ++it) {
        if (m_current.plainCache.contains(it.key()))
            continue;
        if (auto plain = crypto::unseal(key, sealAd(m_current.kid, it.key()), it.value()))
            m_current.plainCache.insert(it.key(), *plain);
    }
    if (m_current.grantedAt < 0)
        m_current.grantedAt = nowMs();
}

void ClipboardSync::onUnsealed(const QByteArray &key, bool keep)
{
    // A trusted device keeps the key: its next pastes (and the hand-on to other trusted devices)
    // need no round trip. A guest's plaintext lives only as long as the latch.
    if (keep && m_current.key.isEmpty() && keyOpens(m_current, key))
        m_current.key = key;
    applyKey(key);
    serveFormatWaiters();
    // Holding the key now, reads need no grant any more. A key asked for by a paste under way
    // (one with paste evidence) does not cost that paste its evidence for the files it brings.
    refreshGate(hasPasteEvidence(nowMs()));
}

void ClipboardSync::serveFormatWaiters()
{
    const QList<FormatWaiter> waiters = std::exchange(m_formatWaiters, {});
    for (const FormatWaiter &w : waiters) {
        if (!w.done)
            continue;
        if (m_current.plainCache.contains(w.mime))
            w.done(true, m_current.plainCache.value(w.mime), {});
        else
            failPaste(w.done, tr("The clipboard content could not be read"));
    }
}

void ClipboardSync::failPaste(const UnsealCallback &done, const QString &error)
{
    qCInfo(lcClipboard) << "Paste failed:" << error;
    noticeOnce(QStringLiteral("paste"), tr("The clipboard could not be pasted: %1").arg(error), true);
    done(false, {}, error);
}

bool ClipboardSync::keyOpens(const Content &content, const QByteArray &key) const
{
    if (key.size() != crypto::kKeySize)
        return false;
    for (auto it = content.sealed.constBegin(); it != content.sealed.constEnd(); ++it) {
        auto plain = crypto::unseal(key, sealAd(content.kid, it.key()), it.value());
        if (!plain)
            return false;
        crypto::wipe(*plain);
    }
    return true;
}

void ClipboardSync::failFormatWaiters(const QString &error, bool tell)
{
    m_formatTimer.stop();
    m_formatRid = 0;
    const QList<FormatWaiter> waiters = std::exchange(m_formatWaiters, {});
    if (tell && !waiters.isEmpty()) {
        qCInfo(lcClipboard) << "Paste refused:" << error;
        noticeOnce(QStringLiteral("paste"), tr("The clipboard could not be pasted: %1").arg(error), true);
    }
    for (const FormatWaiter &w : waiters)
        if (w.done)
            w.done(false, {}, error);
}

void ClipboardSync::failFilesWaiters(const QString &error, bool tell, FetchError code)
{
    m_evidenceTimer.stop();
    if (m_job && m_job->handle) { // transferring already: that transfer ends
        cancelFilesJob(m_job, code);
        return;
    }
    const std::shared_ptr<FilesJob> job = std::exchange(m_job, {});
    if (!job)
        return;
    job->done = true;
    const QMap<quint64, Waiter> waiters = std::exchange(job->waiters, {});
    if (tell && !waiters.isEmpty()) {
        qCInfo(lcClipboard) << "Files paste refused:" << error;
        noticeOnce(QStringLiteral("pasteFiles"), tr("The copied files could not be pasted: %1").arg(error), true);
    }
    for (const Waiter &w : waiters)
        w.done(false, {}, error, code);
}

// --- Copied files (a paste) --------------------------------------------------------------------

quint64 ClipboardSync::fetchFiles(FilesCallback done, const QByteArray &kid, StartedCallback started, bool streaming,
                                  qint64 listedAgoMs, bool fresh)
{
    if (!kid.isEmpty() && kid != m_current.kid) {
        // A stale clipboard object (an application kept it, or the service restarted): nothing
        // moves. A paste (a press just before) tells the user to copy again rather than getting
        // an empty paste; a reader that is not one is refused quietly.
        const qint64 gesture = m_presence.lastPasteGesture ? m_presence.lastPasteGesture() : -1;
        if (gesture >= 0 && gesture >= nowMs() - kChordWindowMs)
            noticeOnce(QStringLiteral("stale"), tr("This clipboard is no longer available: copy it again"), true);
        else
            qCInfo(lcClipboard) << "A stale clipboard object was read: nothing transferred";
        done(false, {}, tr("The clipboard changed"), FetchError::Changed);
        return 0;
    }
    if (fresh && pendingItems() == 0) {
        if (m_current.filesFrom.isEmpty() || m_current.stagedRoot.isEmpty()) {
            // Only another device's files transferred for this paste are given away to be moved:
            // never files that are here already (copied here, or written elsewhere), which a move
            // would take from where the user keeps them.
            qCWarning(lcClipboard) << "A move paste of files that were not transferred for it: refused";
            done(false, {}, tr("These files cannot be moved"), FetchError::Other);
            return 0;
        }
        // A move paste takes what it is given away: the copy transferred before is not handed out
        // again (an earlier move paste may have taken it, or may still be taking it). A new
        // transfer, into a new folder, as for the first paste. Put back as it was if the request
        // is then refused for want of a paste (refuseNoPaste): nothing was transferred.
        qCInfo(lcClipboard) << "A move paste of files transferred before: transferring them again";
        m_moveUnstaged = MoveUnstaged{m_current.kid, m_current.files, m_current.stagedRoot};
        unstage();
    }
    if (pendingItems() == 0) {
        // Already here (copied here, or transferred by an earlier paste): nothing moves, so no
        // paste is needed. A streaming paste of files transferred here reads them where they were
        // written, pinned there for it.
        if (m_current.files.isEmpty()) {
            done(false, {}, tr("There are no copied files"), FetchError::Other);
            return 0;
        }
        quint64 id = 0;
        if (streaming && !m_current.stagedRoot.isEmpty()) {
            id = m_nextWaiter++;
            startWaiter(id, m_current.stagedRoot, started);
        }
        done(true, m_current.files, {}, FetchError::None);
        return id;
    }
    const quint64 id = m_nextWaiter++;
    if (m_job) {
        // On its way already: waiting for a gesture, a grant, the allow round trip or the
        // transfer. Joined; a streaming waiter is given the transfer's folder once there is one.
        m_job->waiters.insert(id, Waiter{std::move(done), started, streaming});
        m_job->moveWaiter = m_job->moveWaiter || fresh;
        if (streaming && m_job->handle)
            startWaiter(id, m_job->target, started);
        return id;
    }
    m_job = std::make_shared<FilesJob>();
    m_job->stamp = m_current.stamp;
    m_job->kid = m_current.kid;
    m_job->peer = m_current.filesFrom;
    m_job->waiters.insert(id, Waiter{std::move(done), std::move(started), streaming});
    m_job->moveWaiter = fresh;
    const qint64 now = nowMs();
    m_evidenceRef = evidenceReference(now);
    m_descriptorRef = listedAgoMs >= 0 && listedAgoMs <= kDescriptorReferenceMs ? now - listedAgoMs : -1;
    if (jobHasPasteEvidence()) {
        proceedFetchFiles();
        return hasFilesWaiter(id) ? id : 0;
    }
    // Refused for want of a paste a moment ago, and nothing pressed since: the same reader asking
    // again (one request per item or per format) is refused at once rather than after another
    // grace each time.
    const qint64 gesture = m_presence.lastPasteGesture ? m_presence.lastPasteGesture() : -1;
    if (m_noPasteKid == m_current.kid && m_noPasteAt >= 0 && now - m_noPasteAt <= kNoPasteMemoryMs
        && gesture < m_noPasteAt) {
        refuseNoPaste();
        return 0;
    }
    // No gesture yet: the press of this very paste may still be on its way (the helper reports it
    // on a connection of its own). A moment, then refused.
    m_evidenceTimer.start(kEvidenceGraceMs);
    return id;
}

bool ClipboardSync::hasFilesWaiter(quint64 id) const
{
    if (m_pins.contains(id) || (m_job && m_job->waiters.contains(id)))
        return true;
    return std::any_of(m_detachedJobs.cbegin(), m_detachedJobs.cend(),
                       [id](const std::shared_ptr<FilesJob> &job) { return job->waiters.contains(id); });
}

void ClipboardSync::startWaiter(quint64 id, const QString &root, const StartedCallback &started)
{
    m_pins.insert(id, root);
    if (started)
        started(root);
}

void ClipboardSync::dropFilesWaiter(quint64 id)
{
    m_pins.remove(id);
    std::vector<std::shared_ptr<FilesJob>> jobs = m_detachedJobs;
    if (m_job)
        jobs.push_back(m_job);
    for (const auto &job : jobs) {
        auto it = job->waiters.find(id);
        if (it == job->waiters.end())
            continue;
        const bool streaming = it->streaming;
        job->waiters.erase(it);
        if (job != m_job) {
            // A detached transfer serves only the streaming pastes reading it (its plain waiters
            // were answered when it was detached): with none left it is of no use.
            const bool read = std::any_of(job->waiters.cbegin(), job->waiters.cend(),
                                          [](const Waiter &w) { return w.streaming; });
            if (!read) {
                qCInfo(lcClipboard) << "Nobody reads the transfer of a replaced clipboard any more: cancelled";
                cancelFilesJob(job, FetchError::Cancelled);
            }
            return;
        }
        if (!job->waiters.isEmpty())
            return;
        if (job->handle) {
            // The paste that read it while it arrives went away (Explorer cancelled, or gave up):
            // nobody reads it. A plain request that goes leaves the files to the next paste.
            if (streaming) {
                qCInfo(lcClipboard) << "The paste reading the copied files went away: transfer cancelled";
                cancelFilesJob(job, FetchError::Cancelled);
            }
        } else {
            // Not transferring yet (waiting for a gesture or a grant): nobody to give it to.
            m_evidenceTimer.stop();
            m_filesGrantTimer.stop();
            m_filesRid = 0;
            if (m_ownAllowArid)
                m_pendingAllows.remove(std::exchange(m_ownAllowArid, 0));
            job->done = true;
            m_job.reset();
        }
        return;
    }
}

void ClipboardSync::unstage()
{
    if (m_current.filesFrom.isEmpty() || m_current.stagedRoot.isEmpty())
        return;
    m_current.pendingFiles = m_current.stagedFiles;
    m_current.pendingDirs = m_current.stagedDirs;
    m_current.stagedFiles = 0;
    m_current.stagedDirs = 0;
    m_current.files.clear();
    m_current.stagedRoot.clear();
}

void ClipboardSync::filesConsumed(quint64 id)
{
    const QString root = m_pins.value(id);
    if (root.isEmpty()) {
        dropFilesWaiter(id); // not pinned (yet): whatever of it still waits ends here
        return;
    }
    m_pins.remove(id);
    dropFilesWaiter(id); // nothing else of it is left (its transfer ended before it was pinned for good)
    const QString clean = QDir::cleanPath(root);
    if (!m_current.stagedRoot.isEmpty() && QDir::cleanPath(m_current.stagedRoot).compare(clean, Qt::CaseInsensitive) == 0) {
        qCInfo(lcClipboard) << "A move paste took the copied files: the next paste transfers them again";
        unstage();
    }
    // What the paste left (the emptied folder, files it skipped) goes now, unless something still
    // uses it. Only one of our own folders: "<received copies>/<name>", never anything else.
    const QString clipboardDir = QDir::cleanPath(QDir(m_downloads()).filePath(QStringLiteral("Clipboard")));
    if (QFileInfo(clean).absolutePath().compare(clipboardDir, Qt::CaseInsensitive) != 0
        || QFileInfo(clean).fileName().isEmpty()) {
        qCWarning(lcClipboard) << "Not removing a consumed folder outside the received copies:" << clean;
        return;
    }
    const auto skip = pruneSkip();
    if (skip(clean))
        return; // another paste still reads it, or a transfer writes there: the prune takes it later
    m_transfer->runAsUser([clean, skip] {
        // Checked again on the worker: a transfer may have started there meanwhile.
        if (!skip(clean) && QFileInfo::exists(clean))
            fs::removeTree(clean);
    });
}

bool ClipboardSync::filesMovable() const
{
    return pendingItems() > 0 && !m_current.filesFrom.isEmpty() && m_current.remoteFileOffer
        && m_current.filesAllowed && !needsPasteEvidence();
}

QStringList ClipboardSync::pinnedTargets() const
{
    QStringList roots;
    for (const QString &root : m_pins) {
        if (!roots.contains(root))
            roots.append(root);
    }
    return roots;
}

void ClipboardSync::receiveFiles(const QByteArray &kid, FilesCallback done)
{
    if (kid.isEmpty() || kid != m_current.kid) {
        done(false, {}, tr("The clipboard changed"), FetchError::Changed);
        return;
    }
    if (pendingItems() == 0) {
        if (m_current.files.isEmpty())
            done(false, {}, tr("There are no copied files"), FetchError::Other);
        else
            done(true, m_current.files, {}, FetchError::None);
        return;
    }
    // Only files whose publisher let this device in already (a trusted device's, or an older
    // peer's open offer): a guest's paste goes through the master's grant, which needs the chord.
    if (!m_current.filesAllowed || needsPasteEvidence()) {
        done(false, {}, tr("Paste to transfer the copied files"), FetchError::Other);
        return;
    }
    qCInfo(lcClipboard) << "Receiving the copied files at the user's request";
    const quint64 id = m_nextWaiter++;
    if (!m_job) {
        m_job = std::make_shared<FilesJob>();
        m_job->stamp = m_current.stamp;
        m_job->kid = m_current.kid;
        m_job->peer = m_current.filesFrom;
    }
    m_job->waiters.insert(id, Waiter{std::move(done), {}, false});
    m_job->applyWhenDone = true;
    if (m_job->handle || m_filesRid || m_ownAllowArid)
        return; // on its way already
    m_evidenceTimer.stop(); // a request waiting for a gesture goes on with this one
    proceedFetchFiles();
}

void ClipboardSync::pasteGestureNoted()
{
    if (!m_evidenceTimer.isActive() || !m_job)
        return;
    if (jobHasPasteEvidence()) {
        m_evidenceTimer.stop();
        proceedFetchFiles();
    }
}

bool ClipboardSync::hasPasteEvidence(qint64 ref, qint64 latest) const
{
    // Only a gesture made since this clipboard's object is on the local clipboard: a press that
    // came before it (the click that brought the cursor here, say) did not paste it.
    if (m_appliedAt < 0 || m_appliedStamp.version == 0 || m_appliedStamp.version != m_current.stamp.version
        || m_appliedStamp.author != m_current.stamp.author)
        return false;
    const qint64 from = std::max(m_appliedAt, ref - kChordWindowMs);
    if (m_presence.pasteGestureBetween)
        return m_presence.pasteGestureBetween(from, latest);
    const qint64 gesture = m_presence.lastPasteGesture ? m_presence.lastPasteGesture() : -1;
    return gesture >= 0 && gesture >= from && (latest < 0 || gesture <= latest);
}

bool ClipboardSync::jobHasPasteEvidence() const
{
    if (hasPasteEvidence(m_evidenceRef))
        return true;
    // A press around the moment the reader read the list: the paste that read it, whose first
    // contents read may come long after (a file manager creates the folders first). Presses made
    // since then (a click elsewhere while the folders are created) do not hide it: the recent
    // ones are kept (Presence::pasteGestureBetween).
    return m_descriptorRef >= 0 && hasPasteEvidence(m_descriptorRef, m_descriptorRef + kChordWindowMs);
}

qint64 ClipboardSync::evidenceReference(qint64 now) const
{
    // A file manager reads the list, then (once it was given, which may take the network a while
    // for a large copy) the contents: the window then counts from when the list was asked for.
    if (!m_list.kid.isEmpty() && m_list.kid == m_current.kid && m_list.askedAt >= 0 && m_list.answeredAt >= 0
        && now - m_list.answeredAt <= kListReferenceMs)
        return std::min(m_list.askedAt, now);
    return now;
}

void ClipboardSync::refuseNoPaste()
{
    const QByteArray kid = m_current.kid;
    const bool local = m_presence.localPressSupported && m_presence.localPressSupported();
    qCInfo(lcClipboard).noquote() << "Copied files not transferred: no key or click at this device just before the"
                                  << "request" << (local ? "" : "(this device's own presses are not reported here)");
    m_noPasteKid = kid;
    m_noPasteAt = nowMs();
    failFilesWaiters(tr("Paste to transfer the copied files"), false, FetchError::NoPaste);
    if (m_moveUnstaged && m_moveUnstaged->kid == kid && kid == m_current.kid && !m_job
        && m_current.stagedRoot.isEmpty() && pendingItems() > 0) {
        // A move paste's request made the copy here "not transferred" (fetchFiles), and was not a
        // paste after all: nothing moved, so the copy is the current one again.
        m_current.files = m_moveUnstaged->files;
        m_current.stagedRoot = m_moveUnstaged->root;
        m_current.stagedFiles = m_current.pendingFiles;
        m_current.stagedDirs = m_current.pendingDirs;
        m_current.pendingFiles = 0;
        m_current.pendingDirs = 0;
        qCInfo(lcClipboard) << "The move paste's request was refused: the copy here stays the current one";
    }
    m_moveUnstaged.reset();
    if (kid == m_current.kid && pendingItems() > 0 && kid != m_waitingToldKid) {
        m_waitingToldKid = kid;
        emit filesWaiting(kid, pendingItems(), peerName(m_current.filesFrom));
        if (!local) {
            // This device's own presses are not reported (a Wayland session, macOS without the
            // event tap or with secure input on): a paste here cannot be told from a reader, and
            // the user would otherwise not know why nothing came.
            noticeOnce(QStringLiteral("noLocalPress"),
                       tr("The copied files were not transferred: this device does not report its own key presses, "
                          "so a paste here cannot be told from an application reading the clipboard"),
                       true);
        }
    }
}

void ClipboardSync::proceedFetchFiles()
{
    if (!m_job || m_job->handle || m_filesRid || m_ownAllowArid)
        return;
    m_moveUnstaged.reset(); // a paste after all: the transfer goes ahead
    if (m_current.filesAllowed) {
        // The publisher let us in when it sent the clipboard (we are trusted there, or it is an
        // older peer whose offer is open): the paste is the grant.
        doFetchFiles();
        return;
    }
    if (isMaster()) {
        // We are the master and the files are on a device that did not let us in yet: have it
        // let us in, and fetch once it answers (clip.allowed).
        if (!m_node->isConnected(m_current.filesFrom)) {
            failFilesWaiters(tr("The device with the files is not reachable"));
            return;
        }
        if (peerKnownOlder(m_current.filesFrom)) {
            doFetchFiles();
            return;
        }
        const quint64 arid = m_nextArid++;
        QCborMap m;
        m.insert(kT, QStringLiteral("clip.allow"));
        m.insert(QStringLiteral("arid"), qint64(arid));
        m.insert(QStringLiteral("kid"), m_current.kid);
        m.insert(QStringLiteral("o"), qint64(m_current.remoteFileOffer));
        m.insert(QStringLiteral("d"), m_node->id());
        if (!m_node->sendMessage(m_current.filesFrom, m)) {
            failFilesWaiters(tr("The device with the files is not reachable"));
            return;
        }
        m_pendingAllows.insert(arid, PendingAllow{m_node->id(), 0, {}, m_current.filesFrom, true});
        m_ownAllowArid = arid;
        m_filesGrantTimer.start(kUnsealTimeoutMs);
        return;
    }
    // A guest asks the master for the grant: the master decides on the paste chord it forwarded
    // here, and tells the publisher to let us in.
    requestFilesGrant();
}

void ClipboardSync::doFetchFiles()
{
    const std::shared_ptr<FilesJob> job = m_job;
    if (!job || job->handle)
        return; // nobody waits any more, or on its way
    const Stamp stamp = m_current.stamp;
    const QByteArray peer = m_current.filesFrom;
    const QString clipboardDir = QDir(m_downloads()).filePath(QStringLiteral("Clipboard"));
    // A fresh folder for every transfer: a paste again after a cancel or a failure never meets
    // what an earlier one left (or is still deleting). The counter starts at the time the service
    // started, so that a later run of the service does not name a folder an earlier run left
    // (kept copies, or one cut short by a crash): a streaming paste is told the folder before the
    // worker could remove such a leftover, and would read its files.
    static std::atomic<quint64> jobs{quint64(std::max<qint64>(QDateTime::currentMSecsSinceEpoch(), 0))};
    const QString target =
        QDir(clipboardDir).filePath(QStringLiteral("%1-%2").arg(stamp.version).arg(jobs.fetch_add(1) + 1));
    qCInfo(lcClipboard) << "Pasting" << m_current.pendingFiles << "files and" << m_current.pendingDirs << "folders,"
                        << m_current.pendingBytes << "bytes";
    if (m_current.pendingFiles > 0)
        emit filesNotice(tr("Receiving %n copied file(s) from %1…", nullptr, m_current.pendingFiles).arg(peerName(peer)),
                         false);
    else
        emit filesNotice(tr("Receiving %n copied folder(s) from %1…", nullptr, m_current.pendingDirs).arg(peerName(peer)),
                         false);
    FetchOptions options;
    options.kind = QStringLiteral("clipboard");
    // Room for this one first, on the transfer's worker as the console user, before the stream is
    // asked for: the older received copies beyond what is kept are only renamed out of the way
    // there (retireReceived: quick, whatever their size), and deleted alongside the transfer
    // (cleanupAsUser, on the pool, as the same user; a copy that does not fit waits for it before
    // its free space check refuses it). Deleting 15,000 files or 15 GB first held the transfer
    // up for seconds. A folder of the same name could only be left by an earlier run of the
    // service whose clock was ahead: it goes too (out of the way before anything is written).
    const auto retired = std::make_shared<QStringList>();
    const auto skip = pruneSkip();
    options.prepareAsUser = [clipboardDir, target, skip, retired] {
        QElapsedTimer took;
        took.start();
        *retired = retireReceived(clipboardDir, kKeptDownloads - 1, skip);
        if (QFileInfo::exists(target)) {
            const QString aside = retireFolder(target);
            if (aside.isEmpty())
                fs::removeTree(target); // could not be renamed: deleted at once, as before
            else
                retired->append(aside);
        }
        if (!retired->isEmpty())
            qCInfo(lcClipboard) << "Set" << retired->size() << "older received copies aside for deletion in"
                                << took.elapsed() << "ms";
    };
    // Written by prepareAsUser on the fetch's worker, read here afterwards (FileTransfer runs this
    // only once prepareAsUser has returned).
    options.cleanupAsUser = [retired, skip] {
        if (!retired->isEmpty())
            removeRetired(*retired, skip);
    };
    job->target = target;
    job->handle = m_transfer->fetch(
        peer, m_current.remoteFileOffer, target, true, m_current.pendingBytes,
        [this, job](const FetchResult &result) {
            if (!job->done) // done: ended from here already (or this object is going)
                onFilesFetched(job, result);
        },
        std::move(options));
    // The callback never runs inside fetch(): the streaming waiters are given the folder now. It
    // may not exist yet: the worker creates it (prepareAsUser, the free space check, then the
    // folder) and fills it as the transfer goes; the reader waits for it (see fetchFiles).
    const QList<quint64> ids = job->waiters.keys();
    for (const quint64 id : ids) {
        const auto it = job->waiters.constFind(id);
        if (it == job->waiters.cend() || !it->streaming)
            continue;
        const StartedCallback started = it->started; // the map may change in the call
        startWaiter(id, target, started);
    }
}

void ClipboardSync::onFilesFetched(const std::shared_ptr<FilesJob> &job, const FetchResult &result)
{
    job->done = true;
    job->handle = 0;
    const bool detached = job != m_job;
    if (detached)
        m_detachedJobs.erase(std::remove(m_detachedJobs.begin(), m_detachedJobs.end(), job), m_detachedJobs.end());
    else
        m_job.reset();
    const QMap<quint64, Waiter> waiters = std::exchange(job->waiters, {});
    // A detached transfer (its clipboard was replaced while a streaming paste read it) answers its
    // own waiters and never touches the current clipboard.
    const bool current = !detached && m_current.stamp.version == job->stamp.version
        && m_current.stamp.author == job->stamp.author;
    const QString from = peerName(job->peer);
    QString error;
    FetchError code = FetchError::None;
    if (job->abortCode != FetchError::None) {
        code = job->abortCode;
        error = code == FetchError::Changed ? tr("The clipboard changed meanwhile") : result.error;
    } else if (!detached && !current) {
        error = tr("The clipboard changed meanwhile");
        code = FetchError::Changed;
    } else if (result.cancelled) {
        error = result.error;
        code = FetchError::Cancelled;
    } else if (!result.ok) {
        error = result.error;
        code = FetchError::Other;
    }
    if (code != FetchError::None) {
        if (code == FetchError::Cancelled || code == FetchError::Changed) {
            // The user cancelled it, the paste reading it went away, or the clipboard (or the user)
            // changed: nothing went wrong, nothing to tell.
            qCInfo(lcClipboard) << "The transfer of the copied files ended:" << error;
        } else {
            qCWarning(lcClipboard) << "Could not fetch the copied files:" << error;
            emit filesNotice(tr("Could not get the copied files from %1: %2").arg(from, error), true);
        }
        for (const Waiter &w : waiters)
            w.done(false, {}, error, code);
        return;
    }
    job->ok = true;
    const QStringList files = normalized(result.files);
    if (current) {
        m_current.files = files;
        m_current.stagedFiles = m_current.pendingFiles;
        m_current.stagedDirs = m_current.pendingDirs;
        m_current.pendingFiles = 0;
        m_current.pendingDirs = 0;
        m_current.stagedRoot = job->target;
        endFilesWaiting();
    }
    qCInfo(lcClipboard) << "Fetched" << files.size() << "copied items" << (detached ? "(replaced clipboard)" : "");
    emit filesNotice(tr("%n item(s) received from %1", nullptr, int(files.size())).arg(from), false);
    pruneLater(QDir(m_downloads()).filePath(QStringLiteral("Clipboard")));
    // Taken with Receive: the owner puts the local files on the clipboard (an application that
    // refused to paste before sees them now). Not when a move paste waits for the same transfer:
    // it is given these very files to move away, and the clipboard object Explorer is moving
    // them from must stay (its pin keeps the folder from the prune meanwhile).
    if (current && job->applyWhenDone && !job->moveWaiter)
        emitApply();
    else if (current && job->applyWhenDone)
        qCInfo(lcClipboard) << "Received, and a move paste takes them: the clipboard is not applied again";
    for (const Waiter &w : waiters)
        w.done(true, files, {}, FetchError::None);
}

void ClipboardSync::cancelFilesJob(const std::shared_ptr<FilesJob> &job, FetchError code)
{
    if (!job || job->done || !job->handle)
        return;
    job->abortCode = code;
    m_transfer->cancel(job->handle); // its callback runs inside: onFilesFetched
}

std::function<bool(const QString &)> ClipboardSync::pruneSkip() const
{
    QSet<QString> pins;
    for (const QString &root : m_pins)
        pins.insert(QDir::cleanPath(root));
    return [active = m_transfer->activeTargetCheck(), pins](const QString &path) {
        return active(path) || pins.contains(QDir::cleanPath(path));
    };
}

// --- Listing copied files (what a paste will bring) ---------------------------------------------

void ClipboardSync::listFiles(const QByteArray &kid, ListCallback done)
{
    if (kid.isEmpty() || kid != m_current.kid) {
        done(false, {}, tr("The clipboard changed"));
        return;
    }
    // Only files copied on another device and still there: never this device's own copy (its
    // owner's clipboard holds the real files), so nothing below re-applies what is ours.
    if (pendingItems() == 0) {
        done(false, {}, tr("There are no copied files"));
        return;
    }
    if (m_current.listRefused) {
        done(false, {}, tr("The copied files cannot be pasted before they are transferred"));
        return;
    }
    // Only files from a publisher that let this device in already (a trusted device, or an older
    // peer whose offer is open): a guest's paste goes through the master's grant, after which its
    // files are transferred, never listed first.
    if (!m_current.filesAllowed) {
        done(false, {}, tr("The copied files are not listed on this device: paste them"));
        return;
    }
    const qint64 now = nowMs();
    auto serve = [&](const QList<ListedFile> &items) {
        m_list.kid = kid; // whose ask time this is (evidenceReference)
        m_list.askedAt = now;
        m_list.answeredAt = now;
        done(true, items, {});
    };
    // Listed in the announcement (counted against fc and dc, and checked against the bounds of a
    // streaming paste, when it came: onClip). Checked again, should the bounds be missed there.
    if (!m_current.fileList.isEmpty()) {
        const QString violation = listViolation(m_current.fileList, -1, -1);
        if (!violation.isEmpty()) {
            qCInfo(lcClipboard) << "The copied files cannot be pasted before they are transferred:" << violation;
            m_current.listRefused = true;
            emitApply(); // the owner's next object pastes them once transferred
            done(false, {}, violation);
            return;
        }
        serve(m_current.fileList);
        return;
    }
    if (m_list.have && m_list.kid == kid) {
        serve(m_list.items);
        return;
    }
    if (m_list.handle) { // on its way: joined
        m_list.waiters.append({std::move(done), now});
        return;
    }
    // Not announced as short (sp) or too many to stream: such a list would only be refused, after
    // moving the publisher's data for nothing. Pasted once transferred, as it was applied.
    if (!m_current.shortPaths || pendingItems() > kMaxVirtualEntries) {
        done(false, {}, tr("The copied files cannot be pasted before they are transferred"));
        return;
    }
    const int files = m_current.pendingFiles;
    const int dirs = m_current.pendingDirs;
    m_list = FileListState{};
    m_list.stamp = m_current.stamp;
    m_list.kid = kid;
    m_list.waiters.append({std::move(done), now});
    qCInfo(lcClipboard) << "Listing" << files << "copied files and" << dirs << "folders";
    m_list.handle = m_transfer->fetchList(
        m_current.filesFrom, m_current.remoteFileOffer, m_current.pendingBytes,
        [this, kid, files, dirs](bool ok, bool refused, const QString &error, const QList<ListedFile> &items) {
            if (!m_list.handle || m_list.kid != kid) // dropped (cancelList keeps it from coming)
                return;
            m_list.handle = 0;
            if (!ok && !refused) {
                qCInfo(lcClipboard) << "Could not list the copied files:" << error;
                answerListWaiters(false, {}, error); // not kept: the next request tries again
                return;
            }
            // The publisher's manifest refused as such (paths that differ only in case here, too
            // many items, sizes beyond the announcement) or a list beyond what was announced (sp,
            // fc or dc): deterministic, so no streaming paste of these files, and no asking again.
            const QString violation = ok ? listViolation(items, files, dirs) : error;
            if (!violation.isEmpty()) {
                qCWarning(lcClipboard) << "The list of the copied files is refused:" << violation;
                m_current.listRefused = true;
                answerListWaiters(false, {}, violation);
                if (m_current.kid == kid)
                    emitApply();
                return;
            }
            m_list.have = true;
            m_list.items = items;
            answerListWaiters(true, items, {});
        });
}

void ClipboardSync::answerListWaiters(bool ok, const QList<ListedFile> &items, const QString &error)
{
    const QList<ListWaiter> waiters = std::exchange(m_list.waiters, {});
    if (ok) {
        // Each request's window counts from its own ask: the latest one is the list last served.
        for (const ListWaiter &w : waiters)
            m_list.askedAt = std::max(m_list.askedAt, w.askedAt);
        m_list.answeredAt = nowMs();
    }
    for (const ListWaiter &w : waiters)
        w.done(ok, items, error);
}

QString ClipboardSync::listViolation(const QList<ListedFile> &list, int files, int dirs)
{
    if (list.size() > kMaxVirtualEntries)
        return tr("More than %1 copied files and folders").arg(kMaxVirtualEntries);
    for (const ListedFile &f : list) {
        if (f.path.size() > kMaxVirtualPath)
            return tr("A copied item's path is too long: %1").arg(f.path);
    }
    const qsizetype listedDirs = listedDirCount(list);
    if ((dirs >= 0 && listedDirs != dirs) || (files >= 0 && list.size() - listedDirs != files))
        return tr("The copied files are not those announced");
    return {};
}

void ClipboardSync::dropList()
{
    if (m_list.handle)
        m_transfer->cancelList(m_list.handle);
    const QList<ListWaiter> waiters = std::exchange(m_list.waiters, {});
    m_list = FileListState{};
    for (const ListWaiter &w : waiters)
        w.done(false, {}, tr("The clipboard changed")); // replaced: nothing went wrong
}

bool ClipboardSync::filesStreamable() const
{
    if (pendingItems() == 0 || !m_current.filesAllowed || m_current.listRefused || needsPasteEvidence())
        return false;
    return !m_current.fileList.isEmpty() || (m_current.shortPaths && pendingItems() <= kMaxVirtualEntries);
}

void ClipboardSync::cancelFileFetch()
{
    if (!m_job)
        return;
    if (m_job->handle) {
        // A transfer a streaming paste reads (Explorer copying the files as they arrive) goes on
        // for it: the newer clipboard does not cancel a paste the user started. Any other ends.
        const bool streaming = std::any_of(m_job->waiters.cbegin(), m_job->waiters.cend(),
                                           [](const Waiter &w) { return w.streaming; });
        if (streaming) {
            qCInfo(lcClipboard) << "The clipboard changed while its files are pasted: that transfer goes on";
            const std::shared_ptr<FilesJob> job = std::exchange(m_job, {});
            m_detachedJobs.push_back(job);
            // It goes on for the streaming pastes only: a plain request of the replaced clipboard
            // ends as it would without them (it must not keep the transfer alive once they go).
            QList<FilesCallback> plain;
            for (auto it = job->waiters.begin(); it != job->waiters.end();) {
                if (it->streaming) {
                    ++it;
                } else {
                    plain.append(it->done);
                    it = job->waiters.erase(it);
                }
            }
            for (const FilesCallback &done : std::as_const(plain))
                done(false, {}, tr("The clipboard changed meanwhile"), FetchError::Changed);
        } else {
            cancelFilesJob(m_job, FetchError::Changed);
        }
        return;
    }
    m_filesGrantTimer.stop();
    m_filesRid = 0;
    if (m_ownAllowArid)
        m_pendingAllows.remove(std::exchange(m_ownAllowArid, 0));
    failFilesWaiters(tr("The clipboard changed meanwhile"), false, FetchError::Changed);
}

QString ClipboardSync::peerName(const QByteArray &peerId) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == peerId)
            return p.name;
    }
    return QString::fromLatin1(peerId.toHex().left(8));
}

// --- Pruning the received copies ----------------------------------------------------------------
//
// The oldest received copies beyond kKeptDownloads go, as the console user, off the main thread.
// Before a paste's transfer they are only renamed out of the way ("<kRetiredPrefix><name>-<random>"
// in the same folder: no copy is ever named so, the ordering below leaves such names out, and the
// random part means a rename never meets an earlier one) and deleted alongside the transfer; any
// prune deletes what is left so named (a crash, or a service stopped while deleting). One process-
// wide set of the folders being deleted keeps two prunes (a paste's cleanup and the prune after a
// transfer) from walking the same tree at once. A paste's cleanup that finds a folder claimed by
// another prune waits until that one is done with it: its end (FetchOptions::cleanupAsUser
// returning) tells the transfer the space is free for its free space check, which would otherwise
// run, and refuse a copy that fits, while the other thread is still deleting. fs::removeTree never
// follows a link, and a link renamed aside is renamed itself, never what it points to.

namespace {
const QLatin1String kRetiredPrefix(".removing-");

std::mutex g_removingMutex;
std::condition_variable g_removed; // a folder left g_removing
QSet<QString> g_removing;          // folders being deleted, cleaned paths

bool claimRemoval(const QString &path)
{
    std::lock_guard<std::mutex> lock(g_removingMutex);
    if (g_removing.contains(path))
        return false;
    g_removing.insert(path);
    return true;
}

void endRemoval(const QString &path)
{
    {
        std::lock_guard<std::mutex> lock(g_removingMutex);
        g_removing.remove(path);
    }
    g_removed.notify_all();
}

// Waits until no prune is deleting `path` (cleaned). The claimant holds no lock while it deletes
// and only ever one claim, released whatever removeTree did, so this always ends.
void awaitRemoval(const QString &path)
{
    std::unique_lock<std::mutex> lock(g_removingMutex);
    g_removed.wait(lock, [&] { return !g_removing.contains(path); });
}

enum class IfClaimed { Skip, Wait };

// Deletes `path` unless another prune is deleting it already; then, with IfClaimed::Wait, returns
// only once that one is done (not deleting it again: what it could not delete, open files on
// Windows, this one could not either).
void removeOnce(const QString &path, IfClaimed ifClaimed = IfClaimed::Skip)
{
    const QString clean = QDir::cleanPath(path);
    if (!claimRemoval(clean)) {
        if (ifClaimed == IfClaimed::Wait)
            awaitRemoval(clean);
        return;
    }
    fs::removeTree(clean);
    endRemoval(clean);
}

bool isRetiredName(const QString &name) { return name.startsWith(kRetiredPrefix); }

// The received copies in `dir`, oldest first, leaving out the folders set aside and those `skip`
// keeps (a transfer still writes there, or a paste still reads there): they are not counted.
QStringList receivedOldestFirst(const QDir &dir, const std::function<bool(const QString &)> &skip)
{
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    entries.removeIf([&](const QString &name) { return isRetiredName(name) || (skip && skip(dir.filePath(name))); });
    // "<stamp>-<n>": by stamp, then by n (numerically: "-10" after "-9"), then by name.
    auto key = [](const QString &name) {
        return std::make_tuple(name.section(QLatin1Char('-'), 0, 0).toLongLong(),
                               name.section(QLatin1Char('-'), 1).toLongLong(), name);
    };
    std::sort(entries.begin(), entries.end(), [&](const QString &a, const QString &b) { return key(a) < key(b); });
    return entries;
}

// The folders set aside and never deleted (hidden on Linux and macOS: asked for explicitly).
QStringList retiredLeftovers(const QDir &dir)
{
    QStringList paths;
    const QStringList names = dir.entryList({kRetiredPrefix + QLatin1Char('*')},
                                            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QString &name : names) {
        if (isRetiredName(name))
            paths.append(dir.filePath(name));
    }
    return paths;
}
} // namespace

QString ClipboardSync::retireFolder(const QString &path)
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    if (name.isEmpty())
        return {};
    const QString aside = QStringLiteral("%1%2-%3")
                              .arg(kRetiredPrefix, name,
                                   QString::number(QRandomGenerator::global()->generate64(), 16));
    QDir parent = info.dir();
    if (!parent.rename(name, aside))
        return {}; // open inside (Windows), gone, or not ours to rename
    return parent.filePath(aside);
}

QStringList ClipboardSync::retireReceived(const QString &clipboardDir, int keep,
                                          const std::function<bool(const QString &)> &skip)
{
    const QDir dir(clipboardDir);
    QStringList out = retiredLeftovers(dir);
    QStringList entries = receivedOldestFirst(dir, skip);
    while (entries.size() > keep) {
        const QString path = dir.filePath(entries.takeFirst());
        const QString aside = retireFolder(path);
        // Not renamed (a file open inside it, on Windows): deleted in place later instead, as far
        // as it can be, if nothing uses it by then.
        out.append(aside.isEmpty() ? path : aside);
    }
    return out;
}

void ClipboardSync::removeRetired(const QStringList &paths, const std::function<bool(const QString &)> &skip)
{
    QElapsedTimer took;
    took.start();
    int removed = 0;
    for (const QString &path : paths) {
        // A folder renamed aside is nobody's; one left under its own name is checked again: a
        // transfer or a paste may use it by now.
        if (!isRetiredName(QFileInfo(path).fileName()) && skip && skip(path))
            continue;
        removeOnce(path, IfClaimed::Wait); // the space must be free when this returns
        ++removed;
    }
    if (removed > 0)
        qCInfo(lcClipboard) << "Deleted" << removed << "older received copies in" << took.elapsed() << "ms";
}

void ClipboardSync::pruneReceived(const QString &clipboardDir, int keep,
                                  const std::function<bool(const QString &)> &skip)
{
    const QDir dir(clipboardDir);
    for (const QString &leftover : retiredLeftovers(dir))
        removeOnce(leftover);
    QStringList entries = receivedOldestFirst(dir, skip);
    while (entries.size() > keep)
        removeOnce(dir.filePath(entries.takeFirst()));
}

void ClipboardSync::pruneLater(const QString &clipboardDir) const
{
    m_transfer->runAsUser([clipboardDir, skip = pruneSkip()] {
        pruneReceived(clipboardDir, kKeptDownloads, skip);
    });
}

} // namespace df
