#include "filedrag.h"

#include "filetransfer.h"
#include "inputcontroller.h"
#include "fsutil.h"
#include "node.h"
#include "workspace.h"

#include <QCborArray>
#include <QCborMap>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QVersionNumber>

#include <algorithm>
#include <atomic>

// Drag milestones (catch, hover, release, transfer, result) go to the always-on drag log
// (installDiagnosticLog): counts, sizes and short device ids only, never names or paths.
Q_LOGGING_CATEGORY(lcDrag, "deskflower.drag")

namespace df {

namespace {
const QString kT = QStringLiteral("t");
const QString kO = QStringLiteral("o");

QCborMap typed(const char *t)
{
    QCborMap m;
    m.insert(kT, QString::fromLatin1(t));
    return m;
}

QString shortId(const QByteArray &id)
{
    return QString::fromLatin1(id.toHex().left(8));
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// The listed names must all be safe, and the list must hold exactly `count` files and `dirs`
// folders, or none is used.
QList<FileDrag::ListedFile> listFromCbor(const QCborValue &value, int count, int dirs)
{
    QList<FileDrag::ListedFile> files;
    if (!listedFromCbor(value, &files))
        return {};
    const int listedDirs = listedDirCount(files);
    if (listedDirs != dirs || files.size() - listedDirs != count)
        return {};
    return files;
}

// `name` in `folder`, or "name (2).ext", "name (3).ext"... if taken: nothing is overwritten.
QString freeName(const QDir &folder, const QString &name)
{
    if (!QFileInfo::exists(folder.filePath(name)))
        return folder.filePath(name);
    const QFileInfo info(name);
    const QString base = info.suffix().isEmpty() ? name : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 2;; ++n) {
        const QString candidate = folder.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}
} // namespace

// A dragged folder tree being listed on a thread of its own (see catchHere).
struct FileDrag::Walk {
    std::thread thread;
    std::atomic_bool finished = false;
};

FileDrag::FileDrag(Node *node, FileTransfer *transfer, Workspace *workspace, InputController *input, Agent agent,
                   FolderProvider downloads, QObject *parent)
    : QObject(parent), m_node(node), m_transfer(transfer), m_workspace(workspace), m_input(input),
      m_agent(std::move(agent)), m_downloads(std::move(downloads))
{
    connect(m_node, &Node::messageReceived, this, &FileDrag::onMessage);
    connect(m_input, &InputController::dragCatchRequested, this, &FileDrag::onCatchRequested);
    connect(m_input, &InputController::dragDropped, this, &FileDrag::onDropped);
    connect(m_input, &InputController::activeDeviceChanged, this, &FileDrag::onActiveChanged);
    connect(m_input, &InputController::dragRefused, this, [this](const QByteArray &target) {
        refusalNotice(QStringLiteral("enter"), target,
                      tr("%1 takes no keyboard and mouse from this device: the dragged files stay where they are")
                          .arg(peerName(target)));
    });
    connect(m_input, &InputController::dragCatchTimedOut, this, [this](const QByteArray &source) {
        qCInfo(lcDrag) << "Listing on" << shortId(source) << "took too long: crossed without the files";
        emit notice(tr("Listing the items dragged on %1 took too long: nothing was moved").arg(peerName(source)),
                    true);
    });
    // The drag shown here belongs to a master and a source: without either it cannot go on.
    connect(m_node, &Node::peerDisconnected, this, [this](const QByteArray &peerId) {
        const auto it = m_heres.constFind(m_shown);
        if (m_shown && it != m_heres.constEnd() && (it->source == peerId || it->master == peerId))
            unhoverHere(m_shown, tr("%1 disconnected").arg(peerName(peerId)));
    });
    connect(m_workspace, &Workspace::changed, this, [this] {
        const auto it = m_heres.constFind(m_shown);
        if (m_shown && it != m_heres.constEnd() && it->master != m_workspace->masterId())
            unhoverHere(m_shown, tr("Another device took over the keyboard and mouse"));
    });
    setNativeDrag(bool(m_agent.hover));
}

FileDrag::~FileDrag()
{
    for (const auto &walk : m_walks) {
        if (walk->thread.joinable())
            walk->thread.join();
    }
}

void FileDrag::setNativeDrag(bool on)
{
    if (on != m_nativeDrag)
        qCInfo(lcDrag) << "Native drags on this device:" << on;
    m_nativeDrag = on;
    m_node->setCapability(QString::fromLatin1(kDragCapability), on);
}

QString FileDrag::peerVersion(const QByteArray &id) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == id)
            return p.version;
    }
    return {};
}

bool FileDrag::understandsFolders(const QByteArray &id) const
{
    return id == m_node->id() || peerUnderstandsFolders(peerVersion(id));
}

