#include "modeltypes.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>

#include <algorithm>
#include <limits>

namespace {
QString text(const QCborMap &map, const char *key) { return map.value(QLatin1String(key)).toString(); }
bool flag(const QCborMap &map, const char *key) { return map.value(QLatin1String(key)).toBool(); }
qint64 integer(const QCborMap &map, const char *key) { return map.value(QLatin1String(key)).toInteger(); }
} // namespace

PeerInfo PeerInfo::fromCbor(const QCborMap &map)
{
    PeerInfo p;
    p.id = text(map, "id");
    p.name = text(map, "name");
    p.os = text(map, "os");
    p.fingerprint = text(map, "fingerprint");
    p.address = text(map, "address");
    p.port = int(integer(map, "port"));
    p.viaNat = flag(map, "viaNat");
    p.trusted = flag(map, "trusted");
    p.connected = flag(map, "connected");
    p.exploring = flag(map, "exploring");
    p.allowView = flag(map, "allowView");
    p.trust = text(map, "trust");
    p.effectiveTrust = text(map, "effectiveTrust");
    p.declaredTrust = text(map, "declaredTrust");
    p.version = text(map, "version");
    p.update = text(map, "update");
    p.swapCtrlCmd = flag(map, "swapCtrlCmd");
    p.protocol = int(integer(map, "protocol"));
    p.protocolMismatch = text(map, "protocolMismatch");
    p.canServeUpdate = flag(map, "canServeUpdate");
    return p;
}

QList<PeerInfo> ProtocolAlerts::fresh(const QList<PeerInfo> &peers)
{
    QList<PeerInfo> out;
    for (const PeerInfo &p : peers) {
        if (p.protocolMismatch.isEmpty() || p.id.isEmpty())
            continue;
        const QString key = p.id + QLatin1Char('/') + QString::number(p.protocol);
        if (m_told.contains(key))
            continue;
        m_told.insert(key);
        out.append(p);
    }
    return out;
}

DeviceScreen DeviceScreen::fromCbor(const QCborMap &map)
{
    DeviceScreen s;
    s.i = text(map, "i");
    s.n = text(map, "n");
    s.x = int(integer(map, "x"));
    s.y = int(integer(map, "y"));
    s.w = int(integer(map, "w"));
    s.h = int(integer(map, "h"));
    s.s = map.value(QLatin1String("s")).toDouble();
    s.p = flag(map, "p");
    return s;
}

DeviceInfo DeviceInfo::fromCbor(const QCborMap &map)
{
    DeviceInfo d;
    d.id = text(map, "id");
    d.name = text(map, "name");
    d.self = flag(map, "self");
    d.connected = flag(map, "connected");
    d.remoteView = flag(map, "remoteView");
    d.trust = text(map, "trust");
    d.guestThere = flag(map, "guestThere");
    d.viewPlan = map.value(QLatin1String("viewPlan")).toMap().toVariantMap(); // an older service sends none
    d.thumbsDenied = flag(map, "thumbsDenied");
    d.x = int(integer(map, "x"));
    d.y = int(integer(map, "y"));
    for (const QCborValue &v : map.value(QLatin1String("screens")).toArray())
        d.screens.append(DeviceScreen::fromCbor(v.toMap()));
    return d;
}

QRectF DeviceInfo::bounds() const
{
    // As the arrangement editor computed it in JavaScript: Math.min and Math.max from infinities.
    constexpr double inf = std::numeric_limits<double>::infinity();
    double l = inf, t = inf, r = -inf, b = -inf;
    for (const DeviceScreen &s : screens) {
        l = std::min(l, double(s.x));
        t = std::min(t, double(s.y));
        r = std::max(r, double(s.x) + s.w);
        b = std::max(b, double(s.y) + s.h);
    }
    return QRectF(l, t, r - l, b - t);
}

QStringList DeviceInfo::screenIds() const
{
    QStringList ids;
    for (const DeviceScreen &s : screens)
        ids.append(s.i);
    return ids;
}

DeviceScreen DeviceInfo::screen(const QString &screenId) const
{
    for (const DeviceScreen &s : screens) {
        if (s.i == screenId)
            return s;
    }
    return {};
}

QStringList DeviceInfo::editablePlan(const QStringList &localScreenIds) const
{
    QStringList plan;
    for (const DeviceScreen &s : screens) {
        const QString v = placement(s.i);
        plan.append(v == QLatin1String("-") || localScreenIds.contains(v) ? v : QString());
    }
    return plan;
}

TransferInfo TransferInfo::fromCbor(const QCborMap &map)
{
    TransferInfo t;
    t.id = integer(map, "id");
    t.peer = text(map, "peer");
    t.peerName = text(map, "peerName");
    t.kind = text(map, "kind");
    t.state = text(map, "state");
    t.error = text(map, "error");
    t.done = double(integer(map, "done"));
    t.total = double(integer(map, "total"));
    t.files = int(integer(map, "files"));
    t.fileCount = int(integer(map, "fileCount"));
    t.target = text(map, "target");
    // An older service does not say: its transfers the agent is shown can be cancelled.
    t.cancellable = map.contains(QStringLiteral("cancellable")) ? map.value(QStringLiteral("cancellable")).toBool()
                                                                : t.state == QLatin1String("running");
    return t;
}

PortsInfo PortsInfo::fromCbor(const QCborMap &map)
{
    PortsInfo p;
    p.data = int(integer(map, "data"));
    p.discovery = int(integer(map, "discovery"));
    p.phone = int(integer(map, "phone"));
    p.dataInUse = int(integer(map, "dataInUse"));
    p.tcpInUse = int(integer(map, "tcpInUse"));
    p.discoveryInUse = int(integer(map, "discoveryInUse"));
    p.phoneInUse = int(integer(map, "phoneInUse"));
    return p;
}
