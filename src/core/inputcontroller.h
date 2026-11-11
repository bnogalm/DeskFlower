#pragma once

// Routes keyboard and mouse between devices.
//
// The local session helper (deskflower-session) talks to this class through CBOR maps:
//   helper -> controller: edge {s, x, y, lb, c}  cursor pushes past an outer edge of the local
//                                          desktop (lb: left button held; c: already capturing)
//                         mm {x, y}        relative motion while captured
//                         mb {b, d}        button (1 left, 2 right, 3 middle, 4 x1, 5 x2), d = down
//                         mw {x, y}        wheel (120 units per notch)
//                         key {s, d}       PS/2 set-1 scancode (0xE0xx for extended keys)
//                         captureLost {x, y}  capture ended on its own (input is local again)
//   controller -> helper: config {master}  capture {on, x, y, ge}  releaseAll  ping
//                         edges {e: [{s, x, y, w, h, a, b}]}
//                         mabs {x, y}  mb {b, d, x, y}  mw {x, y}  key {s, d}  hideCursor
//
// Capturing at the edge: while the service decides on an edge hit, the OS holds the cursor on the
// edge and whatever is moved meanwhile is lost. So the master's helper is told in advance where a
// push past an edge will cross (edges: for each stretch, the side s, the screen x, y, w, h and
// the along-edge coordinates a..b): a connected device that did not just refuse the input, did not
// declare this device a guest, and is entered rather than viewed. On such a stretch the helper
// captures at once and reports the edge
// with c = true, and the motion that follows (mm) is applied after the crossing. The controller
// then either crosses (its capture {on} is a no-op for the helper) or, when it does not (return
// guard, device gone or refused meanwhile, drag being asked about), answers capture {on: false}
// at the edge point, so the cursor reappears where it was, with that edge as ge: the helper does
// not capture there again for a moment (kReturnGuardMs), since the reason usually lasts and the
// cursor is still pushed against the edge. A return through an edge carries ge too, and neither
// side crosses there again for a moment. An early capture reported while the input is already
// away (the crossing went through a plain report meanwhile) is simply kept. Helpers without the
// map (older ones, other platforms) never send c and work as before.
//
// On the master the controller keeps a virtual cursor in the active device's native coordinates
// and sends absolute positions, so pointer acceleration is applied only once (by the master's OS).
// Between devices (reliable channel): in.enter {x, y}, in.leave, in.btn {b, d, x, y},
// in.wheel {x, y}, in.key {s, d}. Pointer motion goes as unreliable datagrams
// [kind u8 = 1][seq u32][x i32][y i32] (latest wins), one per pass of the master's event loop:
// moves read together from the helper make one datagram.
// Motion is absolute and only the latest position matters: a slave that falls behind (busy CPU)
// applies the newest of the positions waiting for it, not each one in turn, and anything else it
// receives (buttons, keys) still comes after the moves that preceded it.
//
// A device only obeys input coming from the current master, and never from a guest
// (Node::isGuest), which is refused like a device without a session. One that cannot take it (nothing runs
// in its session to move its cursor) answers in.refused, and the master takes its input back.
// A device that declared this one a guest (Node::declaredTrust) is known to refuse: it is never
// entered nor put on the helper's edge map, so the cursor is not captured and thrown back every
// few seconds; it comes back once it declares otherwise (peersChanged). The cursor stopping at
// such an edge is explained to the user (crossingBlocked, at most once per kBlockedNoticeMs per
// device). A device that refused the input says so itself (inputRefused); pushing towards it
// during the pause that follows (kRefusedRetryMs) adds nothing to that notice.
//
// There is always a way home: when the screens change while the cursor is on another device (a
// monitor goes off, a resolution or the layout changes) and no edge leads back any more, or that
// device has no screens left, the input comes back to the master.
//
// Remote view (devices flagged in the layout): instead of moving to the device's screens, its
// screens are shown full screen on local monitors. The device's view plan (Workspace::viewPlan)
// says where: a remote screen mapped to a local monitor is shown there, one absent from the plan
// on the monitor where the edge was crossed, and a hidden one (kHiddenScreen) is never shown nor
// entered (its edge is blocked). Screens on different monitors are shown at the same time; those
// sharing a monitor one at a time, the monitor showing the last one the cursor was on. The view
// starts on the screen the layout puts beyond the crossed edge (the nearest visible one if that
// one is hidden). A plan changed while the view is open applies at once (rebuild): the screens
// are shown where it now says, and if the screen with the cursor is now hidden the cursor moves
// to the first visible one. Pushing the cursor past an edge of its screen towards another visible screen of
// that device moves it there; leaving through the side it came in returns home, onto the monitor
// showing that screen. Helper events: viewOpen {device, screens: [{i, x, y, w, h (local monitor),
// rw, rh (remote screen size)}], active: i}, viewShow {screens, active} (the displayed set or the
// active screen changed), viewCursor {i, x, y} (relative to screen i), viewClose (the service adds
// viewStatus {device, state, text}). i indexes the device's screens as the workspace lists them.
// A source older than kMultiViewVersion streams one screen at a time: the displayed set is then
// just the active screen. Nothing reaches the remote device (not even in.enter) until its user
// accepts the view (setViewAccepted), so nobody can click its approval prompt blindly. A failed
// view (cancelView) returns home and the same device is not entered again for a moment. With
// remote view off on this device (Node::isRemoteViewEnabled) flagged devices are entered as usual.
//
// Dragging files between devices: crossing an edge with the left button held (helper: edge {lb}
// for the master's own button, the buttons forwarded for a remote device) holds the crossing back
// until the device the drag started on says whether files are being dragged (dragCatchRequested ->
// dragCaught, see FileDrag; dragCatchPending extends the wait while a large tree is listed); the
// crossing then goes ahead, or, if the target takes no input from here any more, the drag ends at
// its source (dragRefused, dragDropped on the source). Releasing the button afterwards, on
// whichever device the cursor is, drops them there (dragDropped). The master's helper reports
// releases of its own button while a drag is on (dragWatch {on} -> dragRelease {x, y}).
// While a device's user is being asked, its input from the requester is ignored
// (setRemoteInputBlocked).