bool FileDrag::understandsRevision(const QByteArray &id) const
{
    if (id == m_node->id())
        return true;
    const QVersionNumber v = QVersionNumber::fromString(peerVersion(id));
    return !v.isNull() && v >= QVersionNumber::fromString(QLatin1String(kDragRevisionVersion));
}

bool FileDrag::showsDrags(const QByteArray &device) const
{
    if (device == m_node->id())
        return m_nativeDrag;
    for (const PeerView &p : m_node->peers()) {
        if (p.id != device)
            continue;
        if (p.capabilitiesKnown)
            return p.capabilities.contains(QLatin1String(kDragCapability));
        // Before capabilities were announced only the Windows agent ran drags.
        return p.platform.isEmpty() || p.platform.startsWith(QLatin1String("windows"));
    }
    return false;
}

QString FileDrag::peerName(const QByteArray &id) const
{
    if (id == m_node->id())
        return m_node->name();
    for (const PeerView &p : m_node->peers()) {
        if (p.id == id)
            return p.name;
    }
    return shortId(id);
}

void FileDrag::refusalNotice(const QString &kind, const QByteArray &device, const QString &text)
{
    const QString key = kind + QLatin1Char(':') + QString::fromLatin1(device.toHex());
    const qint64 now = nowMs();
    const auto it = m_refusalNoticed.constFind(key);
    if (it != m_refusalNoticed.constEnd() && now - *it < kRefusalNoticeMs)
        return;
    m_refusalNoticed.insert(key, now);
    qCInfo(lcDrag) << "Refused:" << kind << shortId(device);
    emit notice(text, true);
}

void FileDrag::pruneDownloads(const QString &dir) const
{
    // On a transfer thread, as the user (whose folders they are), never under a fetch still at work.
    m_transfer->runAsUser([dir, skip = m_transfer->activeTargetCheck()] {
        const QDir d(dir);
        QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
        entries.removeIf([&](const QString &name) { return skip(d.filePath(name)); });
        while (entries.size() > kKeptDownloads)
            fs::removeTree(d.filePath(entries.takeFirst()));
    });
}

// --- Master side ---------------------------------------------------------------------------------

void FileDrag::onCatchRequested(const QByteArray &source, QPoint at)
{
    m_dragSource.clear();
    m_dragOffer = 0;
    m_dragFiles.clear();
    m_hoverDevice.clear();
    qCInfo(lcDrag) << "Catch on" << shortId(source);
    if (source == m_node->id()) {
        catchHere(at, [this, source](int) { m_input->dragCatchPending(source); },
                  [this, source](quint64 offer, int count, int dirs, qint64 bytes, const QList<ListedFile> &files) {
                      if (m_input->catchSource() != source) {
                          // The crossing stopped waiting (dragCatchTimedOut): nobody drags these.
                          withdraw(offer);
                          return;
                      }
                      m_dragSource = source;
                      m_dragOffer = offer;
                      m_dragCount = count;
                      m_dragDirs = dirs;
                      m_dragBytes = bytes;
                      m_dragFiles = files;
                      m_input->dragCaught(source, offer != 0);
                  });
        return;
    }
    if (m_node->isGuest(source)) {
        // Its answer would be ignored (a guest could name any files), so it is not asked: the
        // crossing goes ahead at once instead of waiting out the catch timeout.
        refusalNotice(QStringLiteral("guest-source"), source,
                      tr("%1 is a guest: files cannot be dragged from it").arg(peerName(source)));
        m_input->dragCaught(source, false);
        return;
    }
    QCborMap m = typed("drag.catch");
    m.insert(QStringLiteral("x"), at.x());
    m.insert(QStringLiteral("y"), at.y());
    m_node->sendMessage(source, m);
}

void FileDrag::onActiveChanged(const QByteArray &device)
{
    if (!m_input->isDraggingFiles() || !m_dragOffer || m_dragFiles.isEmpty() || device == m_hoverDevice)
        return;
    if (!m_hoverDevice.isEmpty())
        sendUnhover(std::exchange(m_hoverDevice, {}));
    if (!showsDrags(device)) {
        // Nothing would show it there: a release on it sends drag.drop (saved into Received).
        qCInfo(lcDrag) << "No native drags on" << shortId(device) << ": shown nothing there";
        return;
    }
    m_hoverDevice = device;
    sendHover(device);
}

