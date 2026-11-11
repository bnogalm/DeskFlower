#include "inputcontroller.h"

#include "clock.h"
#include "keymap.h"
#include "node.h"
#include "wire.h"
#include "workspace.h"

#include <QLoggingCategory>

#include <algorithm>
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
QPoint inset(QPoint p, Side crossed, int depth = 1)
{
    switch (crossed) {
    case Side::Right: return p + QPoint(depth, 0);
    case Side::Left: return p - QPoint(depth, 0);
    case Side::Bottom: return p + QPoint(0, depth);
    case Side::Top: return p - QPoint(0, depth);
    }
    return p;
}

QList<int> indicesOf(const QList<ViewScreen> &shown)
{
    QList<int> out;
    for (const ViewScreen &v : shown)
        out.append(v.index);
    return out;
}
} // namespace

InputController::InputController(Node *node, Workspace *workspace, QObject *parent)
    : QObject(parent), m_node(node), m_workspace(workspace), m_active(node->id())
{
    m_catchTimer.setSingleShot(true);
    connect(&m_catchTimer, &QTimer::timeout, this, [this] {
        if (m_pendingCrossing) {
            qCInfo(lcInput) << "No answer about a drag: crossing as usual";
            // Files were found but listing them took too long: the drag the source had is over
            // (the catch ended it), so the user must hear that nothing was moved.
            const bool listing = m_catchExtended;
            const QByteArray source = m_catchSource;
            finishCatch(false);
            if (listing)
                emit dragCatchTimedOut(source);
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
        pushEdgeMap();
    });
    connect(m_node, &Node::peerConnected, this, &InputController::pushEdgeMap);
    // The remote view switch (Node::setRemoteViewEnabled) changes which devices are viewed rather
    // than entered, and says so only through peersChanged.
    connect(m_node, &Node::peersChanged, this, &InputController::pushEdgeMap);
    // The master flagged as a guest while its cursor is here: nothing moves that cursor any more
    // (its messages are dropped from now on), so it is sent home at once, as when the helper goes,
    // rather than left stuck until it blindly finds an edge.
    connect(m_node, &Node::peersChanged, this, [this] {
        const QByteArray master = m_workspace->masterId();
        if (!m_masterHere || !m_node->isGuest(master))
            return;
        qCInfo(lcInput) << "The master is a guest now: its cursor goes home";
        m_masterHere = false;
        m_cursorAway = false;
        emitSession(QStringLiteral("releaseAll"));
        emitSession(QStringLiteral("hideCursor"));
        refuseInput(master);
    });
    m_edgeMapRetry.setSingleShot(true);
    connect(&m_edgeMapRetry, &QTimer::timeout, this, &InputController::pushEdgeMap);

    m_ping.setInterval(kPingMs);
    connect(&m_ping, &QTimer::timeout, this, [this] { emitSession(QStringLiteral("ping")); });
    rebuild();
}

bool InputController::isMaster() const { return m_workspace->masterId() == m_node->id(); }

bool InputController::cursorHere(qint64 graceMs) const
{
    return !m_cursorAway || nowMs() - m_cursorLeftAt < graceMs;
}

bool InputController::cursorLeftRecently(qint64 graceMs) const
{
    return m_cursorAway && nowMs() - m_cursorLeftAt < graceMs;
}

bool InputController::pasteOrderedOn(const QByteArray &device, qint64 windowMs) const
{
    return m_lastPasteAt >= 0 && m_lastPasteDevice == device && nowMs() - m_lastPasteAt < windowMs;
}

void InputController::notePasteGesture()
{
    m_lastPasteGestureAt = nowMs();
    m_pasteGestures.push_back(m_lastPasteGestureAt);
    while (!m_pasteGestures.empty()
           && (m_pasteGestures.size() > kPasteGestureHistoryMax
               || m_pasteGestures.front() < m_lastPasteGestureAt - kPasteGestureHistoryMs))
        m_pasteGestures.pop_front();
    emit pasteGesture();
}

bool InputController::pasteGestureBetween(qint64 from, qint64 to) const
{
    for (auto it = m_pasteGestures.rbegin(); it != m_pasteGestures.rend(); ++it) {
        if (*it < from)
            return false; // the ones before it are older still
        if (to < 0 || *it <= to)
            return true;
    }
    return false;
}

void InputController::noteLocalPasteGesture() { notePasteGesture(); }

bool InputController::inputOn(const QByteArray &device, qint64 windowMs) const
{
    return m_lastInputAt >= 0 && m_lastInputDevice == device && nowMs() - m_lastInputAt < windowMs;
}

bool InputController::inputArrived(qint64 windowMs) const
{
    return m_lastMasterPressAt >= 0 && nowMs() - m_lastMasterPressAt < windowMs;
}

void InputController::notePress(bool pasteChord)
{
    m_lastInputDevice = m_active;
    m_lastInputAt = nowMs();
    if (pasteChord) {
        m_lastPasteDevice = m_active;
        m_lastPasteAt = m_lastInputAt;
    }
}

namespace {
// Control and Command (sent as the Windows key's codes) trade places (setSwapCtrlCmd).
quint32 swappedCtrlCmd(quint32 scancode)
{
    using namespace keymap::paste;
    switch (scancode) {
    case kLeftCtrl: return kLeftCmd;
    case kRightCtrl: return kRightCmd;
    case kLeftCmd: return kLeftCtrl;
    case kRightCmd: return kRightCtrl;
    default: return scancode;
    }
}

// Keys this device receives end up on a Mac here: Command+V pastes (keymap::isPasteChord).
#ifdef Q_OS_MACOS
constexpr bool kCmdPastesHere = true;
#else
constexpr bool kCmdPastesHere = false;
#endif
} // namespace

bool InputController::canEnter(const QByteArray &device) const
{
    return m_node->isConnected(device) && m_node->declaredTrust(device) != TrustLevel::Guest;
}

void InputController::noteBlocked(const QByteArray &device)
{
    const qint64 now = nowMs();
    auto it = m_blockedNoticeAt.constFind(device);
    if (it != m_blockedNoticeAt.constEnd() && now - *it < m_blockedNoticeMs)
        return;
    m_blockedNoticeAt.insert(device, now);
    qCInfo(lcInput) << "Crossing into" << device.toHex() << "blocked: it declared this device a guest";
    emit crossingBlocked(device);
}

void InputController::emitSession(const QString &ev, QCborMap args)
{
    if (ev == QLatin1String("releaseAll"))
        m_heldFromMaster.clear(); // the helper lets go of every key the master pressed here
    if (!m_sessionUp)
        return;
    if (ev == QLatin1String("mabs"))
        m_pendingMotion.reset(); // superseded
    else if (m_pendingMotion)
        flushMotion(); // what came before goes first
    args.insert(QStringLiteral("ev"), ev);
    emit toSession(args);
}

void InputController::sessionConnected()
{
    m_sessionUp = true;
    m_localPressSupported = false; // until this helper says so (older ones never do)
    m_heldFromMaster.clear();      // a new helper (maybe replacing one) holds nothing yet
    m_ping.start();
    m_wasMaster = isMaster();
    QCborMap config;
    config.insert(QStringLiteral("master"), m_wasMaster);
    emitSession(QStringLiteral("config"), config);
    m_lastEdgeMap.reset(); // a new helper knows nothing yet
    pushEdgeMap();
}

void InputController::sessionDisconnected()
{
    m_sessionUp = false;
    m_localPressSupported = false; // nothing reports local presses any more
    m_heldFromMaster.clear();
    m_ping.stop();
    m_pendingMotion.reset(); // nothing from this session reaches the next helper
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
    if (m_viewing)
        refreshViewPlan(); // the plan may have changed (Workspace::changed)
    pushEdgeMap();
}

void InputController::refreshViewPlan()
{
    QList<QRect> hidden;
    const QList<ViewEntry> entries = resolveViewPlan(m_active, m_viewCrossed, &hidden);
    const auto sameEntries = [&] {
        if (entries.size() != m_viewScreens.size() || hidden != m_viewHidden)
            return false;
        for (int k = 0; k < entries.size(); ++k) {
            const ViewEntry &a = entries.at(k), &b = m_viewScreens.at(k);
            if (a.index != b.index || a.remote != b.remote || a.monitor != b.monitor)
                return false;
        }
        return true;
    };
    // Never empty in practice: a plan hiding every screen is ignored (resolveViewPlan), and a
    // device left without screens has already ended the view (keepAWayHome, just before).
    if (entries.isEmpty() || sameEntries())
        return;
    // The screen with the cursor stays active if still visible; if the plan hides it now, the
    // cursor goes to the centre of the first visible one.
    const int activeIndex = m_viewScreens.at(m_viewActive).index;
    int active = 0;
    for (int k = 0; k < entries.size(); ++k) {
        if (entries.at(k).index == activeIndex)
            active = k;
    }
    const bool moved = entries.at(active).index != activeIndex;
    qCInfo(lcInput) << "The view plan of" << m_active.toHex() << "changed during the view: applied";
    m_viewScreens = entries;
    m_viewHidden = hidden;
    activateViewScreen(active);
    if (moved) {
        m_cursor = m_viewRemote.center();
        sendMotion();
    }
    const QList<ViewScreen> shown = displayedViewScreens();
    emitSession(QStringLiteral("viewShow"), viewShowArgs(shown));
    // Monitors may have changed for the same screens: RemoteView compares the sizes itself.
    m_viewShown = indicesOf(shown);
    emit viewScreensChanged(m_active, shown);
    sendViewCursor();
}

void InputController::pushEdgeMap()
{
    if (!m_sessionUp)
        return;
    QCborArray spans;
    qint64 nextRetry = -1; // when the earliest refusal that keeps a device off the map expires
    if (isMaster()) {
        const qint64 now = nowMs();
        for (const EdgeSpan &s : m_router.neighbourSpans(m_node->id())) {
            // A viewed device keeps the round trip: its viewer window needs it anyway.
            if (!canEnter(s.device) || viewsDevice(s.device))
                continue;
            const qint64 until = m_refusedUntil.value(s.device, -1);
            if (now < until) {
                if (nextRetry < 0 || until < nextRetry)
                    nextRetry = until;
                continue;
            }
            QCborMap m;
            m.insert(kS, int(s.side));
            m.insert(kX, s.screen.x());
            m.insert(kY, s.screen.y());
            m.insert(QStringLiteral("w"), s.screen.width());
            m.insert(QStringLiteral("h"), s.screen.height());
            m.insert(QStringLiteral("a"), s.from);
            m.insert(kB, s.to);
            spans.append(m);
        }
    }
    if (nextRetry >= 0) {
        const int delay = int(qMax<qint64>(0, nextRetry - nowMs())) + 1;
        if (!m_edgeMapRetry.isActive() || m_edgeMapRetry.remainingTime() > delay)
            m_edgeMapRetry.start(delay);
    }
    if (m_lastEdgeMap && *m_lastEdgeMap == spans)
        return;
    m_lastEdgeMap = spans;
    QCborMap edges;
    edges.insert(QStringLiteral("e"), spans);
    emitSession(QStringLiteral("edges"), edges);
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
        // The view was laid out for the screens of both devices as they were: any of them gone
        // (or changed) ends it, rather than leaving a window with nothing behind it.
        const QList<QRect> remote = m_router.screens(m_active), local = m_router.screens(self);
        const bool intact = std::all_of(m_viewScreens.begin(), m_viewScreens.end(), [&](const ViewEntry &e) {
            return remote.contains(e.remote) && local.contains(e.monitor);
        });
        if (!intact) {
            qCInfo(lcInput) << "The screens of the view of" << m_active.toHex() << "changed: back home";
            returnLocal(m_router.contains(self, m_viewHome) ? m_viewHome : m_router.primaryCenter(self), true);
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
        const QPoint at(i(msg, kX), i(msg, kY));
        const Side side = Side(i(msg, kS));
        // A helper that captured at the edge on its own (edges, see the header) is told when the
        // input stays here, so that the cursor reappears where it was. The reason usually lasts
        // (a drag being asked about, a guard, a map that is behind), and the cursor is still
        // pushed against the edge: the edge is guarded (ge) so that it does not capture again on
        // the very next move, which would make the cursor flicker.
        const bool captured = msg.value(QStringLiteral("c")).toBool();
        const auto decline = [this, captured, at, side] {
            if (!captured)
                return;
            QCborMap capture;
            capture.insert(QStringLiteral("on"), false);
            capture.insert(kX, at.x());
            capture.insert(kY, at.y());
            capture.insert(QStringLiteral("ge"), int(side));
            emitSession(QStringLiteral("capture"), capture);
        };
        if (!isMaster()) {
            decline();
            return;
        }
        // Already away: the helper was told to capture when the input left (enter), so one that
        // captured at the edge on its own meanwhile is in the state wanted; declining it would
        // free the mouse while the input is still routed elsewhere.
        if (m_active != self)
            return;
        if (m_returnGuard.isValid() && side == m_returnEdge && m_returnGuard.elapsed() < kReturnGuardMs) {
            decline();
            return; // just came back through this edge
        }
        const auto target = m_router.cross(self, at, side);
        if (!target || target->device == self || !canEnter(target->device)) {
            // A connected device that refuses this one as a guest: say why the cursor stops.
            if (target && target->device != self && m_node->isConnected(target->device))
                noteBlocked(target->device);
            decline();
            return;
        }
        if (nowMs() < m_refusedUntil.value(target->device, -1)) {
            decline();
            return; // it just refused the input (and inputRefused said so)
        }
        if (m_pendingCrossing) {
            decline();
            return; // waiting to know what is dragged
        }
        if (msg.value(QStringLiteral("lb")).toBool() && !m_dragging) {
            // (The helper does not capture early with the button held: the cursor stays in sight
            // on the edge while the drag is asked about.)
            const bool view = viewsDevice(target->device);
            if (!view || nowMs() >= m_viewRetryAfter.value(target->device, -1)) {
                beginCatch(self, at, PendingCrossing{false, *target, side, at, view});
                return;
            }
        }
        if (viewsDevice(target->device)) {
            // A cursor resting on the edge would otherwise ask again and again after a failure.
            if (nowMs() < m_viewRetryAfter.value(target->device, -1)) {
                decline();
                return;
            }
            enterView(*target, at, side);
            // Nothing to show (a failure that went through cancelView already let the cursor go).
            if (m_active == self && nowMs() >= m_viewRetryAfter.value(target->device, -1))
                decline();
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
    if (m_active == self) { // not capturing: stale event
        if (cmd == QLatin1String("key") && m_keyTrace)
            m_keyTrace({}, quint32(i(msg, kS)), msg.value(kD).toBool(), true); // not forwarded
        return;
    }

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
        if (down) {
            m_remoteButtons.insert(button);
            notePress(false);
        } else if (!m_remoteButtons.remove(button)) {
            return;
        }
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
        quint32 scancode = quint32(i(msg, kS));
        if (m_swapCtrlCmd.contains(m_active))
            scancode = swappedCtrlCmd(scancode);
        const bool down = msg.value(kD).toBool();
        // Watched on the way for the clipboard: a paste chord on the remote device is the user
        // asking for the clipboard there (autorepeat of the key counts as the same order).
        if (down) {
            notePress(keymap::isPasteChord(m_heldForwarded, scancode, m_activeIsMac));
            m_heldForwarded.insert(scancode);
        } else {
            m_heldForwarded.remove(scancode);
        }
        QCborMap m;
        m.insert(QStringLiteral("t"), QStringLiteral("in.key"));
        m.insert(kS, qint64(scancode));
        m.insert(kD, down);
        sendToActive(m);
        if (m_keyTrace) // an empty peer: dropped (the input is here, or a view is not accepted yet)
            m_keyTrace(m_active != m_node->id() && !(m_viewing && !m_viewAccepted) ? m_active : QByteArray(),
                       scancode, down, true);
    }
}

QPoint InputController::landing(const Crossing &target, Side side, int depth) const
{
    for (const int d : {depth, 1}) {
        const QPoint at = inset(target.point, side, d);
        if (m_router.contains(target.device, at))
            return at;
    }
    return target.point;
}

void InputController::enter(const Crossing &target, Side side, int depth)
{
    const QByteArray self = m_node->id();
    const bool wasLocal = m_active == self;
    const QByteArray previous = m_active;
    m_remoteButtons.clear(); // in.leave releases them there
    m_heldForwarded.clear();
    if (!wasLocal) {
        QCborMap leave;
        leave.insert(QStringLiteral("t"), QStringLiteral("in.leave"));
        sendToActive(leave);
    }
    m_active = target.device;
    m_activeIsMac = false;
    for (const PeerView &p : m_node->peers()) {
        if (p.id == m_active)
            m_activeIsMac = p.platform.startsWith(QLatin1String("macos"));
    }
    m_cursor = landing(target, side, depth);
    m_motionDirty = false; // in.enter carries the position
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

bool InputController::viewsDevice(const QByteArray &device) const
{
    return m_workspace->isRemoteView(device) && m_node->isRemoteViewEnabled();
}

bool InputController::sourceSupportsMultiView(const QByteArray &device) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == device)
            return peerSupportsMultiView(p.version);
    }
    return false;
}

