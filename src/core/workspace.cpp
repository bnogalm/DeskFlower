#include "workspace.h"

#include "clock.h"
#include "node.h"

#include <QCborArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>

namespace df {

namespace {
const QString kT = QStringLiteral("t");

constexpr int kMaxThumbnailBytes = 256 * 1024;
constexpr int kMaxScreens = 16;
// Fresh captures go to peers that asked for thumbnails this recently.
constexpr qint64 kThumbRequestWindowMs = 5 * 1000;

QRect bounds(const QList<ScreenInfo> &screens)
{
    QRect r;
    for (const ScreenInfo &s : screens)
        r = r.united(s.geometry);
    return r;
}

QString hex(const QByteArray &b) { return QString::fromLatin1(b.toHex()); }
QByteArray unhex(const QString &s) { return QByteArray::fromHex(s.toLatin1()); }
} // namespace

QCborMap ScreenInfo::toCbor() const
{
    QCborMap m;
    m.insert(QStringLiteral("i"), id);
    m.insert(QStringLiteral("n"), name);
    m.insert(QStringLiteral("x"), geometry.x());
    m.insert(QStringLiteral("y"), geometry.y());
    m.insert(QStringLiteral("w"), geometry.width());
    m.insert(QStringLiteral("h"), geometry.height());
    m.insert(QStringLiteral("s"), scale);
    m.insert(QStringLiteral("p"), primary);
    return m;
}

ScreenInfo ScreenInfo::fromCbor(const QCborMap &m)
{
    ScreenInfo s;
    s.id = m.value(QStringLiteral("i")).toString().left(128);
    s.name = m.value(QStringLiteral("n")).toString().left(128);
    s.geometry = QRect(int(m.value(QStringLiteral("x")).toInteger()), int(m.value(QStringLiteral("y")).toInteger()),
                       int(std::clamp<qint64>(m.value(QStringLiteral("w")).toInteger(), 1, 100000)),
                       int(std::clamp<qint64>(m.value(QStringLiteral("h")).toInteger(), 1, 100000)));
    s.scale = m.value(QStringLiteral("s")).toDouble(1.0);
    s.primary = m.value(QStringLiteral("p")).toBool();
    return s;
}

Workspace::Workspace(Node *node, const QString &configDir, QObject *parent)
    : QObject(parent), m_node(node), m_path(QDir(configDir).filePath(QStringLiteral("workspace.json")))
{
    load();
    for (const PeerView &p : m_node->peers()) {
        if (p.trusted && p.allowView)
            m_viewGrants.insert(p.id);
    }
    connect(m_node, &Node::peerConnected, this, &Workspace::onPeerConnected);
    connect(m_node, &Node::peerDisconnected, this, &Workspace::onPeerDisconnected);
    connect(m_node, &Node::messageReceived, this, &Workspace::onMessage);
    connect(m_node, &Node::peersChanged, this, &Workspace::onPeersChanged);
}

// --- Local updates -----------------------------------------------------------------------------

void Workspace::setLocalScreens(const QList<ScreenInfo> &screens)
{
    const QByteArray self = m_node->id();
    if (m_screens.value(self) == screens)
        return;
    m_screens.insert(self, screens.mid(0, kMaxScreens));
    // Drop thumbnails of screens that disappeared.
    auto &thumbs = m_thumbs[self];
    for (auto it = thumbs.begin(); it != thumbs.end();) {
        const bool exists = std::any_of(screens.begin(), screens.end(),
                                        [&](const ScreenInfo &s) { return s.id == it.key(); });
        it = exists ? std::next(it) : thumbs.erase(it);
    }
    save();
    for (const PeerView &p : m_node->peers()) {
        if (p.connected)
            sendScreens(p.id);
    }
    emit changed();
}

void Workspace::setLocalThumbnail(const QString &screenId, const QByteArray &jpeg)
{
    const QByteArray self = m_node->id();
    if (jpeg.size() > kMaxThumbnailBytes || m_thumbs.value(self).value(screenId) == jpeg)
        return;
    m_thumbs[self].insert(screenId, jpeg);
    QCborMap msg;
    msg.insert(kT, QStringLiteral("thumb"));
    msg.insert(QStringLiteral("s"), screenId);
    msg.insert(QStringLiteral("d"), jpeg);
    // Unicast to the granted peers that recently asked, never to everyone.
    const qint64 now = nowMs();
    const auto requests = m_thumbRequests;
    for (auto it = requests.constBegin(); it != requests.constEnd(); ++it) {
        if (now - it.value() <= kThumbRequestWindowMs && m_node->isViewAllowed(it.key())
            && m_node->isConnected(it.key()))
            m_node->sendMessage(it.key(), msg);
    }
    emit thumbnailChanged(self, screenId);
}

void Workspace::requestRemoteThumbnails()
{
    QCborMap msg;
    msg.insert(kT, QStringLiteral("thumbReq"));
    broadcast(msg);
}

void Workspace::setLayout(const QMap<QByteArray, QPoint> &offsets)
{
    m_layout.stamp = Stamp{m_layout.stamp.version + 1, m_node->id()};
    m_layout.offsets = offsets; // remote-view flags are kept
    save();
    broadcast(layoutMessage());
    emit changed();
}

void Workspace::setRemoteView(const QByteArray &deviceId, bool on)
{
    if (isRemoteView(deviceId) == on)
        return;
    m_layout.stamp = Stamp{m_layout.stamp.version + 1, m_node->id()};
    // Keep the current (possibly default) placement so the device stays where the user sees it.
    if (m_layout.offsets.isEmpty())
        m_layout.offsets = effectiveLayout();
    if (on)
        m_layout.remoteView.insert(deviceId);
    else
        m_layout.remoteView.remove(deviceId);
    save();
    broadcast(layoutMessage());
    emit changed();
}

void Workspace::setSharing(bool share)
{
    const QByteArray self = m_node->id();
    if (!share && m_master.masterId != self)
        return; // only the master can stop sharing
    if (share && m_master.masterId == self)
        return;
    m_master.stamp = Stamp{m_master.stamp.version + 1, self};
    m_master.masterId = share ? self : QByteArray();
    save();
    broadcast(masterMessage());
    emit changed();
}

// --- Queries -----------------------------------------------------------------------------------

QByteArray Workspace::thumbnail(const QByteArray &deviceId, const QString &screenId) const
{
    return m_thumbs.value(deviceId).value(screenId);
}

QList<QByteArray> Workspace::devices() const
{
    QList<QByteArray> out;
    const QByteArray self = m_node->id();
    if (!m_screens.value(self).isEmpty())
        out.append(self);
    QList<QByteArray> others;
    for (const PeerView &p : m_node->peers()) {
        if (p.trusted && !m_screens.value(p.id).isEmpty())
            others.append(p.id);
    }
    std::sort(others.begin(), others.end());
    return out + others;
}

QMap<QByteArray, QPoint> Workspace::effectiveLayout() const
{
    QMap<QByteArray, QPoint> result;
    QRect placed;
    QList<QByteArray> unplaced;
    const QList<QByteArray> ids = devices();
    for (const QByteArray &id : ids) {
        auto it = m_layout.offsets.constFind(id);
        if (it == m_layout.offsets.constEnd()) {
            unplaced.append(id);
            continue;
        }
        result.insert(id, *it);
        placed = placed.united(bounds(m_screens.value(id)).translated(*it));
    }
    // New devices go to the right of what is already placed, top-aligned.
    for (const QByteArray &id : unplaced) {
        const QRect local = bounds(m_screens.value(id));
        const QPoint offset = placed.isNull() ? QPoint(0, 0)
                                              : QPoint(placed.right() + 1 - local.left(), placed.top() - local.top());
        result.insert(id, offset);
        placed = placed.united(local.translated(offset));
    }
    return result;
}

// --- Replication -------------------------------------------------------------------------------

QCborMap Workspace::layoutMessage() const
{
    QCborArray packs;
    for (auto it = m_layout.offsets.constBegin(); it != m_layout.offsets.constEnd(); ++it) {
        QCborMap p;
        p.insert(QStringLiteral("i"), it.key());
        p.insert(QStringLiteral("x"), it->x());
        p.insert(QStringLiteral("y"), it->y());
        packs.append(p);
    }
    QCborMap msg;
    msg.insert(kT, QStringLiteral("layout"));
    msg.insert(QStringLiteral("v"), qint64(m_layout.stamp.version));
    msg.insert(QStringLiteral("a"), m_layout.stamp.author);
    msg.insert(QStringLiteral("p"), packs);
    QCborArray remoteView;
    for (const QByteArray &id : m_layout.remoteView)
        remoteView.append(id);
    msg.insert(QStringLiteral("rv"), remoteView);
    return msg;
}

QCborMap Workspace::masterMessage() const
{
    QCborMap msg;
    msg.insert(kT, QStringLiteral("master"));
    msg.insert(QStringLiteral("e"), qint64(m_master.stamp.version));
    msg.insert(QStringLiteral("a"), m_master.stamp.author);
    msg.insert(QStringLiteral("m"), m_master.masterId);
    return msg;
}

void Workspace::broadcast(const QCborMap &msg, const QByteArray &except)
{
    for (const PeerView &p : m_node->peers()) {
        if (p.connected && p.id != except)
            m_node->sendMessage(p.id, msg);
    }
}

void Workspace::sendScreens(const QByteArray &peerId)
{
    QCborArray list;
    for (const ScreenInfo &s : m_screens.value(m_node->id()))
        list.append(s.toCbor());
    QCborMap msg;
    msg.insert(kT, QStringLiteral("screens"));
    msg.insert(QStringLiteral("s"), list);
    m_node->sendMessage(peerId, msg);
}

void Workspace::sendThumbnails(const QByteArray &peerId)
{
    const auto thumbs = m_thumbs.value(m_node->id());
    for (auto it = thumbs.constBegin(); it != thumbs.constEnd(); ++it) {
        QCborMap msg;
        msg.insert(kT, QStringLiteral("thumb"));
        msg.insert(QStringLiteral("s"), it.key());
        msg.insert(QStringLiteral("d"), it.value());
        m_node->sendMessage(peerId, msg);
    }
}

void Workspace::handleThumbnailRequest(const QByteArray &peerId)
{
    // Remembered even when refused, so a grant given later answers it.
    m_thumbRequests.insert(peerId, nowMs());
    if (!m_node->isViewAllowed(peerId)) {
        QCborMap msg;
        msg.insert(kT, QStringLiteral("thumbDenied"));
        m_node->sendMessage(peerId, msg);
        return;
    }
    sendThumbnails(peerId); // cached ones right away, fresh ones once captured
    emit localThumbnailsRequested();
}

void Workspace::dropRemoteThumbnails(const QByteArray &peerId)
{
    const auto thumbs = m_thumbs.take(peerId);
    for (auto it = thumbs.constBegin(); it != thumbs.constEnd(); ++it)
        emit thumbnailChanged(peerId, it.key());
}

void Workspace::onPeersChanged()
{
    // Source side: tell peers whose grant was revoked to drop our pixels; answer pending requests
    // of newly granted ones.
    QSet<QByteArray> granted;
    for (const PeerView &p : m_node->peers()) {
        if (p.trusted && p.allowView)
            granted.insert(p.id);
    }
    for (const QByteArray &id : m_viewGrants - granted) {
        m_thumbRequests.remove(id);
        if (m_node->isConnected(id) && m_node->isTrusted(id)) {
            QCborMap msg;
            msg.insert(kT, QStringLiteral("thumbDenied"));
            m_node->sendMessage(id, msg);
        }
    }
    const QSet<QByteArray> added = granted - m_viewGrants;
    m_viewGrants = granted;
    for (const QByteArray &id : added) {
        if (m_thumbRequests.contains(id) && m_node->isConnected(id))
            handleThumbnailRequest(id);
    }

    // Viewer side: forget the pixels of devices that are no longer paired.
    const QByteArray self = m_node->id();
    for (const QByteArray &id : m_thumbs.keys()) {
        if (id != self && !m_node->isTrusted(id))
            dropRemoteThumbnails(id);
    }
    for (auto it = m_thumbDenied.begin(); it != m_thumbDenied.end();)
        it = m_node->isTrusted(*it) ? std::next(it) : m_thumbDenied.erase(it);
    emit changed();
}

void Workspace::onPeerDisconnected(const QByteArray &peerId)
{
    m_thumbRequests.remove(peerId);
    // Viewer side: its pixels are not kept past the connection (a revoke or unpair may never reach
    // us); they are asked for again on reconnection.
    dropRemoteThumbnails(peerId);
}

void Workspace::onPeerConnected(const QByteArray &peerId)
{
    sendScreens(peerId);
    // Ask for its thumbnails; it answers only if it granted us view.
    QCborMap thumbReq;
    thumbReq.insert(kT, QStringLiteral("thumbReq"));
    m_node->sendMessage(peerId, thumbReq);
    if (m_layout.stamp.version > 0)
        m_node->sendMessage(peerId, layoutMessage());
    if (m_master.stamp.version > 0)
        m_node->sendMessage(peerId, masterMessage());
}

void Workspace::onMessage(const QByteArray &peerId, const QCborMap &msg)
{
    const QString type = msg.value(kT).toString();
    if (type == QLatin1String("screens")) {
        QList<ScreenInfo> list;
        for (const QCborValue &v : msg.value(QStringLiteral("s")).toArray()) {
            if (list.size() < kMaxScreens)
                list.append(ScreenInfo::fromCbor(v.toMap()));
        }
        if (m_screens.value(peerId) != list) {
            m_screens.insert(peerId, list);
            // Drop thumbnails of screens that disappeared.
            auto it = m_thumbs.find(peerId);
            if (it != m_thumbs.end()) {
                for (const QString &screenId : it->keys()) {
                    const bool exists = std::any_of(list.begin(), list.end(),
                                                    [&](const ScreenInfo &s) { return s.id == screenId; });
                    if (!exists) {
                        it->remove(screenId);
                        emit thumbnailChanged(peerId, screenId);
                    }
                }
            }
            save();
            emit changed();
        }
    } else if (type == QLatin1String("thumb")) {
        const QString screenId = msg.value(QStringLiteral("s")).toString().left(128);
        const QByteArray jpeg = msg.value(QStringLiteral("d")).toByteArray();
        // Only for screens the peer announced (which bounds the count), and of a bounded size.
        const QList<ScreenInfo> screens = m_screens.value(peerId);
        const bool known = std::any_of(screens.begin(), screens.end(),
                                       [&](const ScreenInfo &s) { return s.id == screenId; });
        if (known && !jpeg.isEmpty() && jpeg.size() <= kMaxThumbnailBytes) {
            m_thumbs[peerId].insert(screenId, jpeg);
            emit thumbnailChanged(peerId, screenId);
            if (m_thumbDenied.remove(peerId))
                emit changed();
        }
    } else if (type == QLatin1String("thumbReq")) {
        handleThumbnailRequest(peerId);
    } else if (type == QLatin1String("thumbDenied")) {
        dropRemoteThumbnails(peerId);
        if (!m_thumbDenied.contains(peerId)) {
            m_thumbDenied.insert(peerId);
            emit changed();
        }
    } else if (type == QLatin1String("layout")) {
        LayoutState incoming;
        incoming.stamp = Stamp{quint64(msg.value(QStringLiteral("v")).toInteger()),
                               msg.value(QStringLiteral("a")).toByteArray()};
        for (const QCborValue &v : msg.value(QStringLiteral("p")).toArray()) {
            const QCborMap p = v.toMap();
            incoming.offsets.insert(p.value(QStringLiteral("i")).toByteArray(),
                                    QPoint(int(p.value(QStringLiteral("x")).toInteger()),
                                           int(p.value(QStringLiteral("y")).toInteger())));
        }
        for (const QCborValue &v : msg.value(QStringLiteral("rv")).toArray())
            incoming.remoteView.insert(v.toByteArray());
        if (incoming.stamp.newerThan(m_layout.stamp)) {
            m_layout = incoming;
            save();
            broadcast(layoutMessage(), peerId);
            emit changed();
        }
    } else if (type == QLatin1String("master")) {
        MasterState incoming;
        incoming.stamp = Stamp{quint64(msg.value(QStringLiteral("e")).toInteger()),
                               msg.value(QStringLiteral("a")).toByteArray()};
        incoming.masterId = msg.value(QStringLiteral("m")).toByteArray();
        if (incoming.stamp.newerThan(m_master.stamp)) {
            m_master = incoming;
            save();
            broadcast(masterMessage(), peerId);
            emit changed();
        }
    }
}

// --- Persistence -------------------------------------------------------------------------------

void Workspace::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    m_layout.stamp = Stamp{quint64(layout.value(QStringLiteral("version")).toInteger()),
                           unhex(layout.value(QStringLiteral("author")).toString())};
    const QJsonObject packs = layout.value(QStringLiteral("packs")).toObject();
    for (auto it = packs.begin(); it != packs.end(); ++it) {
        const QJsonArray xy = it->toArray();
        m_layout.offsets.insert(unhex(it.key()), QPoint(xy.at(0).toInt(), xy.at(1).toInt()));
    }
    for (const QJsonValue &v : layout.value(QStringLiteral("remoteView")).toArray())
        m_layout.remoteView.insert(unhex(v.toString()));