void FileDrag::sendHover(const QByteArray &device)
{
    qCInfo(lcDrag) << "Hover on" << shortId(device);
    if (device == m_node->id()) {
        hoverHere(m_node->id(), m_dragSource, m_dragOffer, m_dragCount, m_dragDirs, m_dragBytes, m_dragFiles);
        return;
    }
    // A device that knows no folder entries would refuse the whole list over one: it gets the
    // files alone (folders holding files still arrive through their paths).
    const bool folders = understandsFolders(device);
    QCborMap m = typed("drag.hover");
    m.insert(kO, qint64(m_dragOffer));
    m.insert(QStringLiteral("s"), QString::fromLatin1(m_dragSource.toHex()));
    m.insert(QStringLiteral("n"), m_dragCount);
    if (folders && m_dragDirs > 0)
        m.insert(QStringLiteral("dc"), m_dragDirs);
    m.insert(QStringLiteral("b"), m_dragBytes);
    m.insert(QStringLiteral("fl"), listedToCbor(m_dragFiles, folders));
    m_node->sendMessage(device, m);
}

void FileDrag::sendUnhover(const QByteArray &device)
{
    if (device == m_node->id()) {
        unhoverHere(m_dragOffer, tr("The drag left this device"));
        return;
    }
    QCborMap m = typed("drag.unhover");
    m.insert(kO, qint64(m_dragOffer));
    m_node->sendMessage(device, m);
}

void FileDrag::onDropped(const QByteArray &target, QPoint at)
{
    const QByteArray source = std::exchange(m_dragSource, {});
    const quint64 offer = std::exchange(m_dragOffer, 0);
    const QByteArray hovered = std::exchange(m_hoverDevice, {});
    const bool listed = !m_dragFiles.isEmpty();
    if (!offer)
        return;
    qCInfo(lcDrag) << "Released on" << shortId(target) << (listed && hovered == target ? "(drag shown)" : "");
    // The source learns where it went (and accepts a result from that device only).
    if (source == m_node->id()) {
        dropped(offer, target);
    } else {
        QCborMap d = typed("drag.dropped");
        d.insert(kO, qint64(offer));
        d.insert(QStringLiteral("d"), QString::fromLatin1(target.toHex()));
        m_node->sendMessage(source, d);
    }
    if (listed && hovered == target) {
        // The drag shown there ends with a drop: whatever is under the cursor takes the files.
        if (target == m_node->id()) {
            releaseHere(offer);
        } else {
            QCborMap r = typed("drag.release");
            r.insert(kO, qint64(offer));
            r.insert(QStringLiteral("x"), at.x());
            r.insert(QStringLiteral("y"), at.y());
            m_node->sendMessage(target, r);
        }
        return;
    }
    if (target == source)
        return; // back where they are: nothing to transfer
    if (target == m_node->id()) {
        dropHere(source, offer, m_dragCount, m_dragDirs, m_dragBytes, at);
        return;
    }
    QCborMap m = typed("drag.drop");
    m.insert(kO, qint64(offer));
    m.insert(QStringLiteral("s"), QString::fromLatin1(source.toHex()));
    m.insert(QStringLiteral("n"), m_dragCount);
    if (m_dragDirs > 0 && understandsFolders(target))
        m.insert(QStringLiteral("dc"), m_dragDirs);
    m.insert(QStringLiteral("b"), m_dragBytes);
    m.insert(QStringLiteral("x"), at.x());
    m.insert(QStringLiteral("y"), at.y());
    m_node->sendMessage(target, m);
}

// --- Source side ---------------------------------------------------------------------------------