QList<InputController::ViewEntry> InputController::resolveViewPlan(const QByteArray &device, const QRect &crossed,
                                                                   QList<QRect> *hiddenOut) const
{
    const QList<ScreenInfo> remote = m_workspace->screens(device);
    const QList<ScreenInfo> local = m_workspace->screens(m_node->id());
    const Workspace::ViewPlan plan = m_workspace->viewPlan(device);
    const auto hidden = [&](const ScreenInfo &s) { return plan.value(s.id) == QLatin1String(Workspace::kHiddenScreen); };
    // A plan hiding every screen would leave nothing to enter: it is ignored.
    const bool allHidden = !remote.isEmpty() && std::all_of(remote.begin(), remote.end(), hidden);
    QList<ViewEntry> entries;
    QList<QRect> monitors; // distinct ones, in the order they are first used
    for (int i = 0; i < remote.size(); ++i) {
        const ScreenInfo &s = remote.at(i);
        if (s.geometry.isEmpty())
            continue;
        if (!allHidden && hidden(s)) {
            if (hiddenOut)
                hiddenOut->append(s.geometry);
            continue;
        }
        ViewEntry e;
        e.index = i;
        e.remote = s.geometry;
        e.monitor = crossed; // unless the plan maps it to a monitor that is still there
        const QString wanted = allHidden ? QString() : plan.value(s.id);
        for (const ScreenInfo &m : local) {
            if (!wanted.isEmpty() && m.id == wanted && !m.geometry.isEmpty())
                e.monitor = m.geometry;
        }
        if (!monitors.contains(e.monitor)) {
            // Only so many screens are displayed at once: past the cap, a screen joins the first
            // monitor's rotation rather than opening one more window.
            if (monitors.size() >= kMaxViewScreens)
                e.monitor = monitors.first();
            else
                monitors.append(e.monitor);
        }
        // A monitor starts by showing the first screen assigned to it.
        e.shown = std::none_of(entries.begin(), entries.end(), [&](const ViewEntry &o) { return o.monitor == e.monitor; });
        entries.append(e);
    }
    return entries;
}