    const QJsonObject master = root.value(QStringLiteral("master")).toObject();
    m_master.stamp = Stamp{quint64(master.value(QStringLiteral("version")).toInteger()),
                           unhex(master.value(QStringLiteral("author")).toString())};
    m_master.masterId = unhex(master.value(QStringLiteral("device")).toString());

    const QJsonObject screens = root.value(QStringLiteral("screens")).toObject();
    for (auto it = screens.begin(); it != screens.end(); ++it) {
        QList<ScreenInfo> list;
        for (const QJsonValue &v : it->toArray())
            list.append(ScreenInfo::fromCbor(QCborMap::fromJsonObject(v.toObject())));
        m_screens.insert(unhex(it.key()), list);
    }
}

void Workspace::save() const
{
    QJsonObject packs;
    for (auto it = m_layout.offsets.constBegin(); it != m_layout.offsets.constEnd(); ++it)
        packs.insert(hex(it.key()), QJsonArray{it->x(), it->y()});
    QJsonObject layout;
    layout.insert(QStringLiteral("version"), qint64(m_layout.stamp.version));
    layout.insert(QStringLiteral("author"), hex(m_layout.stamp.author));
    layout.insert(QStringLiteral("packs"), packs);
    QJsonArray remoteView;
    for (const QByteArray &id : m_layout.remoteView)
        remoteView.append(hex(id));
    layout.insert(QStringLiteral("remoteView"), remoteView);

    QJsonObject master;
    master.insert(QStringLiteral("version"), qint64(m_master.stamp.version));
    master.insert(QStringLiteral("author"), hex(m_master.stamp.author));
    master.insert(QStringLiteral("device"), hex(m_master.masterId));

    // Remote screens are cached so offline devices can still be shown and arranged.
    QJsonObject screens;
    for (auto it = m_screens.constBegin(); it != m_screens.constEnd(); ++it) {
        QJsonArray list;
        for (const ScreenInfo &s : it.value())
            list.append(s.toCbor().toJsonObject());
        screens.insert(hex(it.key()), list);
    }

    QJsonObject root;
    root.insert(QStringLiteral("layout"), layout);
    root.insert(QStringLiteral("master"), master);
    root.insert(QStringLiteral("screens"), screens);
    QSaveFile f(m_path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson());
        f.commit();
    }
}

} // namespace df