void FileDrag::catchHere(QPoint at, std::function<void(int)> pending, CaughtCallback done)
{
    if (!m_agent.catchFiles) {
        done(0, 0, 0, 0, {});
        return;
    }
    const quint64 seq = ++m_catchSeq;
    m_agent.catchFiles(at, [this, seq, pending, done](const QStringList &paths) {
        // A newer catch replaced this one: it answers, and drag.caught names no catch, so an
        // answer from this one would be taken for the newer one's.
        if (seq != m_catchSeq)
            return;
        if (paths.isEmpty()) {
            done(0, 0, 0, 0, {});
            return;
        }
        // Files are being dragged. Listing them may take a while (a large tree, read with the
        // console user's rights): the master waits longer, and the cursor says why.
        qCInfo(lcDrag) << "Caught" << paths.size() << "dragged items: listing them";
        if (pending)
            pending(int(paths.size()));
        if (m_agent.badge)
            m_agent.badge(tr("Preparing %n item(s)…", nullptr, int(paths.size())));
        m_walks.erase(std::remove_if(m_walks.begin(), m_walks.end(),
                                     [](const std::shared_ptr<Walk> &w) {
                                         if (!w->finished)
                                             return false;
                                         w->thread.join();
                                         return true;
                                     }),
                      m_walks.end());
        auto walk = std::make_shared<Walk>();
        const Impersonator impersonate = m_transfer->impersonator();
        QElapsedTimer started;
        started.start();
        walk->thread = std::thread([this, walk, impersonate, paths, seq, done, started] {
            auto offer = std::make_shared<Offer>();
            QString error;
            bool ok = false;
            {
                // As the console user, as FileTransfer::addOfferPaths does (on this thread).
                const std::shared_ptr<void> asUser = impersonate ? impersonate() : std::make_shared<int>(0);
                if (!asUser)
                    error = QStringLiteral("Nobody is signed in");
                else
                    ok = Offer::addPaths(offer.get(), paths, kMaxBytes, kMaxFiles, &error);
            }
            // The destructor joins this thread, so `this` outlives the call.
            QMetaObject::invokeMethod(this, [this, offer, ok, error, paths, seq, done, started] {
                if (m_agent.badge)
                    m_agent.badge({});
                if (seq != m_catchSeq) {
                    qCInfo(lcDrag) << "Listing ended after a newer catch: dropped";
                    return; // the newer catch answers (see above)
                }
                qCInfo(lcDrag) << "Listed in" << started.elapsed() << "ms:" << offer->files.size() << "files,"
                               << offer->dirs.size() << "folders," << offer->fileBytes() << "bytes,"
                               << offer->skipped.size() << "left out" << (ok ? "" : "(refused)");
                if (!ok) {
                    emit notice(tr("These files cannot be moved to another device: %1").arg(error), true);
                    done(0, 0, 0, 0, {});
                    return;
                }
                if (!offer->skipped.isEmpty())
                    emit notice(tr("%n dragged item(s) stay here (links or special files)", nullptr,
                                   int(offer->skipped.size())),
                                true);
                if (!offer->hasFiles()) {
                    done(0, 0, 0, 0, {});
                    return;
                }
                QList<ListedFile> files;
                if (offer->files.size() + offer->dirs.size() <= kMaxListedFiles)
                    files = offer->listing();
                // Older drags that were never released anywhere are over. Those released on a
                // device may still be taken there (a transfer into a folder, a drop target
                // extracting on its own thread): they stay until their result, for a while.
                const qint64 now = nowMs();
                for (auto it = m_outgoing.begin(); it != m_outgoing.end();) {
                    if (!it->target.isEmpty() && now - it->droppedAt < kDroppedResultMs) {
                        ++it;
                        continue;
                    }
                    m_transfer->unpublish(it.key());
                    it = m_outgoing.erase(it);
                }
                const quint64 id = m_transfer->publish(*offer);
                m_outgoing.insert(id, Outgoing{paths, {}, int(offer->files.size()), int(offer->dirs.size()), files, 0});
                done(id, int(offer->files.size()), int(offer->dirs.size()), offer->fileBytes(), files);
            }, Qt::QueuedConnection);
            walk->finished = true;
        });
        m_walks.push_back(std::move(walk));
    });
}

void FileDrag::withdraw(quint64 offer)
{
    if (!offer || !m_outgoing.remove(offer))
        return;
    qCInfo(lcDrag) << "Offer withdrawn: the catch was given up";
    m_transfer->unpublish(offer);
}

void FileDrag::dropped(quint64 offer, const QByteArray &target)
{
    auto it = m_outgoing.find(offer);
    if (it == m_outgoing.end())
        return;
    if (target == m_node->id()) {
        // Dropped back here: Windows moved (or did not move) the originals itself.
        qCInfo(lcDrag) << "Dropped back on the source: nothing transferred";
        m_transfer->unpublish(offer);
        m_outgoing.erase(it);
        return;
    }
    it->target = target;
    it->droppedAt = nowMs();
    // The user dropped them there: a guest may fetch these (and only these).
    m_transfer->allowPeer(offer, target, true);
}

void FileDrag::onResult(const QByteArray &from, quint64 offer, bool ok, bool kept, const QString &error)
{
    auto it = m_outgoing.find(offer);
    if (it == m_outgoing.end() || it->target != from)
        return; // only the device the files were dropped on may report
    const Outgoing out = *it;
    m_outgoing.erase(it);
    m_transfer->unpublish(offer);
    const int items = out.count + out.dirs;
    if (ok && !kept && !understandsRevision(from)) {
        // An older device says ok as soon as the drop target read the virtual files, with no word
        // on whether it kept them (its Received copy proves nothing): a copy, never a move.
        qCInfo(lcDrag) << "Result from an older device: taken as a copy";
        kept = true;
    }
    qCInfo(lcDrag) << "Result from" << shortId(from) << (ok ? (kept ? "copied" : "taken") : "failed");
    if (!ok) {
        emit notice(tr("The files were not moved to %1: %2").arg(peerName(from), error), true);
        return;
    }
    if (kept) {
        // The drop target read them but never said it kept them: a copy, the originals stay.
        emit notice(tr("%n item(s) copied to %1; the originals were kept (the drop was not confirmed as a move)",
                       nullptr, items)
                        .arg(peerName(from)),
                    false);
        return;
    }
    // Every file arrived complete and the target confirmed it: the originals go to the Recycle Bin
    // (recoverable).
    if (!m_agent.recycle) {
        emit notice(tr("%n item(s) copied to %1; the originals were kept", nullptr, items).arg(peerName(from)), false);
        return;
    }
    m_agent.recycle(out.paths, [this, items, from](bool done, const QString &why) {
        qCInfo(lcDrag) << "Originals recycled:" << done;
        if (done)
            emit notice(tr("%n item(s) moved to %1 (originals in the Recycle Bin)", nullptr, items).arg(peerName(from)),
                        false);
        else
            emit notice(tr("%n item(s) copied to %1, but the originals could not be recycled: %2", nullptr, items)
                            .arg(peerName(from), why),
                        true);
    });
}