#include "router.h"
#include "viewtypes.h"

#include <QCborArray>
#include <QCborMap>
#include <QHash>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QSize>

#include <deque>
#include <functional>
#include <optional>
#include <QTimer>

namespace df {

class Node;
class Workspace;

class InputController : public QObject
{
    Q_OBJECT
public:
    InputController(Node *node, Workspace *workspace, QObject *parent = nullptr);

    void sessionConnected();
    void sessionDisconnected();
    void handleSessionMessage(const QCborMap &msg);

    // Device currently receiving our keyboard and mouse; our own id when local.
    QByteArray activeDevice() const { return m_active; }
    QPoint cursor() const { return m_cursor; }
    // Master side: sequence number of the last motion datagram sent (tests).
    quint32 motionSeq() const { return m_motionSeq; }
    bool isViewing() const { return m_viewing; }
    // The cursor is on this device (master: input is local; slave: the master's cursor is here,
    // or nobody drives it), or left it less than graceMs ago.
    bool cursorHere(qint64 graceMs) const;
    // Slave side: the master's cursor is on this device because it crossed in (in.enter, no
    // in.leave yet). Evidence of a real crossing, unlike cursorHere which is true when idle.
    bool cursorArrived() const { return m_masterHere; }
    // The cursor left this device less than graceMs ago (it was here and crossed away).
    bool cursorLeftRecently(qint64 graceMs) const;

    // Evidence of a paste, for the shared clipboard (ClipboardSync). Master side: the keys and
    // buttons it forwards to the active device are watched on the way: a paste chord (Ctrl+V,
    // Shift+Insert, Ctrl+Shift+V; Cmd+V arrives as the set-1 code the mac backend maps Command
    // to) was forwarded to `device` less than windowMs ago / any key or button press was. Slave
    // side: a key or button press from the master reached this device less than windowMs ago.
    bool pasteOrderedOn(const QByteArray &device, qint64 windowMs) const;
    bool inputOn(const QByteArray &device, qint64 windowMs) const;
    bool inputArrived(qint64 windowMs) const;

