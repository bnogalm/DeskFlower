#include "inputcontroller.h"

#include "clock.h"
#include "node.h"
#include "wire.h"
#include "workspace.h"

#include <QLoggingCategory>

#include <cstdlib>
#include <limits>

Q_LOGGING_CATEGORY(lcInput, "deskflower.input")

namespace df {

namespace {
constexpr quint8 kMotionDatagram = 1;
constexpr int kPingMs = 1000;
// After coming back through an edge, edge hits on that same edge are ignored for a moment so
// that hand jitter right after the crossing does not bounce the input straight back.
constexpr int kReturnGuardMs = 250;

const QString kX = QStringLiteral("x");
const QString kY = QStringLiteral("y");
const QString kB = QStringLiteral("b");
const QString kD = QStringLiteral("d");
const QString kS = QStringLiteral("s");

int i(const QCborMap &m, const QString &k) { return int(m.value(k).toInteger()); }

Side opposite(Side s)
{
    switch (s) {
    case Side::Left: return Side::Right;
    case Side::Right: return Side::Left;
    case Side::Top: return Side::Bottom;
    case Side::Bottom: return Side::Top;
    }
    return s;
}

// One pixel further in the direction of travel through `crossed`: arriving exactly on the facing
// edge would let a single pixel of jitter (or a motionless report) cross straight back.
QPoint inset(QPoint p, Side crossed)
{
    switch (crossed) {
    case Side::Right: return p + QPoint(1, 0);
    case Side::Left: return p - QPoint(1, 0);
    case Side::Bottom: return p + QPoint(0, 1);
    case Side::Top: return p - QPoint(0, 1);
    }
    return p;
}
} // namespace

InputController::InputController(Node *node, Workspace *workspace, QObject *parent)
    : QObject(parent), m_node(node), m_workspace(workspace), m_active(node->id())
{
    m_catchTimer.setSingleShot(true);
    connect(&m_catchTimer, &QTimer::timeout, this, [this] {
        if (m_pendingCrossing) {
            qCInfo(lcInput) << "No answer about a drag: crossing as usual";
            finishCatch(false);
        }
    });
    connect(m_workspace, &Workspace::changed, this, &InputController::rebuild);
    connect(m_node, &Node::messageReceived, this, &InputController::onPeerMessage);
    connect(m_node, &Node::datagramReceived, this, &InputController::onPeerDatagram);
    connect(m_node, &Node::peerDisconnected, this, [this](const QByteArray &peerId) {
        if (peerId == m_active)
            returnLocal(m_router.primaryCenter(m_node->id()), false);
        if (peerId == m_workspace->masterId())
            emitSession(QStringLiteral("releaseAll"));
    });

    m_ping.setInterval(kPingMs);
    connect(&m_ping, &QTimer::timeout, this, [this] { emitSession(QStringLiteral("ping")); });
    rebuild();
}

bool InputController::isMaster() const { return m_workspace->masterId() == m_node->id(); }

bool InputController::cursorHere(qint64 graceMs) const
{
    return !m_cursorAway || nowMs() - m_cursorLeftAt < graceMs;
}

void InputController::emitSession(const QString &ev, QCborMap args)
{
    if (!m_sessionUp)
        return;
    args.insert(QStringLiteral("ev"), ev);
    emit toSession(args);
}

void InputController::sessionConnected()
{
    m_sessionUp = true;
    m_ping.start();
    m_wasMaster = isMaster();
    QCborMap config;
    config.insert(QStringLiteral("master"), m_wasMaster);
    emitSession(QStringLiteral("config"), config);
}

void InputController::sessionDisconnected()
{
    m_sessionUp = false;
    m_ping.stop();
    if (m_active != m_node->id())
        returnLocal(m_router.primaryCenter(m_node->id()), true);
    // Slave: nothing moves the cursor here any more, so the master's would be stuck on this device.
    if (m_masterHere) {
        m_masterHere = false;
        refuseInput(m_workspace->masterId());
    }
}

void InputController::returnHome()
{
    if (m_active == m_node->id())
        return;
    qCInfo(lcInput) << "This device locked or switched sessions: input back home";
    returnLocal(m_router.primaryCenter(m_node->id()), true);
}

void InputController::rebuild()
{
    QHash<QByteArray, QList<ScreenInfo>> screens;
    for (const QByteArray &id : m_workspace->devices())
        screens.insert(id, m_workspace->screens(id));
    m_router.set(screens, m_workspace->effectiveLayout());

    if (isMaster() != m_wasMaster) {
        m_wasMaster = isMaster();
        QCborMap config;
        config.insert(QStringLiteral("master"), m_wasMaster);
        emitSession(QStringLiteral("config"), config);
    }

    // Losing the master role while controlling another device hands the input back.
    if (!isMaster() && m_active != m_node->id())
        returnLocal(m_router.primaryCenter(m_node->id()), true);
    else if (m_active != m_node->id())
        keepAWayHome();
}

void InputController::keepAWayHome()
{
    const QByteArray self = m_node->id();
    if (!m_router.canReach(m_active, self)) {
        qCInfo(lcInput) << "No way back from" << m_active.toHex() << "(its screens changed): input back home";
        returnLocal(m_router.primaryCenter(self), true);
        return;
    }
    if (m_viewing) {
        if (!m_router.screens(m_active).contains(m_viewRemote)) {
            qCInfo(lcInput) << "The viewed screen of" << m_active.toHex() << "is gone: back home";
            returnLocal(m_viewHome, true);
        }
        return;
    }
    if (!m_router.contains(m_active, m_cursor)) {
        // Its screen went away or shrank: onto the nearest one that is left.
        m_cursor = m_router.clamp(m_active, m_cursor, m_cursor);
        sendMotion();
    }
}

// --- Master side -------------------------------------------------------------------------------

void InputController::handleSessionMessage(const QCborMap &msg)
{
    const QString cmd = msg.value(QStringLiteral("cmd")).toString();
    const QByteArray self = m_node->id();

    if (cmd == QLatin1String("edge")) {
        if (!isMaster() || m_active != self)
            return;
        const QPoint at(i(msg, kX), i(msg, kY));
        const Side side = Side(i(msg, kS));
        if (m_returnGuard.isValid() && side == m_returnEdge && m_returnGuard.elapsed() < kReturnGuardMs)
            return; // just came back through this edge
        const auto target = m_router.cross(self, at, side);
        if (!target || target->device == self || !m_node->isConnected(target->device))
            return;
        if (nowMs() < m_refusedUntil.value(target->device, -1))
            return; // it just refused the input
        if (m_pendingCrossing)
            return; // waiting to know what is dragged
        if (msg.value(QStringLiteral("lb")).toBool() && !m_dragging) {
            const bool view = m_workspace->isRemoteView(target->device);
            if (!view || nowMs() >= m_viewRetryAfter.value(target->device, -1)) {
                beginCatch(self, at, PendingCrossing{false, *target, side, at, view});
                return;
            }
        }
        if (m_workspace->isRemoteView(target->device)) {
            // A cursor resting on the edge would otherwise ask again and again after a failure.
            if (nowMs() < m_viewRetryAfter.value(target->device, -1))
                return;
            enterView(target->device, at, side);
        }
        else
            enter(*target, side);
        return;
    }
    if (cmd == QLatin1String("captureLost")) {
        // The helper could not keep the keyboard and mouse: they are local again, so is the input.
        if (m_active != self) {
            qCInfo(lcInput) << "The session helper lost the capture: input back home";
            returnLocal(QPoint(i(msg, kX), i(msg, kY)), true);
        }
        return;
    }
    if (cmd == QLatin1String("dragRelease")) {
        if (m_dragging && m_active == self)
            drop(self, QPoint(i(msg, kX), i(msg, kY)));
        return;
    }
    if (m_active == self)
        return; // not capturing: stale event

    if (cmd == QLatin1String("mm")) {
        if (!m_pendingCrossing)
            moveBy(i(msg, kX), i(msg, kY));
    } else if (cmd == QLatin1String("mb")) {
        const int button = i(msg, kB);
        const bool down = msg.value(kD).toBool();
        if (!down && button == 1 && m_dragging) {
            drop(m_active, m_cursor);
            return;
        }
        // A release whose press the device never got (pressed before crossing) is not sent.
        if (down)
            m_remoteButtons.insert(button);
        else if (!m_remoteButtons.remove(button))
            return;
        QCborMap m;
        m.insert(QStringLiteral("t"), QStringLiteral("in.btn"));
        m.insert(kB, button);
        m.insert(kD, down);
        m.insert(kX, m_cursor.x());
        m.insert(kY, m_cursor.y());
        sendToActive(m);
    } else if (cmd == QLatin1String("mw")) {
        QCborMap m;
        m.insert(QStringLiteral("t"), QStringLiteral("in.wheel"));
        m.insert(kX, i(msg, kX));
        m.insert(kY, i(msg, kY));
        sendToActive(m);
    } else if (cmd == QLatin1String("key")) {
        QCborMap m;
        m.insert(QStringLiteral("t"), QStringLiteral("in.key"));
        m.insert(kS, i(msg, kS));
        m.insert(kD, msg.value(kD).toBool());
        sendToActive(m);
    }
}

void InputController::enter(const Crossing &target, Side side)
{
    const QByteArray self = m_node->id();
    const bool wasLocal = m_active == self;
    const QByteArray previous = m_active;
    m_remoteButtons.clear(); // in.leave releases them there
    if (!wasLocal) {
        QCborMap leave;
        leave.insert(QStringLiteral("t"), QStringLiteral("in.leave"));
        sendToActive(leave);
    }
    m_active = target.device;
    m_cursor = target.point;
    const QPoint inside = inset(m_cursor, side);
    if (m_router.contains(m_active, inside))
        m_cursor = inside;
    qCInfo(lcInput) << "Input ->" << target.device.toHex() << m_cursor;
    if (wasLocal) {
        QCborMap capture;
        capture.insert(QStringLiteral("on"), true);
        emitSession(QStringLiteral("capture"), capture);
    }
    QCborMap m;
    m.insert(QStringLiteral("t"), QStringLiteral("in.enter"));
    m.insert(kX, m_cursor.x());
    m.insert(kY, m_cursor.y());
    sendToActive(m);
    if (wasLocal) {
        m_cursorAway = true;
        m_cursorLeftAt = nowMs();
    }
    m_cursorFrom = previous;
    emit activeDeviceChanged(m_active);
    if (!m_viewing || m_viewAccepted)
        emit cursorMoved(previous, m_active);
}

void InputController::enterView(const QByteArray &device, QPoint edgePoint, Side side)
{
    const QRect monitor = m_router.screenAt(m_node->id(), edgePoint);
    const QRect remote = m_router.primaryRect(device);
    if (monitor.isEmpty() || remote.isEmpty())
        return;
    // Same relative position along the edge: the remote screen replaces the local monitor.
    const bool horizontal = side == Side::Left || side == Side::Right;
    const double f = horizontal ? double(edgePoint.y() - monitor.top()) / qMax(1, monitor.height() - 1)
                                : double(edgePoint.x() - monitor.left()) / qMax(1, monitor.width() - 1);
    QPoint entry;
    switch (side) {
    case Side::Right: entry = QPoint(remote.left(), remote.top() + qRound(f * (remote.height() - 1))); break;
    case Side::Left: entry = QPoint(remote.right(), remote.top() + qRound(f * (remote.height() - 1))); break;
    case Side::Bottom: entry = QPoint(remote.left() + qRound(f * (remote.width() - 1)), remote.top()); break;
    case Side::Top: entry = QPoint(remote.left() + qRound(f * (remote.width() - 1)), remote.bottom()); break;
    }
    m_viewing = true;
    m_viewAccepted = false; // enter() sends nothing to the device until it accepts
    m_viewMonitor = monitor;
    m_viewRemote = remote;
    m_viewSide = side;
    m_viewHome = edgePoint;
    if (monitor.contains(inset(edgePoint, opposite(side))))
        m_viewHome = inset(edgePoint, opposite(side));
    enter(Crossing{device, entry}, side);

    QCborMap open;
    open.insert(QStringLiteral("device"), QString::fromLatin1(device.toHex()));
    open.insert(kX, monitor.x());
    open.insert(kY, monitor.y());
    open.insert(QStringLiteral("w"), monitor.width());
    open.insert(QStringLiteral("h"), monitor.height());
    open.insert(QStringLiteral("rw"), remote.width());
    open.insert(QStringLiteral("rh"), remote.height());
    emitSession(QStringLiteral("viewOpen"), open);
    emit viewRequested(device, monitor.size(), remote);
    if (m_viewing) // may have failed straight away (cancelView)
        sendViewCursor();
}

void InputController::setViewAccepted(const QByteArray &device)
{
    if (!m_viewing || m_active != device || m_viewAccepted)
        return;
    m_viewAccepted = true;
    QCborMap m;
    m.insert(QStringLiteral("t"), QStringLiteral("in.enter"));
    m.insert(kX, m_cursor.x());
    m.insert(kY, m_cursor.y());
    sendToActive(m);
    sendViewCursor();
    emit cursorMoved(m_cursorFrom, device);
}

void InputController::cancelView()
{
    if (!m_viewing)
        return;
    qCInfo(lcInput) << "Remote view of" << m_active.toHex() << "failed: back home";
    m_viewRetryAfter.insert(m_active, nowMs() + kViewRetryMs);
    armReturnGuard(m_viewSide);
    returnLocal(m_viewHome, true);
}

void InputController::sendViewCursor()
{
    QCborMap c;
    c.insert(kX, m_cursor.x() - m_viewRemote.left());
    c.insert(kY, m_cursor.y() - m_viewRemote.top());
    emitSession(QStringLiteral("viewCursor"), c);
}

QRect InputController::neighbourScreen(const QRect &r, QPoint next) const
{
    const int sx = next.x() > r.right() ? 1 : next.x() < r.left() ? -1 : 0;
    const int sy = next.y() > r.bottom() ? 1 : next.y() < r.top() ? -1 : 0;
    const QPoint across = QPoint(qBound(r.left(), next.x(), r.right()), qBound(r.top(), next.y(), r.bottom()))
                          + QPoint(sx, sy);
    QRect best;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    for (const QRect &s : m_router.screens(m_active)) {
        if (s == r)
            continue;
        // It must touch the crossed edge: hold `across` along the crossing direction.
        if ((sx && (across.x() < s.left() || across.x() > s.right()))
            || (sy && (across.y() < s.top() || across.y() > s.bottom())))
            continue;
        const QPoint inside(qBound(s.left(), across.x(), s.right()), qBound(s.top(), across.y(), s.bottom()));
        const qint64 distance = qint64(qAbs(inside.x() - across.x())) + qAbs(inside.y() - across.y());
        if (distance < bestDistance) {
            bestDistance = distance;
            best = s;
        }
    }
    return best;
}

void InputController::moveInView(int dx, int dy)
{
    const QPoint next = m_cursor + QPoint(dx, dy);
    const QRect &r = m_viewRemote;
    if (!r.contains(next)) {
        // Past an edge of the shown screen, onto another screen of the same device: show that one.
        // The pixel just across the edge decides (a fast move does not skip the neighbour); screens
        // of different heights or widths still connect where they are nearest.
        const QRect other = neighbourScreen(r, next);
        if (!other.isEmpty()) {
            m_viewRemote = other;
            m_cursor = QPoint(qBound(other.left(), next.x(), other.right()), qBound(other.top(), next.y(), other.bottom()));
            QCborMap size;
            size.insert(QStringLiteral("rw"), other.width());
            size.insert(QStringLiteral("rh"), other.height());
            emitSession(QStringLiteral("viewScreen"), size);
            emit viewScreenChanged(m_active, other);
            sendMotion();
            sendViewCursor();
            return;
        }
    }
    // Leaving through the side we came in (the opposite of the local edge) returns home.
    bool back = false;
    double f = 0;
    switch (m_viewSide) {
    case Side::Right: back = next.x() < r.left(); f = double(next.y() - r.top()) / qMax(1, r.height() - 1); break;
    case Side::Left: back = next.x() > r.right(); f = double(next.y() - r.top()) / qMax(1, r.height() - 1); break;
    case Side::Bottom: back = next.y() < r.top(); f = double(next.x() - r.left()) / qMax(1, r.width() - 1); break;
    case Side::Top: back = next.y() > r.bottom(); f = double(next.x() - r.left()) / qMax(1, r.width() - 1); break;
    }
    if (back) {
        f = qBound(0.0, f, 1.0);
        const QRect &m = m_viewMonitor;
        QPoint home;
        switch (m_viewSide) {
        case Side::Right: home = QPoint(m.right(), m.top() + qRound(f * (m.height() - 1))); break;
        case Side::Left: home = QPoint(m.left(), m.top() + qRound(f * (m.height() - 1))); break;
        case Side::Bottom: home = QPoint(m.left() + qRound(f * (m.width() - 1)), m.bottom()); break;
        case Side::Top: home = QPoint(m.left() + qRound(f * (m.width() - 1)), m.top()); break;
        }
        // Back home through the side opposite to the one crossed on the way out.
        const Side crossed = opposite(m_viewSide);
        if (m.contains(inset(home, crossed)))
            home = inset(home, crossed);
        armReturnGuard(m_viewSide);
        if (m_remoteButtons.contains(1) && !m_dragging) {
            // Leaving the viewed device with the button held: maybe dragging files from it.
            beginCatch(m_active, m_cursor, PendingCrossing{true, {}, m_viewSide, home});
            return;
        }
        returnLocal(home, true);
        return;
    }
    const QPoint clamped(qBound(r.left(), next.x(), r.right()), qBound(r.top(), next.y(), r.bottom()));
    if (clamped != m_cursor) {
        m_cursor = clamped;
        sendMotion();
        sendViewCursor();
    }
}

void InputController::returnLocal(QPoint at, bool notifyRemote)
{
    const QByteArray self = m_node->id();
    if (m_active == self)
        return;
    const QByteArray previous = m_active;
    // A remote view the device never accepted: the cursor was never there.
    const bool wasThere = !m_viewing || m_viewAccepted;
    m_remoteButtons.clear();
    if (m_viewing) {
        m_viewing = false;
        emitSession(QStringLiteral("viewClose"));
        emit viewEnded(m_active);
    }
    if (notifyRemote) {
        QCborMap leave;
        leave.insert(QStringLiteral("t"), QStringLiteral("in.leave"));
        sendToActive(leave);
    }
    m_active = self;
    m_cursor = at;
    qCInfo(lcInput) << "Input -> local" << at;
    QCborMap capture;
    capture.insert(QStringLiteral("on"), false);
    capture.insert(kX, at.x());
    capture.insert(kY, at.y());
    emitSession(QStringLiteral("capture"), capture);
    m_cursorAway = false;
    emit activeDeviceChanged(m_active);
    if (wasThere)
        emit cursorMoved(previous, self);
}

void InputController::moveBy(int dx, int dy)
{
    if (m_pendingCrossing)
        return; // waiting to know what is dragged
    if (m_viewing) {
        moveInView(dx, dy);
        return;
    }
    const QPoint next = m_cursor + QPoint(dx, dy);
    if (m_router.contains(m_active, next)) {
        m_cursor = next;
        sendMotion();
        return;
    }
    const QPoint clamped = m_router.clamp(m_active, m_cursor, next);
    const int overX = next.x() - clamped.x();
    const int overY = next.y() - clamped.y();
    Side side;
    if (std::abs(overX) >= std::abs(overY))
        side = overX > 0 ? Side::Right : Side::Left;
    else
        side = overY > 0 ? Side::Bottom : Side::Top;

    if (overX != 0 || overY != 0) {
        const auto target = m_router.cross(m_active, clamped, side);
        const bool catchDrag = target && !m_dragging && m_remoteButtons.contains(1)
                               && (target->device == m_node->id() || m_node->isConnected(target->device));
        if (target && target->device == m_node->id()) {
            QPoint at = inset(target->point, side);
            if (!m_router.contains(target->device, at))
                at = target->point;
            armReturnGuard(opposite(side));
            if (catchDrag) {
                m_cursor = clamped;
                beginCatch(m_active, clamped, PendingCrossing{true, *target, side, at});
                return;
            }
            returnLocal(at, true);
            return;
        }
        if (target && m_node->isConnected(target->device)
            && nowMs() >= m_refusedUntil.value(target->device, -1)) {
            if (catchDrag) {
                m_cursor = clamped;
                beginCatch(m_active, clamped, PendingCrossing{false, *target, side, {}});
                return;
            }
            enter(*target, side);
            return;
        }
    }
    if (clamped != m_cursor) {
        m_cursor = clamped;
        sendMotion();
    }
}

void InputController::beginCatch(const QByteArray &source, QPoint at, const PendingCrossing &crossing)
{
    qCInfo(lcInput) << "Crossing with the button held on" << source.toHex() << ": dragging files?";
    m_pendingCrossing = crossing;
    m_catchSource = source;
    m_catchTimer.start(kDragCatchTimeoutMs);
    emit dragCatchRequested(source, at);
}

void InputController::dragCaught(const QByteArray &source, bool files)
{
    if (!m_pendingCrossing || source != m_catchSource)
        return;
    finishCatch(files);
}

void InputController::finishCatch(bool files)
{
    m_catchTimer.stop();
    const PendingCrossing crossing = *m_pendingCrossing;
    m_pendingCrossing.reset();
    if (files) {
        m_dragging = true;
        m_dragSource = m_catchSource;
        QCborMap on;
        on.insert(QStringLiteral("on"), true);
        emitSession(QStringLiteral("dragWatch"), on); // our own button's release ends it locally
    }
    if (crossing.toLocal)
        returnLocal(crossing.point, true);
    else if (crossing.view && m_node->isConnected(crossing.target.device))
        enterView(crossing.target.device, crossing.point, crossing.side);
    else if (m_node->isConnected(crossing.target.device))
        enter(crossing.target, crossing.side);
    if (files)
        m_remoteButtons.remove(1); // released there by the catch; the drag goes on here
}

void InputController::drop(const QByteArray &target, QPoint at)
{
    m_dragging = false;
    QCborMap off;
    off.insert(QStringLiteral("on"), false);
    emitSession(QStringLiteral("dragWatch"), off);
    qCInfo(lcInput) << "Files dropped on" << target.toHex() << at;
    emit dragDropped(target, at);
}

void InputController::armReturnGuard(Side localEdge)
{
    m_returnEdge = localEdge;
    m_returnGuard.start();
}

void InputController::sendMotion()
{
    if (m_viewing && !m_viewAccepted)
        return;
    QByteArray d;
    d.append(char(kMotionDatagram));
    wire::appendU32(d, ++m_motionSeq);
    wire::appendU32(d, quint32(m_cursor.x()));
    wire::appendU32(d, quint32(m_cursor.y()));
    m_node->sendDatagram(m_active, d);
}

void InputController::sendToActive(const QCborMap &msg)
{
    if (m_viewing && !m_viewAccepted)
        return; // the device has not accepted the view yet
    if (m_active != m_node->id())
        m_node->sendMessage(m_active, msg);
}

// --- Slave side --------------------------------------------------------------------------------

void InputController::setRemoteInputBlocked(const QByteArray &peerId, bool on)
{
    if (on) {
        m_blockedPeer = peerId;
        emitSession(QStringLiteral("releaseAll"));
    } else if (m_blockedPeer == peerId) {
        m_blockedPeer.clear();
    }
}

void InputController::onPeerMessage(const QByteArray &peerId, const QCborMap &msg)
{
    const QString type = msg.value(QStringLiteral("t")).toString();
    if (!type.startsWith(QLatin1String("in.")))
        return;
    if (type == QLatin1String("in.refused")) {
        // Master side: the device cannot take the input (nothing runs in its session to move its
        // cursor); the user would be stuck there.
        if (isMaster() && peerId == m_active) {
            qCInfo(lcInput) << peerId.toHex() << "cannot take the input: back home";
            m_refusedUntil.insert(peerId, nowMs() + kRefusedRetryMs);
            returnLocal(m_router.primaryCenter(m_node->id()), false);
            emit inputRefused(peerId);
        }
        return;
    }
    if (peerId != m_workspace->masterId() || peerId == m_node->id()) {
        qCDebug(lcInput) << "Ignoring" << type << "from" << peerId.toHex() << "(not the master)";
        return; // only the master may drive this device
    }
    if (!m_blockedPeer.isEmpty() && peerId == m_blockedPeer)
        return; // it cannot answer our user's prompt for them

    if (type == QLatin1String("in.enter")) {
        if (!m_sessionUp) {
            refuseInput(peerId);
            return;
        }
        m_masterHere = true;
        m_haveMotionSeq = false;
        m_cursorAway = false;
        QCborMap m;
        m.insert(kX, i(msg, kX));
        m.insert(kY, i(msg, kY));
        emitSession(QStringLiteral("mabs"), m);
    } else if (type == QLatin1String("in.leave")) {
        m_masterHere = false;
        m_cursorAway = true;
        m_cursorLeftAt = nowMs();
        emitSession(QStringLiteral("releaseAll"));
        emitSession(QStringLiteral("hideCursor")); // the pointer is on another device now
    } else if (type == QLatin1String("in.btn")) {
        QCborMap m;
        m.insert(kB, i(msg, kB));
        m.insert(kD, msg.value(kD).toBool());
        m.insert(kX, i(msg, kX));
        m.insert(kY, i(msg, kY));
        emitSession(QStringLiteral("mb"), m);
    } else if (type == QLatin1String("in.wheel")) {
        QCborMap m;
        m.insert(kX, i(msg, kX));
        m.insert(kY, i(msg, kY));
        emitSession(QStringLiteral("mw"), m);
    } else if (type == QLatin1String("in.key")) {
        QCborMap m;
        m.insert(kS, i(msg, kS));
        m.insert(kD, msg.value(kD).toBool());
        emitSession(QStringLiteral("key"), m);
    }
}

void InputController::refuseInput(const QByteArray &master)
{
    if (master.isEmpty() || master == m_node->id() || !m_node->isConnected(master))
        return;
    qCInfo(lcInput) << "Nothing can move the cursor in this session: input refused";
    QCborMap m;
    m.insert(QStringLiteral("t"), QStringLiteral("in.refused"));
    m_node->sendMessage(master, m);
}

void InputController::onPeerDatagram(const QByteArray &peerId, const QByteArray &data)
{
    if (data.size() < 13 || quint8(data[0]) != kMotionDatagram || peerId != m_workspace->masterId()
        || (!m_blockedPeer.isEmpty() && peerId == m_blockedPeer))
        return;
    const quint32 seq = wire::readU32(data, 1);
    if (m_haveMotionSeq && qint32(seq - m_lastMotionSeq) <= 0)
        return; // older than what we already applied
    m_haveMotionSeq = true;
    m_lastMotionSeq = seq;
    QCborMap m;
    m.insert(kX, qint32(wire::readU32(data, 5)));
    m.insert(kY, qint32(wire::readU32(data, 9)));
    emitSession(QStringLiteral("mabs"), m);
}

} // namespace df