void InputController::activateViewScreen(int at)
{
    m_viewActive = at;
    const ViewEntry active = m_viewScreens.at(at); // a copy: the list is written below
    for (int k = 0; k < m_viewScreens.size(); ++k) {
        ViewEntry &e = m_viewScreens[k];
        if (e.monitor == active.monitor)
            e.shown = k == at;
    }
    m_viewMonitor = active.monitor;
    m_viewRemote = active.remote;
}

QList<ViewScreen> InputController::displayedViewScreens() const
{
    QList<ViewScreen> out;
    for (int k = 0; k < m_viewScreens.size(); ++k) {
        const ViewEntry &e = m_viewScreens.at(k);
        // A legacy source streams one screen at a time: the active one, on its monitor.
        if (m_viewMulti ? e.shown : k == m_viewActive)
            out.append(ViewScreen{e.index, e.remote, e.monitor});
    }
    return out;
}

QCborMap InputController::viewShowArgs(const QList<ViewScreen> &shown) const
{
    QCborArray screens;
    for (const ViewScreen &v : shown) {
        QCborMap m;
        m.insert(QStringLiteral("i"), v.index);
        m.insert(kX, v.monitor.x());
        m.insert(kY, v.monitor.y());
        m.insert(QStringLiteral("w"), v.monitor.width());
        m.insert(QStringLiteral("h"), v.monitor.height());
        m.insert(QStringLiteral("rw"), v.remote.width());
        m.insert(QStringLiteral("rh"), v.remote.height());
        screens.append(m);
    }
    QCborMap args;
    args.insert(QStringLiteral("screens"), screens);
    args.insert(QStringLiteral("active"), m_viewScreens.at(m_viewActive).index);
    return args;
}