// --- Device side (the cursor is here) ------------------------------------------------------------

void FileDrag::hoverHere(const QByteArray &master, const QByteArray &source, quint64 offer, int count, int dirs,
                         qint64 bytes, const QList<ListedFile> &files)
{
    if (offer && m_shown == offer)
        return;
    if (m_shown)
        unhoverHere(m_shown, tr("Another drag started"));
    if (!offer || count < 0 || dirs < 0 || count + dirs <= 0 || count > kMaxFiles || dirs > kMaxFiles || bytes < 0
        || bytes > kMaxBytes || m_heres.contains(offer)) {
        qCWarning(lcDrag) << "Ignored a drag that does not add up";
        return;
    }
    Hover hover;
    hover.offer = offer;
    hover.count = count + dirs;
    hover.local = master == m_node->id();
    if (source == m_node->id()) {
        const auto it = m_outgoing.constFind(offer);
        if (it == m_outgoing.constEnd())
            return;
        hover.own = true; // back on the source: the originals, nothing is transferred
        hover.paths = it->paths;
    } else {
        if (files.size() != count + dirs) {
            qCWarning(lcDrag) << "Ignored a drag whose list does not add up";
            return;
        }
        hover.files = files;
    }
    Here here;
    here.source = source;
    here.master = master;
    here.count = count + dirs;
    here.bytes = bytes;
    here.unavailable = !m_agent.hover;
    m_heres.insert(offer, here);
    m_shown = offer;
    qCInfo(lcDrag) << "Showing a drag of" << hover.count << "items," << bytes << "bytes from" << shortId(source)
                   << (hover.own ? "(the originals)" : "");
    if (m_agent.hover)
        m_agent.hover(hover, [this, offer](const HoverEnd &end) { onHoverEnded(offer, end); });
}

void FileDrag::onHoverEnded(quint64 offer, const HoverEnd &end)
{
    auto it = m_heres.find(offer);
    if (it == m_heres.end())
        return; // cancelled meanwhile: already answered
    qCInfo(lcDrag) << "Drag here ended:"
                   << (end.unavailable ? "unavailable" : end.lost ? "lost" : end.dropped ? "dropped" : "not dropped")
                   << "confirmed" << end.confirmed << "into a folder" << !end.folder.isEmpty() << "released"
                   << it->released;
    if (end.unavailable && !it->released) {
        it->unavailable = true; // no drag here after all: a release saves into Received
        return;
    }
    if (!it->released && !end.dropped && end.folder.isEmpty()) {
        // Ended here by itself (Escape, a watchdog, the agent gone): the master's drag goes on
        // until the button is released, which then takes nothing.
        it->cancelled = true;
        abandon(*it, tr("The drag was cancelled"));
        if (end.lost)
            emit notice(tr("The drag of files from %1 stopped on this device").arg(peerName(it->source)), true);
        return;
    }
    Here here = m_heres.take(offer);
    if (m_shown == offer)
        m_shown = 0;
    if (here.source == m_node->id())
        return; // the originals were dragged: Windows moved them, or not
    if (end.unavailable) {
        receiveInto(here.source, offer, here.count, here.bytes, {});
        return;
    }
    if (!end.folder.isEmpty()) {
        abandon(here, tr("Received into a folder instead"));
        // The agent named it; the service writes there (as the console user): an absolute path to
        // an existing folder only, else Received (which says where the files went).
        const bool usable = usableFolder(end.folder);
        if (!usable)
            qCWarning(lcDrag) << "The folder under the cursor cannot be used: into Received";
        receiveInto(here.source, offer, here.count, here.bytes, usable ? end.folder : QString());
        return;
    }
    if (end.lost) {
        // Released, and then the agent went away (or never answered): nothing here took them.
        abandon(here, tr("The drag ended unexpectedly"));
        emit notice(tr("The files dragged from %1 were not received: the drag here ended unexpectedly")
                        .arg(peerName(here.source)),
                    true);
        reportTo(here.source, offer, false, tr("the drag there ended unexpectedly"));
        return;
    }
    const bool fetched = !here.fetched.isEmpty();
    abandon(here, tr("The drag ended"));
    if (!end.dropped)
        reportTo(here.source, offer, false, tr("nothing under the cursor took them"));
    else if (!fetched)
        reportTo(here.source, offer, false, tr("they were not transferred"));
    else
        reportTo(here.source, offer, true, {}, !end.confirmed);
}