    // Paste evidence for file data (ClipboardSync::Presence::lastPasteGesture): when a paste
    // gesture last happened at this device, in nowMs() milliseconds (-1: never). A gesture is a
    // paste chord or a gesture key (keymap::isPasteEvidenceKey) or a mouse button press, either
    // forwarded here by the master (in.key / in.btn, judged against the keys it holds down here,
    // cleared on in.leave and releaseAll) or pressed on this device's own keyboard and mouse
    // (noteLocalPasteGesture, the helper's localPress). Motion and the wheel never count, nor does
    // input the helper injects. Each gesture emits pasteGesture(), so a request waiting for one
    // can look again at once.
    qint64 lastPasteGestureAt() const { return m_lastPasteGestureAt; }
    // Whether a paste gesture happened between `from` and `to` (nowMs(), both included; `to` < 0:
    // up to now). The gestures of the last kPasteGestureHistoryMs are kept, not only the last one:
    // a file manager reads the list of a large copy at the paste, then creates its folders for a
    // while before it reads a file, and the user may click elsewhere meanwhile.
    bool pasteGestureBetween(qint64 from, qint64 to) const;
    static constexpr qint64 kPasteGestureHistoryMs = 150 * 1000;
    static constexpr size_t kPasteGestureHistoryMax = 512;
    void noteLocalPasteGesture();
    // The session helper reports local presses (localPressSupport): false until it says so, and
    // again when it goes. Without it (Wayland, macOS without its event tap, older helpers) only
    // forwarded input is seen.
    bool localPressSupported() const { return m_localPressSupported; }
    void setLocalPressSupported(bool on) { m_localPressSupported = on; }

    // Remote view, master side: the device accepted (input may flow) / the view failed (go home).
    void setViewAccepted(const QByteArray &device);
    void cancelView();
    // Master side: the cursor comes back to this device (centre of its primary screen) and
    // capture ends, so the keyboard types here again. Used when this device locks or its session
    // switches: what is typed on its lock screen (the password) must stay on it.
    void returnHome();
    // Source side: ignore input from `peerId` while its view request is being decided locally.
    void setRemoteInputBlocked(const QByteArray &peerId, bool on);
    // Master side: devices whose Control and Command keys are swapped on the way (a PC keyboard
    // driving a Mac: Ctrl+C there is Cmd+C). The set-1 codes of Ctrl (0x1D / 0xE01D) and of the
    // Windows key, which the Mac backend maps to Command (0xE05B / 0xE05C), trade places.
    void setSwapCtrlCmd(const QSet<QByteArray> &devices) { m_swapCtrlCmd = devices; }
    // Keyboard diagnostics (KeyDiagnostics::noteKey): told of every key forwarded to another
    // device (sent, after the Ctrl/Command swap; an empty peer: a key from the helper that went
    // nowhere, the input being home or a view not accepted yet) and of every key received from
    // the master.
    using KeyTrace = std::function<void(const QByteArray &peer, quint32 scancode, bool down, bool sent)>;
    void setKeyTrace(KeyTrace trace) { m_keyTrace = std::move(trace); }
    // And of every key received from a device and not taken (`why`: not the master, a guest...).
    using IgnoredKeyTrace = std::function<void(const QByteArray &peer, quint32 scancode, bool down, const QString &why)>;
    void setIgnoredKeyTrace(IgnoredKeyTrace trace) { m_ignoredKeyTrace = std::move(trace); }
    bool swapsCtrlCmd(const QByteArray &device) const { return m_swapCtrlCmd.contains(device); }

    // File drag, master side: answer to dragCatchRequested (files: files are being dragged).
    void dragCaught(const QByteArray &source, bool files);
    // The source has files and is still listing them (a large folder): the crossing waits up to
    // kDragCatchExtendedMs in all instead of kDragCatchTimeoutMs.
    void dragCatchPending(const QByteArray &source);
    bool isDraggingFiles() const { return m_dragging; }
    QByteArray dragSource() const { return m_dragSource; }
    // The device a catch is pending with (empty if none): only it may answer with drag.caught.
    QByteArray catchSource() const { return m_pendingCrossing ? m_catchSource : QByteArray(); }
    static constexpr int kDragCatchTimeoutMs = 1500;
    static constexpr int kDragCatchExtendedMs = 10000;