void InputController::enterView(const Crossing &target, QPoint edgePoint, Side side)
{
    const QByteArray &device = target.device;
    const QRect monitor = m_router.screenAt(m_node->id(), edgePoint);
    if (monitor.isEmpty())
        return;
    QList<QRect> hidden;
    const QList<ViewEntry> entries = resolveViewPlan(device, monitor, &hidden);
    if (entries.isEmpty())
        return;
    // The screen the layout puts beyond the edge; if that one is hidden, the visible one nearest
    // to the crossing point; if the layout cannot tell, the primary one when visible.
    const QRect beyond = m_router.screenAt(device, target.point);
    const QRect primary = m_router.primaryRect(device);
    int active = -1;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    for (int k = 0; k < entries.size(); ++k) {
        const QRect &s = entries.at(k).remote;
        if (s == beyond || (beyond.isEmpty() && s == primary)) {
            active = k;
            break;
        }
        const QPoint inside(qBound(s.left(), target.point.x(), s.right()), qBound(s.top(), target.point.y(), s.bottom()));
        const qint64 distance = qint64(qAbs(inside.x() - target.point.x())) + qAbs(inside.y() - target.point.y());
        if (distance < bestDistance) {
            bestDistance = distance;
            active = k;
        }
    }
    const QRect remote = entries.at(active).remote;
    // Same relative position along the edge: the remote screen replaces the crossed monitor.
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
    m_viewCrossed = monitor;
    m_viewScreens = entries;
    m_viewHidden = hidden;
    activateViewScreen(active);
    m_viewMulti = sourceSupportsMultiView(device);
    m_viewSide = side;
    m_viewHome = edgePoint;
    if (monitor.contains(inset(edgePoint, opposite(side))))
        m_viewHome = inset(edgePoint, opposite(side));
    enter(Crossing{device, entry}, side);
    if (!m_viewing)
        return; // ended by a slot of enter() (the state above is gone with it)

    const QList<ViewScreen> shown = displayedViewScreens();
    m_viewShown = indicesOf(shown);
    QCborMap open = viewShowArgs(shown);
    open.insert(QStringLiteral("device"), QString::fromLatin1(device.toHex()));
    emitSession(QStringLiteral("viewOpen"), open);
    emit viewRequested(device, shown);
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
    c.insert(QStringLiteral("i"), m_viewScreens.at(m_viewActive).index);
    c.insert(kX, m_cursor.x() - m_viewRemote.left());
    c.insert(kY, m_cursor.y() - m_viewRemote.top());
    emitSession(QStringLiteral("viewCursor"), c);
}