void FileDrag::abandon(Here &here, const QString &why)
{
    if (const quint64 handle = std::exchange(here.fetchHandle, 0))
        m_transfer->cancel(handle); // its callback finds no entry, or one without waiters
    const auto waiters = std::exchange(here.waiters, {});
    for (const auto &w : waiters)
        w(false, {}, why);
}

void FileDrag::unhoverHere(quint64 offer, const QString &why)
{
    if (!offer || offer != m_shown)
        return;
    Here here = m_heres.take(offer);
    m_shown = 0;
    qCInfo(lcDrag) << "Drag here cancelled";
    abandon(here, why);
    if (!here.unavailable && !here.cancelled && m_agent.endHover)
        m_agent.endHover(false);
}

void FileDrag::releaseHere(quint64 offer)
{
    if (!offer || offer != m_shown) {
        qCInfo(lcDrag) << "Released without a drag shown here";
        return;
    }
    m_shown = 0;
    auto it = m_heres.find(offer);
    if (it->cancelled || it->unavailable) {
        const Here here = m_heres.take(offer);
        if (here.source == m_node->id())
            return; // back on the source: nothing to take (the source saw the drop)
        if (here.cancelled)
            reportTo(here.source, offer, false, tr("the drag was cancelled there"));
        else
            receiveInto(here.source, offer, here.count, here.bytes, {}); // no drag here: into Received
        return;
    }
    it->released = true;
    qCInfo(lcDrag) << "Released here";
    if (m_agent.endHover)
        m_agent.endHover(true);
}

void FileDrag::fetchDragged(quint64 offer, std::function<void(bool, const QStringList &, const QString &)> done)
{
    auto it = m_heres.find(offer);
    if (!offer || it == m_heres.end() || it->source == m_node->id() || it->cancelled) {
        done(false, {}, tr("No files are being dragged here"));
        return;
    }
    if (!it->fetched.isEmpty()) {
        done(true, it->fetched, {});
        return;
    }
    it->waiters.append(std::move(done));
    if (it->fetchHandle)
        return;
    const QByteArray source = it->source;
    const int count = it->count;
    const QString dir = QDir(m_downloads()).filePath(QStringLiteral("Dragged"));
    // A fresh folder for every transfer: a drop target that reads the files again after a cancel
    // or a failure never meets what the earlier one left (its worker may still be deleting it).
    static std::atomic<quint64> transfers{0};
    const QString target =
        QDir(dir).filePath(QStringLiteral("%1-%2").arg(offer, 0, 16).arg(transfers.fetch_add(1) + 1));
    qCInfo(lcDrag) << "The drop target reads the files: transferring" << it->bytes << "bytes";
    emit notice(tr("Receiving %n item(s) from %1…", nullptr, count).arg(peerName(source)), false);
    QElapsedTimer started;
    started.start();
    const quint64 handle = m_transfer->fetch(source, offer, target, true, it->bytes, [=, this](const FetchResult &result) {
        qCInfo(lcDrag) << "Transfer for the drop target ended after" << started.elapsed() << "ms:"
                       << (result.ok ? "ok" : "failed");
        auto here = m_heres.find(offer);
        if (here == m_heres.end())
            return; // abandoned: its waiters were answered then
        here->fetchHandle = 0;
        const auto waiters = std::exchange(here->waiters, {});
        if (!result.ok) {
            emit notice(tr("Could not receive the files from %1: %2").arg(peerName(source), result.error), true);
            for (const auto &w : waiters)
                w(false, {}, result.error);
            return;
        }
        here->fetched = result.files;
        const QStringList fetched = result.files;
        pruneDownloads(dir);
        for (const auto &w : waiters)
            w(true, fetched, {});
    }, QStringLiteral("drag"));
    it = m_heres.find(offer);
    if (it != m_heres.end() && !it->waiters.isEmpty()) // otherwise the callback already ran
        it->fetchHandle = handle;
}

void FileDrag::reportTo(const QByteArray &source, quint64 offer, bool ok, const QString &error, bool kept,
                        const QString &folder)
{
    qCInfo(lcDrag) << "Reporting to" << shortId(source) << (ok ? (kept ? "copied" : "taken") : "failed");
    if (ok && folder.isEmpty())
        emit notice(tr("Files received from %1").arg(peerName(source)), false);
    else if (ok)
        emit notice(tr("Files received from %1 in %2").arg(peerName(source), QDir::toNativeSeparators(folder)), false);
    if (source == m_node->id()) {
        onResult(m_node->id(), offer, ok, kept, error);
        return;
    }
    QCborMap r = typed("drag.result");
    r.insert(kO, qint64(offer));
    if (kept && !understandsRevision(source)) {
        // It would take ok for a confirmed move and recycle the originals: refuse instead.
        r.insert(QStringLiteral("ok"), false);
        r.insert(QStringLiteral("e"), tr("they were copied there, the drop was not confirmed as a move"));
    } else {
        r.insert(QStringLiteral("ok"), ok);
        if (kept)
            r.insert(QStringLiteral("k"), true);
        r.insert(QStringLiteral("e"), error);
    }
    m_node->sendMessage(source, r);
}