    static constexpr int kViewRetryMs = 3000; // no new view of a device right after a failure
    static constexpr int kRefusedRetryMs = 3000; // not entered again right after it refused input
    static constexpr int kBlockedNoticeMs = 10000; // crossingBlocked, per device
    void setBlockedNoticeInterval(int ms) { m_blockedNoticeMs = ms; } // tests shorten it

signals:
    void toSession(const QCborMap &msg);
    void activeDeviceChanged(const QByteArray &deviceId);
    // Master side: the cursor went from one device to another (either may be this one). Not for a
    // remote view until the device accepts it.
    void cursorMoved(const QByteArray &from, const QByteArray &to);
    // Remote view: the service should stream these screens of the device, each sized for its
    // monitor / the displayed set changed (screens no longer listed stop, new ones start) / stop.
    void viewRequested(const QByteArray &device, const QList<ViewScreen> &shown);
    void viewScreensChanged(const QByteArray &device, const QList<ViewScreen> &shown);
    void viewEnded(const QByteArray &device);
    // File drag: find out what is dragged on `source` at `at` (its coordinates) / drop it on
    // `target` at `at` / the drag ended without a drop.
    void dragCatchRequested(const QByteArray &source, QPoint at);
    void dragDropped(const QByteArray &target, QPoint at);
    // Files were being dragged towards `target`, which takes no input from here (it declared this
    // device a guest, or went away): the drag ended where it started (dragDropped on the source).
    void dragRefused(const QByteArray &target);
    // The source said it was listing dragged files (dragCatchPending) but did not finish within
    // kDragCatchExtendedMs: the crossing went ahead without them.
    void dragCatchTimedOut(const QByteArray &source);
    // Master side: the device could not take the input, which came back here.
    void inputRefused(const QByteArray &deviceId);
    // Master side: the cursor was pushed into `deviceId` and stayed here, because it declared this
    // device a guest. At most once per kBlockedNoticeMs per device.
    void crossingBlocked(const QByteArray &deviceId);
    // A paste gesture happened at this device (lastPasteGestureAt was just updated).
    void pasteGesture();

private:
    void rebuild();
    // Master side: tells the helper where a push past an edge will cross (edges, see above).
    void pushEdgeMap();
    // Master side, input on another device: back home if no edge leads back any more.
    void keepAWayHome();
    // Slave side: tell the master to take its input back.
    void refuseInput(const QByteArray &master);
    bool isMaster() const;
    // Master side: the device is connected and would take our input (it did not declare us a guest).
    bool canEnter(const QByteArray &device) const;
    // Master side: crossingBlocked, rate limited.
    void noteBlocked(const QByteArray &device);
    // Remote view: re-resolves the open view's plan (it changed) and shows the result.
    void refreshViewPlan();
    // Where a crossing lands: `depth` pixels past the target's edge (the motion left over), or
    // one pixel when that is off its screens.
    QPoint landing(const Crossing &target, Side side, int depth) const;
    void enter(const Crossing &target, Side side, int depth = 1);
    // Crossing into `device` shows its screens: flagged in the layout and remote view on here.
    bool viewsDevice(const QByteArray &device) const;
    void enterView(const Crossing &target, QPoint edgePoint, Side side);
    void moveInView(int dx, int dy);
    // Remote view: a visible screen of the viewed device, and the local monitor it is shown on.
    struct ViewEntry {
        int index = 0;  // position among the device's screens (ViewScreen::index)
        QRect remote;   // the screen (its native coordinates)
        QRect monitor;  // local monitor showing it
        bool shown = false; // the one its monitor shows now (one per monitor)
    };
    // Resolves the device's view plan for a view entered on local monitor `crossed`: the visible
    // screens, and (`hiddenOut`) the rects of the ones the plan hides.
    QList<ViewEntry> resolveViewPlan(const QByteArray &device, const QRect &crossed, QList<QRect> *hiddenOut = nullptr) const;
    // The visible screen beyond the edge of the active one that a move to `next` crosses
    // (position in m_viewScreens, or -1). A hidden screen in the way is skipped: the cursor lands
    // on the visible one beyond it, if any.
    int neighbourScreen(QPoint next) const;
    // Makes `at` the active screen, its monitor showing it (state only: moveInView announces).
    void activateViewScreen(int at);
    QList<ViewScreen> displayedViewScreens() const;
    QCborMap viewShowArgs(const QList<ViewScreen> &shown) const;
    bool sourceSupportsMultiView(const QByteArray &device) const;
    void sendViewCursor();
    void returnLocal(QPoint at, bool notifyRemote);
    void moveBy(int dx, int dy);
    void armReturnGuard(Side localEdge);
    // Sends the cursor position to the active device once per event-loop pass (flushMotionOut).
    void sendMotion();
    void flushMotionOut();
    void sendToActive(const QCborMap &msg);
    // File drag: hold a crossing back while the source says what is dragged.
    struct PendingCrossing {
        bool toLocal = false; // returnLocal(point) instead of enter(target, side)
        Crossing target;
        Side side = Side::Right;
        QPoint point;         // toLocal: where; view: the local edge point
        bool view = false;    // enterView(target.device, point, side)
    };
    void beginCatch(const QByteArray &source, QPoint at, const PendingCrossing &crossing);
    void finishCatch(bool files);
    void drop(const QByteArray &target, QPoint at);
    void onPeerMessage(const QByteArray &peerId, const QCborMap &msg);
    void onPeerDatagram(const QByteArray &peerId, const QByteArray &data);
    void emitSession(const QString &ev, QCborMap args = {});
    // Slave side: the latest position from the master, applied once the datagrams that arrived
    // together are read (a device that fell behind jumps there instead of replaying every move).
    void flushMotion();

