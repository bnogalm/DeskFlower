#include "filedrag.h"

#include "filetransfer.h"
#include "inputcontroller.h"
#include "fsutil.h"
#include "node.h"
#include "workspace.h"

#include <QCborArray>
#include <QCborMap>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#include <algorithm>

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

QCborArray listToCbor(const QList<FileDrag::ListedFile> &files)
{
    QCborArray list;
    for (const FileDrag::ListedFile &f : files)
        list.append(QCborArray{f.path, f.size});
    return list;
}

// The listed names must all be safe, or none is used.
QList<FileDrag::ListedFile> listFromCbor(const QCborValue &value)
{
    QList<FileDrag::ListedFile> files;
    for (const QCborValue &v : value.toArray()) {
        const QCborArray item = v.toArray();
        const QString path = FileTransfer::sanitizeRelativePath(item.at(0).toString());
        const qint64 size = item.at(1).toInteger();
        if (path.isEmpty() || size < 0)
            return {};
        files.append(FileDrag::ListedFile{path, size});
    }
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

FileDrag::FileDrag(Node *node, FileTransfer *transfer, Workspace *workspace, InputController *input, Agent agent,
                   FolderProvider downloads, QObject *parent)
    : QObject(parent), m_node(node), m_transfer(transfer), m_workspace(workspace), m_input(input),
      m_agent(std::move(agent)), m_downloads(std::move(downloads))
{
    connect(m_node, &Node::messageReceived, this, &FileDrag::onMessage);
    connect(m_input, &InputController::dragCatchRequested, this, &FileDrag::onCatchRequested);
    connect(m_input, &InputController::dragDropped, this, &FileDrag::onDropped);
    connect(m_input, &InputController::activeDeviceChanged, this, &FileDrag::onActiveChanged);
}

QString FileDrag::peerName(const QByteArray &id) const
{
    if (id == m_node->id())
        return m_node->name();
    for (const PeerView &p : m_node->peers()) {
        if (p.id == id)
            return p.name;
    }
    return QString::fromLatin1(id.toHex().left(8));
}

void FileDrag::pruneDownloads(const QString &dir) const
{
    QDir d(dir);
    QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
    while (entries.size() > kKeptDownloads)
        fs::removeTree(d.filePath(entries.takeFirst()));
}

// --- Master side ---------------------------------------------------------------------------------

void FileDrag::onCatchRequested(const QByteArray &source, QPoint at)
{
    m_dragSource.clear();
    m_dragOffer = 0;
    m_dragFiles.clear();
    m_hoverDevice.clear();
    if (source == m_node->id()) {
        catchHere(at, [this, source](quint64 offer, int count, qint64 bytes, const QList<ListedFile> &files) {
            m_dragSource = source;
            m_dragOffer = offer;
            m_dragCount = count;
            m_dragBytes = bytes;
            m_dragFiles = files;
            m_input->dragCaught(source, offer != 0);
        });
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
        sendUnhover(m_hoverDevice);
    m_hoverDevice = device;
    sendHover(device);
}

void FileDrag::sendHover(const QByteArray &device)
{
    if (device == m_node->id()) {
        hoverHere(m_dragSource, m_dragOffer, m_dragCount, m_dragBytes, m_dragFiles);
        return;
    }
    QCborMap m = typed("drag.hover");
    m.insert(kO, qint64(m_dragOffer));
    m.insert(QStringLiteral("s"), QString::fromLatin1(m_dragSource.toHex()));
    m.insert(QStringLiteral("n"), m_dragCount);
    m.insert(QStringLiteral("b"), m_dragBytes);
    m.insert(QStringLiteral("fl"), listToCbor(m_dragFiles));
    m_node->sendMessage(device, m);
}

void FileDrag::sendUnhover(const QByteArray &device)
{
    if (device == m_node->id()) {
        unhoverHere(m_dragOffer);
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
        return; // without a drag shown there, nothing to do
    if (target == m_node->id()) {
        dropHere(source, offer, m_dragCount, m_dragBytes, at);
        return;
    }
    QCborMap m = typed("drag.drop");
    m.insert(kO, qint64(offer));
    m.insert(QStringLiteral("s"), QString::fromLatin1(source.toHex()));
    m.insert(QStringLiteral("n"), m_dragCount);
    m.insert(QStringLiteral("b"), m_dragBytes);
    m.insert(QStringLiteral("x"), at.x());
    m.insert(QStringLiteral("y"), at.y());
    m_node->sendMessage(target, m);
}

// --- Source side ---------------------------------------------------------------------------------

void FileDrag::catchHere(QPoint at, std::function<void(quint64, int, qint64, const QList<ListedFile> &)> done)
{
    if (!m_agent.catchFiles) {
        done(0, 0, 0, {});
        return;
    }
    m_agent.catchFiles(at, [this, done](const QStringList &paths) {
        if (paths.isEmpty()) {
            done(0, 0, 0, {});
            return;
        }
        Offer offer;
        QString error;
        if (!Offer::addPaths(&offer, paths, kMaxBytes, kMaxFiles, &error)) {
            emit notice(tr("These files cannot be moved to another device: %1").arg(error), true);
            done(0, 0, 0, {});
            return;
        }
        QList<ListedFile> files;
        if (offer.files.size() <= kMaxListedFiles) {
            for (const Offer::File &f : std::as_const(offer.files))
                files.append(ListedFile{f.relativePath, f.size});
        }
        // Older drags never reported back: they are over.
        for (auto it = m_outgoing.begin(); it != m_outgoing.end();) {
            m_transfer->unpublish(it.key());
            it = m_outgoing.erase(it);
        }
        const quint64 id = m_transfer->publish(offer);
        m_outgoing.insert(id, Outgoing{paths, {}, int(offer.files.size()), files});
        qCInfo(lcDrag) << "Dragging" << offer.files.size() << "files," << offer.fileBytes() << "bytes";
        done(id, int(offer.files.size()), offer.fileBytes(), files);
    });
}

void FileDrag::dropped(quint64 offer, const QByteArray &target)
{
    auto it = m_outgoing.find(offer);
    if (it == m_outgoing.end())
        return;
    if (target == m_node->id()) {
        // Dropped back here: Windows moved (or did not move) the originals itself.
        m_transfer->unpublish(offer);
        m_outgoing.erase(it);
        return;
    }
    it->target = target;
}

void FileDrag::onResult(const QByteArray &from, quint64 offer, bool ok, const QString &error)
{
    auto it = m_outgoing.find(offer);
    if (it == m_outgoing.end() || it->target != from)
        return; // only the device the files were dropped on may report
    const Outgoing out = *it;
    m_outgoing.erase(it);
    m_transfer->unpublish(offer);
    if (!ok) {
        emit notice(tr("The files were not moved to %1: %2").arg(peerName(from), error), true);
        return;
    }
    // Every file arrived complete: the originals go to the Recycle Bin (recoverable).
    if (!m_agent.recycle) {
        emit notice(tr("%n file(s) copied to %1; the originals were kept", nullptr, out.count).arg(peerName(from)), false);
        return;
    }
    m_agent.recycle(out.paths, [this, out, from](bool done, const QString &why) {
        if (done)
            emit notice(tr("%n file(s) moved to %1 (originals in the Recycle Bin)", nullptr, out.count).arg(peerName(from)),
                        false);
        else
            emit notice(tr("%n file(s) copied to %1, but the originals could not be recycled: %2", nullptr, out.count)
                            .arg(peerName(from), why),
                        true);
    });
}

// --- Device side (the cursor is here) ------------------------------------------------------------

void FileDrag::hoverHere(const QByteArray &source, quint64 offer, int count, qint64 bytes, const QList<ListedFile> &files)
{
    if (m_here.offer == offer)
        return;
    if (m_here.offer)
        unhoverHere(m_here.offer);
    if (!m_agent.hover || count <= 0 || count > kMaxFiles || bytes < 0 || bytes > kMaxBytes)
        return;
    Hover hover;
    hover.offer = offer;
    hover.count = count;
    if (source == m_node->id()) {
        const auto it = m_outgoing.constFind(offer);
        if (it == m_outgoing.constEnd())
            return;
        hover.own = true;
        hover.paths = it->paths;
    } else {
        if (files.size() != count)
            return;
        hover.files = files;
    }
    m_here = Here{};
    m_here.offer = offer;
    m_here.source = source;
    m_here.count = count;
    m_here.bytes = bytes;
    m_agent.hover(hover, [this, offer, source](bool dropped, bool transferred) {
        if (m_here.offer != offer)
            return; // left this device meanwhile (cancelled)
        const bool released = m_here.released;
        m_here = Here{};
        if (!released || source == m_node->id())
            return; // not dropped here, or the originals were handled by Windows
        if (!dropped)
            reportTo(source, offer, false, tr("nothing under the cursor took them"));
        else if (!transferred)
            reportTo(source, offer, false, tr("they were not transferred"));
        else
            reportTo(source, offer, true, {});
    });
}

void FileDrag::unhoverHere(quint64 offer)
{
    if (!offer || m_here.offer != offer || m_here.released)
        return;
    m_here = Here{};
    if (m_agent.endHover)
        m_agent.endHover(false);
}

void FileDrag::releaseHere(quint64 offer)
{
    if (!offer || m_here.offer != offer) {
        // No drag shown here (no agent?): nothing took the files.
        return;
    }
    m_here.released = true;
    if (m_agent.endHover)
        m_agent.endHover(true);
}

void FileDrag::fetchDragged(quint64 offer, std::function<void(bool, const QStringList &, const QString &)> done)
{
    if (!offer || m_here.offer != offer || m_here.source == m_node->id()) {
        done(false, {}, tr("No files are being dragged here"));
        return;
    }
    if (!m_here.fetched.isEmpty()) {
        done(true, m_here.fetched, {});
        return;
    }
    m_here.waiters.append(std::move(done));
    if (m_here.fetchHandle)
        return;
    const QByteArray source = m_here.source;
    const QString dir = QDir(m_downloads()).filePath(QStringLiteral("Dragged"));
    const QString target = QDir(dir).filePath(QString::number(offer, 16));
    emit notice(tr("Receiving %n file(s) from %1…", nullptr, m_here.count).arg(peerName(source)), false);
    const quint64 handle = m_transfer->fetch(source, offer, target, true, m_here.bytes, [=, this](const FetchResult &result) {
        if (m_here.offer != offer)
            return;
        m_here.fetchHandle = 0;
        const auto waiters = std::exchange(m_here.waiters, {});
        if (!result.ok) {
            emit notice(tr("Could not receive the files from %1: %2").arg(peerName(source), result.error), true);
            for (const auto &w : waiters)
                w(false, {}, result.error);
            return;
        }
        m_here.fetched = result.files;
        qCInfo(lcDrag) << "Transferred" << m_here.count << "dragged files";
        pruneDownloads(dir);
        for (const auto &w : waiters)
            w(true, m_here.fetched, {});
    });
    if (!m_here.waiters.isEmpty()) // otherwise the callback already ran
        m_here.fetchHandle = handle;
}

void FileDrag::reportTo(const QByteArray &source, quint64 offer, bool ok, const QString &error)
{
    if (ok)
        emit notice(tr("Files received from %1").arg(peerName(source)), false);
    if (source == m_node->id()) {
        onResult(m_node->id(), offer, ok, error);
        return;
    }
    QCborMap r = typed("drag.result");
    r.insert(kO, qint64(offer));
    r.insert(QStringLiteral("ok"), ok);
    r.insert(QStringLiteral("e"), error);
    m_node->sendMessage(source, r);
}

void FileDrag::dropHere(const QByteArray &source, quint64 offer, int count, qint64 bytes, QPoint at)
{
    if (count <= 0 || count > kMaxFiles || bytes < 0 || bytes > kMaxBytes || !m_agent.folderAt) {
        reportTo(source, offer, false, tr("Not accepted"));
        return;
    }
    m_agent.folderAt(at, [=, this](const QString &folder) {
        if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
            emit notice(tr("Drop files on a folder window or the desktop"), true);
            reportTo(source, offer, false, tr("Not dropped on a folder"));
            return;
        }
        // Into a hidden folder first, then into place under free names: a failed transfer leaves
        // nothing half-written, and nothing existing is overwritten.
        const QDir dest(folder);
        const QString staging = dest.filePath(QStringLiteral(".deskflower-drop-%1").arg(offer, 0, 16));
        emit notice(tr("Receiving %n file(s) from %1…", nullptr, count).arg(peerName(source)), false);
        m_transfer->fetch(source, offer, staging, true, bytes, [=, this](const FetchResult &result) {
            if (!result.ok) {
                fs::removeTree(staging);
                emit notice(tr("Could not receive the files from %1: %2").arg(peerName(source), result.error), true);
                reportTo(source, offer, false, result.error);
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
                reportTo(source, offer, false, tr("Could not put every file in %1").arg(QDir::toNativeSeparators(folder)));
                return;
            }
            qCInfo(lcDrag) << "Received" << count << "dragged files into" << folder;
            reportTo(source, offer, true, {});
        });
    });
}

// --- Messages ------------------------------------------------------------------------------------

void FileDrag::onMessage(const QByteArray &peerId, const QCborMap &msg)
{
    const QString t = msg.value(kT).toString();
    if (!t.startsWith(QLatin1String("drag.")))
        return;
    const bool fromMaster = peerId == m_workspace->masterId() && peerId != m_node->id();
    const quint64 offer = quint64(msg.value(kO).toInteger());
    const QPoint at(int(msg.value(QStringLiteral("x")).toInteger()), int(msg.value(QStringLiteral("y")).toInteger()));
    const QByteArray source = QByteArray::fromHex(msg.value(QStringLiteral("s")).toString().toLatin1());
    const int count = int(msg.value(QStringLiteral("n")).toInteger());
    const qint64 bytes = msg.value(QStringLiteral("b")).toInteger();

    if (t == QLatin1String("drag.catch") && fromMaster) {
        catchHere(at, [this, peerId](quint64 id, int n, qint64 b, const QList<ListedFile> &files) {
            QCborMap r = typed("drag.caught");
            r.insert(kO, qint64(id));
            r.insert(QStringLiteral("n"), n);
            r.insert(QStringLiteral("b"), b);
            if (!files.isEmpty())
                r.insert(QStringLiteral("fl"), listToCbor(files));
            m_node->sendMessage(peerId, r);
        });
    } else if (t == QLatin1String("drag.caught") && m_workspace->masterId() == m_node->id()) {
        m_dragSource = peerId;
        m_dragOffer = offer;
        m_dragCount = count;
        m_dragBytes = bytes;
        m_dragFiles = listFromCbor(msg.value(QStringLiteral("fl")));
        if (m_dragFiles.size() != count)
            m_dragFiles.clear();
        m_input->dragCaught(peerId, offer != 0);
    } else if (t == QLatin1String("drag.hover") && fromMaster) {
        hoverHere(source, offer, count, bytes, listFromCbor(msg.value(QStringLiteral("fl"))));
    } else if (t == QLatin1String("drag.unhover") && fromMaster) {
        unhoverHere(offer);
    } else if (t == QLatin1String("drag.release") && fromMaster) {
        releaseHere(offer);
    } else if (t == QLatin1String("drag.dropped") && fromMaster) {
        dropped(offer, QByteArray::fromHex(msg.value(QStringLiteral("d")).toString().toLatin1()));
    } else if (t == QLatin1String("drag.drop") && fromMaster) {
        dropHere(source, offer, count, bytes, at);
    } else if (t == QLatin1String("drag.result")) {
        onResult(peerId, offer, msg.value(QStringLiteral("ok")).toBool(), msg.value(QStringLiteral("e")).toString());
    }
}

} // namespace df