void FileDrag::dropHere(const QByteArray &source, quint64 offer, int count, int dirs, qint64 bytes, QPoint at)
{
    if (count < 0 || dirs < 0 || count + dirs <= 0 || count > kMaxFiles || dirs > kMaxFiles || bytes < 0
        || bytes > kMaxBytes) {
        emit notice(tr("Files dragged from %1 were not accepted here").arg(peerName(source)), true);
        reportTo(source, offer, false, tr("Not accepted"));
        return;
    }
    count += dirs;
    if (!m_agent.folderAt) {
        receiveInto(source, offer, count, bytes, {});
        return;
    }
    m_agent.folderAt(at, [=, this](const QString &folder) {
        // Not over a folder window or the desktop (or no agent to tell): into Received.
        receiveInto(source, offer, count, bytes, usableFolder(folder) ? folder : QString());
    });
}

bool FileDrag::usableFolder(const QString &folder) const
{
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder))
        return false;
    const auto asUser = m_transfer->impersonate(); // what the console user can see
    return asUser && QFileInfo(folder).isDir();
}

void FileDrag::receiveInto(const QByteArray &source, quint64 offer, int count, qint64 bytes, const QString &folder)
{
    const bool intoReceived = folder.isEmpty();
    const QString destination = intoReceived ? m_downloads() : folder;
    if (intoReceived) {
        const auto asUser = m_transfer->impersonate();
        if (asUser)
            QDir().mkpath(destination);
    }
    // Into a hidden folder first, then into place under free names: a failed transfer leaves
    // nothing half-written, and nothing existing is overwritten.
    const QDir dest(destination);
    // Fresh for every transfer, like the dragged files' folders (see fetchDragged).
    static std::atomic<quint64> transfers{0};
    const QString staging =
        dest.filePath(QStringLiteral(".deskflower-drop-%1-%2").arg(offer, 0, 16).arg(transfers.fetch_add(1) + 1));
    qCInfo(lcDrag) << "Receiving" << count << "items," << bytes << "bytes into" << (intoReceived ? "Received" : "a folder");
    emit notice(tr("Receiving %n item(s) from %1…", nullptr, count).arg(peerName(source)), false);
    QElapsedTimer started;
    started.start();
    m_transfer->fetch(source, offer, staging, true, bytes, [=, this](const FetchResult &result) {
        // The staging folder and the drop folder are the console user's: move and clean up as
        // that user, never as LocalSystem.
        const auto asUser = m_transfer->impersonate();
        qCInfo(lcDrag) << "Transfer into the folder ended after" << started.elapsed() << "ms:"
                       << (result.ok && asUser ? "ok" : "failed");
        if (!result.ok || !asUser) {
            // After a failure or a cancel the transfer's worker removes what it wrote (and may
            // still be at it), off this thread.
            const QString why = asUser ? result.error : tr("nobody is signed in");
            emit notice(tr("Could not receive the files from %1: %2").arg(peerName(source), why), true);
            reportTo(source, offer, false, why);
            return;
        }
        int placed = 0;
        for (const QString &item : result.files) {
            const QString to = freeName(dest, QFileInfo(item).fileName());
            if (QFile::rename(item, to) || QDir().rename(item, to))
                ++placed;
        }
        fs::removeTree(staging);
        if (placed != result.files.size()) {
            const QString why = tr("Could not put every file in %1").arg(QDir::toNativeSeparators(destination));
            emit notice(why, true);
            reportTo(source, offer, false, why);
            return;
        }
        reportTo(source, offer, true, {}, false, destination);
    }, QStringLiteral("drop"));
}

// --- Messages ------------------------------------------------------------------------------------