    Node *m_node;
    Workspace *m_workspace;
    Router m_router;
    bool m_sessionUp = false;
    bool m_wasMaster = false;
    QTimer m_ping;

    // Master side.
    QByteArray m_active;
    QPoint m_cursor;
    quint32 m_motionSeq = 0;
    bool m_motionDirty = false; // m_cursor not sent yet (flushMotionOut queued)
    std::optional<QCborArray> m_lastEdgeMap; // last edges sent to the helper (none yet)
    QTimer m_edgeMapRetry; // re-pushes the map when a refusal expires
    // Remote view.
    bool m_viewing = false;
    QList<ViewEntry> m_viewScreens; // visible screens of the viewed device, in screen order
    QList<QRect> m_viewHidden;      // its screens the plan hides (never entered, crossed over)
    int m_viewActive = -1;          // position in m_viewScreens of the screen with the cursor
    QRect m_viewMonitor; // local monitor showing the active screen
    QRect m_viewRemote;  // the active screen (its native coordinates)
    bool m_viewMulti = true;        // the source streams several screens at once
    QList<int> m_viewShown;         // indices of the displayed set last given to the service
    Side m_viewSide = Side::Right; // local edge that was crossed
    QPoint m_viewHome;             // where the cursor returns if the view fails
    bool m_viewAccepted = false;   // the source accepted: input is forwarded
    QHash<QByteArray, qint64> m_viewRetryAfter;
    QHash<QByteArray, qint64> m_refusedUntil; // devices that refused the input, until when
    QHash<QByteArray, qint64> m_blockedNoticeAt; // last crossingBlocked, per device
    int m_blockedNoticeMs = kBlockedNoticeMs;
    QRect m_viewCrossed; // local monitor where the edge into the view was crossed
    // Edge we last came back through, and when (see kReturnGuardMs).
    QElapsedTimer m_returnGuard;
    Side m_returnEdge = Side::Right;

    // File drag (master side).
    QSet<int> m_remoteButtons;           // buttons pressed on the active (remote) device
    std::optional<PendingCrossing> m_pendingCrossing;
    QByteArray m_catchSource;
    QTimer m_catchTimer;
    bool m_catchExtended = false; // dragCatchPending
    bool m_dragging = false;
    QByteArray m_dragSource;

    QByteArray m_cursorFrom; // device the cursor came from (for a remote view accepted later)

    // Paste evidence (master side): the modifiers held on the remote device according to the
    // scancodes forwarded, the last press forwarded and the last paste chord, with their device.
    // Command (sent as the Windows key's scancodes) is a chord modifier only on a Mac: Win+V on
    // Windows opens the clipboard history, it pastes nothing.
    void notePress(bool pasteChord);
    QSet<quint32> m_heldForwarded;
    bool m_activeIsMac = false;
    QSet<QByteArray> m_swapCtrlCmd; // see setSwapCtrlCmd
    KeyTrace m_keyTrace;
    IgnoredKeyTrace m_ignoredKeyTrace;
    QByteArray m_lastInputDevice;
    qint64 m_lastInputAt = -1;
    QByteArray m_lastPasteDevice;
    qint64 m_lastPasteAt = -1;

    // Both sides: whether the cursor is away from this device, and since when.
    bool m_cursorAway = false;
    qint64 m_cursorLeftAt = 0;

    // Slave side.
    quint32 m_lastMotionSeq = 0;
    bool m_haveMotionSeq = false;
    std::optional<QPoint> m_pendingMotion;
    bool m_motionFlushQueued = false;
    QByteArray m_blockedPeer;
    bool m_masterHere = false; // the master's cursor is on this device (in.enter, no in.leave yet)
    qint64 m_lastMasterPressAt = -1; // a key or button press from the master (inputArrived)
    QSet<quint32> m_heldFromMaster;  // keys the master holds down here (paste chords)
    QByteArray m_heldFromMasterId;   // the master those keys came from

    // Both sides: paste evidence for file data (lastPasteGestureAt).
    void notePasteGesture();
    qint64 m_lastPasteGestureAt = -1;
    std::deque<qint64> m_pasteGestures; // oldest first, see pasteGestureBetween
    bool m_localPressSupported = false;
};

} // namespace df
