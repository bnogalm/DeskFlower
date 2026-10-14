#pragma once

// Routes keyboard and mouse between devices.
//
// The local session helper (deskflower-session) talks to this class through CBOR maps:
//   helper -> controller: edge {s, x, y}   cursor pushes past an outer edge of the local desktop
//                         mm {x, y}        relative motion while captured
//                         mb {b, d}        button (1 left, 2 right, 3 middle, 4 x1, 5 x2), d = down
//                         mw {x, y}        wheel (120 units per notch)
//                         key {s, d}       PS/2 set-1 scancode (0xE0xx for extended keys)
//   controller -> helper: config {master}  capture {on, x, y}  releaseAll  ping
//                         mabs {x, y}  mb {b, d, x, y}  mw {x, y}  key {s, d}  hideCursor
//
// On the master the controller keeps a virtual cursor in the active device's native coordinates
// and sends absolute positions, so pointer acceleration is applied only once (by the master's OS).
// Between devices (reliable channel): in.enter {x, y}, in.leave, in.btn {b, d, x, y},
// in.wheel {x, y}, in.key {s, d}. Pointer motion goes as unreliable datagrams
// [kind u8 = 1][seq u32][x i32][y i32] (latest wins).
// A device only obeys input coming from the current master.
//
// Remote view (devices flagged in the layout): instead of moving to the device's screens, its
// screens are shown full screen, one at a time, on the local monitor where the edge was crossed
// (the primary one first). Pushing the cursor past an edge of the shown screen towards another
// screen of that device shows that one; leaving through the side it came in returns home. Extra
// helper events: viewOpen {device, x, y, w, h (local monitor), rw, rh (remote screen)},
// viewScreen {rw, rh} (another remote screen is shown), viewCursor {x, y} (relative to the remote
// screen), viewClose (the service adds viewStatus {device, state, text}). Nothing reaches the remote device (not even in.enter) until
// its user accepts the view (setViewAccepted), so nobody can click its approval prompt blindly.
// A failed view (cancelView) returns home and the same device is not entered again for a moment.
//
// Dragging files between devices: crossing an edge with the left button held (helper: edge {lb}
// for the master's own button, the buttons forwarded for a remote device) holds the crossing back
// until the device the drag started on says whether files are being dragged (dragCatchRequested ->
// dragCaught, see FileDrag); the crossing then goes ahead. Releasing the button afterwards, on
// whichever device the cursor is, drops them there (dragDropped). The master's helper reports
// releases of its own button while a drag is on (dragWatch {on} -> dragRelease {x, y}).
// While a device's user is being asked, its input from the requester is ignored
// (setRemoteInputBlocked).

#include "router.h"

#include <QCborMap>
#include <QHash>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QSize>

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
    bool isViewing() const { return m_viewing; }

    // Remote view, master side: the device accepted (input may flow) / the view failed (go home).
    void setViewAccepted(const QByteArray &device);
    void cancelView();
    // Source side: ignore input from `peerId` while its view request is being decided locally.
    void setRemoteInputBlocked(const QByteArray &peerId, bool on);

    // File drag, master side: answer to dragCatchRequested (files: files are being dragged).
    void dragCaught(const QByteArray &source, bool files);
    bool isDraggingFiles() const { return m_dragging; }
    QByteArray dragSource() const { return m_dragSource; }
    static constexpr int kDragCatchTimeoutMs = 1500;

    static constexpr int kViewRetryMs = 3000; // no new view of a device right after a failure

signals:
    void toSession(const QCborMap &msg);
    void activeDeviceChanged(const QByteArray &deviceId);
    // Remote view: the service should stream the device's screen (sized for `monitor`) / stop.
    void viewRequested(const QByteArray &deviceId, QSize monitor, const QRect &remoteScreen);
    // Remote view: show this other screen of the device (its native coordinates).
    void viewScreenChanged(const QByteArray &deviceId, const QRect &remoteScreen);
    void viewEnded(const QByteArray &deviceId);
    // File drag: find out what is dragged on `source` at `at` (its coordinates) / drop it on
    // `target` at `at` / the drag ended without a drop.
    void dragCatchRequested(const QByteArray &source, QPoint at);
    void dragDropped(const QByteArray &target, QPoint at);

private:
    void rebuild();
    bool isMaster() const;
    void enter(const Crossing &target, Side side);
    void enterView(const QByteArray &device, QPoint edgePoint, Side side);
    void moveInView(int dx, int dy);
    // Remote view: the device's screen beyond the edge of `r` that a move to `next` crosses.
    QRect neighbourScreen(const QRect &r, QPoint next) const;
    void sendViewCursor();
    void returnLocal(QPoint at, bool notifyRemote);
    void moveBy(int dx, int dy);
    void armReturnGuard(Side localEdge);
    void sendMotion();
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
    // Remote view.
    bool m_viewing = false;
    QRect m_viewMonitor; // local monitor showing the remote screen
    QRect m_viewRemote;  // remote screen being shown (its native coordinates)
    Side m_viewSide = Side::Right; // local edge that was crossed
    QPoint m_viewHome;             // where the cursor returns if the view fails
    bool m_viewAccepted = false;   // the source accepted: input is forwarded
    QHash<QByteArray, qint64> m_viewRetryAfter;
    // Edge we last came back through, and when (see kReturnGuardMs).
    QElapsedTimer m_returnGuard;
    Side m_returnEdge = Side::Right;

    // File drag (master side).
    QSet<int> m_remoteButtons;           // buttons pressed on the active (remote) device
    std::optional<PendingCrossing> m_pendingCrossing;
    QByteArray m_catchSource;
    QTimer m_catchTimer;
    bool m_dragging = false;
    QByteArray m_dragSource;

    // Slave side.
    quint32 m_lastMotionSeq = 0;
    bool m_haveMotionSeq = false;
    QByteArray m_blockedPeer;
};

} // namespace df