void FileDrag::onMessage(const QByteArray &peerId, const QCborMap &msg)
{
    const QString t = msg.value(kT).toString();
    if (!t.startsWith(QLatin1String("drag.")))
        return;
    // A guest is never obeyed as this device's master, even if it holds the register (demoted after
    // it became master, or relayed by a third device that trusts it): mirror InputController and
    // RemoteView, which also refuse a guest master.
    const bool master = peerId == m_workspace->masterId() && peerId != m_node->id();
    const bool fromMaster = master && !m_node->isGuest(peerId);
    if (master && !fromMaster && t != QLatin1String("drag.result") && t != QLatin1String("drag.caught"))
        refusalNotice(QStringLiteral("guest-master"), peerId,
                      tr("%1 is a guest here: files are not dragged to or from it").arg(peerName(peerId)));
    const quint64 offer = quint64(msg.value(kO).toInteger());
    const QPoint at(int(msg.value(QStringLiteral("x")).toInteger()), int(msg.value(QStringLiteral("y")).toInteger()));
    const QByteArray source = QByteArray::fromHex(msg.value(QStringLiteral("s")).toString().toLatin1());
    const int count = int(msg.value(QStringLiteral("n")).toInteger());
    const int dirs = int(msg.value(QStringLiteral("dc")).toInteger());
    const qint64 bytes = msg.value(QStringLiteral("b")).toInteger();

    if (t == QLatin1String("drag.catch") && fromMaster) {
        // A master that knows no pending answer would take it for "no files".
        std::function<void(int)> pending;
        if (understandsRevision(peerId)) {
            pending = [this, peerId](int approx) {
                QCborMap p = typed("drag.caught");
                p.insert(QStringLiteral("pending"), true);
                p.insert(QStringLiteral("n"), approx);
                m_node->sendMessage(peerId, p);
            };
        }
        catchHere(at, pending, [this, peerId](quint64 id, int n, int dc, qint64 b, const QList<ListedFile> &files) {
            // An older master would drop a list with folders in it, and counts files only.
            const bool folders = understandsFolders(peerId);
            QCborMap r = typed("drag.caught");
            r.insert(kO, qint64(folders || n > 0 ? id : 0));
            r.insert(QStringLiteral("n"), n);
            if (folders && dc > 0)
                r.insert(QStringLiteral("dc"), dc);
            r.insert(QStringLiteral("b"), b);
            if (!files.isEmpty())
                r.insert(QStringLiteral("fl"), listedToCbor(files, folders));
            m_node->sendMessage(peerId, r);
        });
    } else if (t == QLatin1String("drag.caught") && m_workspace->masterId() == m_node->id()
               && m_input->catchSource() == peerId && !m_node->isGuest(peerId)) {
        // Only the device a catch is pending with may answer it: not a third peer racing to
        // substitute the dragged files, and never a guest.
        if (msg.value(QStringLiteral("pending")).toBool()) {
            qCInfo(lcDrag) << "The source is listing" << count << "dragged items";
            m_input->dragCatchPending(peerId);
            return;
        }
        m_dragSource = peerId;
        m_dragOffer = offer;
        m_dragCount = count;
        m_dragDirs = dirs;
        m_dragBytes = bytes;
        m_dragFiles = listFromCbor(msg.value(QStringLiteral("fl")), count, dirs);
        qCInfo(lcDrag) << "Caught on" << shortId(peerId) << ":" << count << "files," << dirs << "folders," << bytes
                       << "bytes" << (offer ? "" : "(nothing)");
        m_input->dragCaught(peerId, offer != 0);
    } else if (t == QLatin1String("drag.caught") && m_workspace->masterId() == m_node->id() && offer
               && !msg.value(QStringLiteral("pending")).toBool() && !m_node->isGuest(peerId)) {
        // The answer to a catch given up meanwhile (dragCatchTimedOut): nobody drags these files,
        // and "dropped back on the source" has it withdraw the offer (any version understands).
        qCInfo(lcDrag) << "Late catch answer from" << shortId(peerId) << ": withdrawn";
        QCborMap d = typed("drag.dropped");
        d.insert(kO, qint64(offer));
        d.insert(QStringLiteral("d"), QString::fromLatin1(peerId.toHex()));
        m_node->sendMessage(peerId, d);
    } else if (t == QLatin1String("drag.hover") && fromMaster) {
        hoverHere(peerId, source, offer, count, dirs, bytes, listFromCbor(msg.value(QStringLiteral("fl")), count, dirs));
    } else if (t == QLatin1String("drag.unhover") && fromMaster) {
        unhoverHere(offer, tr("The drag left this device"));
    } else if (t == QLatin1String("drag.release") && fromMaster) {
        releaseHere(offer);
    } else if (t == QLatin1String("drag.dropped") && fromMaster) {
        dropped(offer, QByteArray::fromHex(msg.value(QStringLiteral("d")).toString().toLatin1()));
    } else if (t == QLatin1String("drag.drop") && fromMaster) {
        dropHere(source, offer, count, dirs, bytes, at);
    } else if (t == QLatin1String("drag.result")) {
        onResult(peerId, offer, msg.value(QStringLiteral("ok")).toBool(), msg.value(QStringLiteral("k")).toBool(),
                 msg.value(QStringLiteral("e")).toString());
    }
}

} // namespace df