int InputController::neighbourScreen(QPoint next) const
{
    const QRect &r = m_viewRemote;
    const int sx = next.x() > r.right() ? 1 : next.x() < r.left() ? -1 : 0;
    const int sy = next.y() > r.bottom() ? 1 : next.y() < r.top() ? -1 : 0;
    QPoint across = QPoint(qBound(r.left(), next.x(), r.right()), qBound(r.top(), next.y(), r.bottom()))
                    + QPoint(sx, sy);
    // Only visible screens (m_viewScreens) are landed on. A hidden one just across the edge is
    // crossed over (`across` moves to its far side) and the search goes on from there, so that a
    // visible screen behind it, maybe shown on a monitor of its own, is still reached.
    for (int passed = 0; passed <= m_viewHidden.size(); ++passed) {
        int best = -1;
        qint64 bestDistance = std::numeric_limits<qint64>::max();
        for (int k = 0; k < m_viewScreens.size(); ++k) {
            if (k == m_viewActive)
                continue;
            const QRect &s = m_viewScreens.at(k).remote;
            // It must touch the crossed edge: hold `across` along the crossing direction.
            if ((sx && (across.x() < s.left() || across.x() > s.right()))
                || (sy && (across.y() < s.top() || across.y() > s.bottom())))
                continue;
            const QPoint inside(qBound(s.left(), across.x(), s.right()), qBound(s.top(), across.y(), s.bottom()));
            const qint64 distance = qint64(qAbs(inside.x() - across.x())) + qAbs(inside.y() - across.y());
            if (distance < bestDistance) {
                bestDistance = distance;
                best = k;
            }
        }
        if (best >= 0)
            return best;
        bool skipped = false;
        for (const QRect &h : m_viewHidden) {
            if (!h.contains(across))
                continue;
            across = QPoint(sx > 0 ? h.right() + 1 : sx < 0 ? h.left() - 1 : across.x(),
                            sy > 0 ? h.bottom() + 1 : sy < 0 ? h.top() - 1 : across.y());
            skipped = true;
            break;
        }
        if (!skipped)
            return -1;
    }
    return -1;
}

void InputController::moveInView(int dx, int dy)
{
    const QPoint next = m_cursor + QPoint(dx, dy);
    if (!m_viewRemote.contains(next)) {
        // Past an edge of the active screen, onto another visible screen of the same device: the
        // cursor goes there, and that screen's monitor shows it. The pixel just across the edge
        // decides (a fast move does not skip the neighbour); screens of different heights or
        // widths still connect where they are nearest.
        const int other = neighbourScreen(next);
        if (other >= 0) {
            activateViewScreen(other);
            const QRect &r = m_viewRemote;
            m_cursor = QPoint(qBound(r.left(), next.x(), r.right()), qBound(r.top(), next.y(), r.bottom()));
            const QList<ViewScreen> shown = displayedViewScreens();
            emitSession(QStringLiteral("viewShow"), viewShowArgs(shown));
            // The service only cares when a monitor shows another screen (the stream changes).
            if (indicesOf(shown) != m_viewShown) {
                m_viewShown = indicesOf(shown);
                emit viewScreensChanged(m_active, shown);
            }
            sendMotion();
            sendViewCursor();
            return;
        }
    }
    const QRect &r = m_viewRemote;
    // Leaving through the side we came in (the opposite of the local edge) returns home, onto the
    // monitor showing the active screen (m_viewMonitor follows it).
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
    m_heldForwarded.clear();
    if (m_viewing) {
        m_viewing = false;
        m_viewScreens.clear();
        m_viewHidden.clear();
        m_viewShown.clear();
        m_viewActive = -1;
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
    if (m_returnGuard.isValid() && m_returnGuard.elapsed() < kReturnGuardMs)
        capture.insert(QStringLiteral("ge"), int(m_returnEdge)); // no early capture there for a moment
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
        // The motion past the edge goes on beyond it (a fast or coalesced move would otherwise
        // stop at the edge for a moment): the first pixel past it is the target's edge.
        const int depth = qMax(1, std::abs(side == Side::Left || side == Side::Right ? overX : overY) - 1);
        const auto target = m_router.cross(m_active, clamped, side);
        const bool catchDrag = target && !m_dragging && m_remoteButtons.contains(1)
                               && (target->device == m_node->id() || canEnter(target->device));
        if (target && target->device == m_node->id()) {
            const QPoint at = landing(*target, side, depth);
            armReturnGuard(opposite(side));
            if (catchDrag) {
                m_cursor = clamped;
                beginCatch(m_active, clamped, PendingCrossing{true, *target, side, at});
                return;
            }
            returnLocal(at, true);
            return;
        }
        if (target && canEnter(target->device) && nowMs() >= m_refusedUntil.value(target->device, -1)) {
            if (catchDrag) {
                m_cursor = clamped;
                beginCatch(m_active, clamped, PendingCrossing{false, *target, side, {}});
                return;
            }
            enter(*target, side, depth);
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
    m_catchExtended = false;
    m_catchTimer.start(kDragCatchTimeoutMs);
    emit dragCatchRequested(source, at);
}

void InputController::dragCaught(const QByteArray &source, bool files)
{
    if (!m_pendingCrossing || source != m_catchSource)
        return;
    finishCatch(files);
}

void InputController::dragCatchPending(const QByteArray &source)
{
    if (!m_pendingCrossing || source != m_catchSource || m_catchExtended)
        return;
    m_catchExtended = true;
    qCInfo(lcInput) << "The dragged files are still being listed: waiting longer";
    m_catchTimer.start(kDragCatchExtendedMs - kDragCatchTimeoutMs);
}

void InputController::finishCatch(bool files)
{
    m_catchTimer.stop();
    m_catchExtended = false;
    const PendingCrossing crossing = *m_pendingCrossing;
    m_pendingCrossing.reset();
    const bool canCross = crossing.toLocal
                          || (crossing.view ? m_node->isConnected(crossing.target.device) : canEnter(crossing.target.device));
    if (!canCross) {
        if (files) {
            // The catch ended the drag on the source and nothing can take it now: end it there
            // rather than leave a drag (and the helper's release watch) on with nowhere to drop.
            m_dragSource = m_catchSource;
            m_remoteButtons.remove(1);
            qCInfo(lcInput) << "Files dragged towards a device that takes no input from here";
            emit dragRefused(crossing.target.device);
            emit dragDropped(m_catchSource, {});
        }
        return;
    }
    if (files) {
        m_dragging = true;
        m_dragSource = m_catchSource;
        QCborMap on;
        on.insert(QStringLiteral("on"), true);
        emitSession(QStringLiteral("dragWatch"), on); // our own button's release ends it locally
    }
    if (crossing.toLocal)
        returnLocal(crossing.point, true);
    else if (crossing.view)
        enterView(crossing.target, crossing.point, crossing.side);
    else
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

// Only the latest position matters, so the moves read together from the helper (it sends up to
// one per hook event) make one datagram per pass of the event loop instead of one each, which a
// slow device would have to decrypt one by one. Whatever must stay in order with the motion
// (buttons, keys, leaving the device) flushes it first: sendToActive.
void InputController::sendMotion()
{
    if (m_motionDirty)
        return;
    m_motionDirty = true;
    QMetaObject::invokeMethod(this, &InputController::flushMotionOut, Qt::QueuedConnection);
}

void InputController::flushMotionOut()
{
    if (!m_motionDirty)
        return;
    m_motionDirty = false;
    if (m_active == m_node->id() || (m_viewing && !m_viewAccepted))
        return; // the input came home meanwhile / the device has not accepted the view yet
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
    if (m_active != m_node->id()) {
        flushMotionOut();
        m_node->sendMessage(m_active, msg);
    }
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
            pushEdgeMap(); // off the helper's map until the pause is over
            emit inputRefused(peerId);
        }
        return;
    }
    const auto ignoredKey = [&](const QString &why) {
        if (m_ignoredKeyTrace && type == QLatin1String("in.key"))
            m_ignoredKeyTrace(peerId, quint32(i(msg, kS)), msg.value(kD).toBool(), why);
    };
    if (peerId != m_workspace->masterId() || peerId == m_node->id()) {
        qCDebug(lcInput) << "Ignoring" << type << "from" << peerId.toHex() << "(not the master)";
        ignoredKey(QStringLiteral("not the master here"));
        return; // only the master may drive this device
    }
    if (!m_blockedPeer.isEmpty() && peerId == m_blockedPeer) {
        ignoredKey(QStringLiteral("its view request awaits an answer here"));
        return; // it cannot answer our user's prompt for them
    }
    if (peerId != m_heldFromMasterId) {
        // Another master now: the keys the previous one held here are not this one's.
        m_heldFromMaster.clear();
        m_heldFromMasterId = peerId;
    }
    if (m_node->isGuest(peerId)) {
        ignoredKey(QStringLiteral("a guest"));
        // A guest never drives this device; tell it, so its cursor does not stay stuck here.
        if (type == QLatin1String("in.enter")) {
            qCInfo(lcInput) << "Refusing the input of guest" << peerId.toHex();
            refuseInput(peerId);
        }
        return;
    }

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
        m_heldFromMaster.clear();
        m_cursorAway = true;
        m_cursorLeftAt = nowMs();
        emitSession(QStringLiteral("releaseAll"));
        emitSession(QStringLiteral("hideCursor")); // the pointer is on another device now
    } else if (type == QLatin1String("in.btn")) {
        if (msg.value(kD).toBool()) {
            m_lastMasterPressAt = nowMs();
            notePasteGesture(); // any button press: a click on Paste, Retry, a menu...
        }
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
        const quint32 scancode = quint32(i(msg, kS));
        if (msg.value(kD).toBool()) {
            m_lastMasterPressAt = nowMs();
            // Judged on the codes this device gets (after the master's Control/Command swap).
            if (keymap::isPasteEvidenceKey(m_heldFromMaster, scancode, kCmdPastesHere))
                notePasteGesture();
            m_heldFromMaster.insert(scancode);
        } else {
            m_heldFromMaster.remove(scancode);
        }
        if (m_keyTrace)
            m_keyTrace(peerId, quint32(i(msg, kS)), msg.value(kD).toBool(), false);
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
    if (!m_sessionUp)
        return; // nothing here to move the cursor (in.enter is refused)
    if (data.size() < 13 || quint8(data[0]) != kMotionDatagram || peerId != m_workspace->masterId()
        || (!m_blockedPeer.isEmpty() && peerId == m_blockedPeer) || m_node->isGuest(peerId))
        return;
    const quint32 seq = wire::readU32(data, 1);
    if (m_haveMotionSeq && qint32(seq - m_lastMotionSeq) <= 0)
        return; // older than what we already applied
    m_haveMotionSeq = true;
    m_lastMotionSeq = seq;
    m_pendingMotion = QPoint(qint32(wire::readU32(data, 5)), qint32(wire::readU32(data, 9)));
    if (!m_motionFlushQueued) {
        m_motionFlushQueued = true;
        QMetaObject::invokeMethod(this, &InputController::flushMotion, Qt::QueuedConnection);
    }
}

void InputController::flushMotion()
{
    m_motionFlushQueued = false;
    if (!m_pendingMotion)
        return;
    const QPoint at = *m_pendingMotion;
    QCborMap m;
    m.insert(kX, at.x());
    m.insert(kY, at.y());
    emitSession(QStringLiteral("mabs"), m); // clears m_pendingMotion
}

} // namespace df
