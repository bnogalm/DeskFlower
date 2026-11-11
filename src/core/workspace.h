#pragma once

// Shared workspace state, replicated between paired devices over the reliable channel:
//   - screens:  every device's monitors, in that device's native desktop coordinates
//               (Windows: physical pixels, macOS: points, X11: pixels)
//   - thumbs:   small JPEG captures of each monitor, so the user can tell them apart. Pixels are
//               private: a device sends its thumbnails only to a peer holding its view grant
//               (Node::isViewAllowed), only to a peer that asked for them, and never relays others'.
//   - layout:   where each device's "pack" of monitors sits in the shared plane (an offset applied
//               to the device's own desktop coordinates)
//   - master:   which device currently shares its keyboard and mouse
//
// Layout and master are last-writer-wins registers ordered by (version, author id) and gossiped to
// every connected peer, so concurrent edits converge everywhere. Guests (Node::isGuest) get them
// but cannot change them: a guest's own layout edit or master claim is answered with ws.refused
// {k: "layout"|"master", r: "guest"} (at most once per kRefusedReplyMs per peer and kind), and
// the guest tells its user (editRefused) instead of being left with a layout nobody else has.
// The guest believes such a reply only from a peer that did declare it a guest
// (Node::declaredTrust), and tells its user at most once per kRefusedReplyMs per peer and kind
// too, so a misbehaving peer cannot flood it with notices.
// Versions before 0.1.22 neither send nor understand it (they ignore unknown messages).
//
// Messages (CBOR maps, field "t"):
//   screens  {s: [screen...]}           thumb {s: screenId, d: jpeg}      thumbReq {}    thumbDenied {}
//   layout   {v, a, p: [{i, x, y}], rv: [id]}     master {e, m, a}        ws.refused {k, r}
//
// Remote view ("rv"): crossing into one of these devices does not move the cursor to its screens;
// they are streamed and shown full screen on the local monitors instead (F7). Where each of its
// screens goes is a view plan: local only (workspace.json "viewPlans"), never gossiped, since it
// names this device's monitors.

#include "viewtypes.h"

#include <QByteArray>
#include <QCborMap>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <algorithm>

namespace df {

class Node;

struct ScreenInfo {
    QString id;      // stable per device (e.g. "\\.\DISPLAY1")
    QString name;    // human readable (manufacturer/model)
    QRect geometry;  // native desktop coordinates of the owning device
    double scale = 1.0;
    bool primary = false;

    QCborMap toCbor() const;
    static ScreenInfo fromCbor(const QCborMap &m);
    bool operator==(const ScreenInfo &o) const = default;
};

// Last-writer-wins register ordering.
struct Stamp {
    quint64 version = 0;
    QByteArray author;
    bool newerThan(const Stamp &o) const { return version != o.version ? version > o.version : author > o.author; }
};

struct LayoutState {
    Stamp stamp;
    QMap<QByteArray, QPoint> offsets; // device id -> pack offset
    QSet<QByteArray> remoteView;      // devices shown on the local screen instead of moved to
};

struct MasterState {
    Stamp stamp;
    QByteArray masterId; // empty: nobody shares
};

class Workspace : public QObject
{
    Q_OBJECT
public:
    Workspace(Node *node, const QString &configDir, QObject *parent = nullptr);

    // Local device (reported by the agent running in the user session).
    void setLocalScreens(const QList<ScreenInfo> &screens);
    void setLocalThumbnail(const QString &screenId, const QByteArray &jpeg);
    // Asks every connected peer to send fresh thumbnails (each one answers only if it granted us view).
    void requestRemoteThumbnails();
    // Drops every cached thumbnail (this device's and peers'), emitting a change for each. Used when
    // the console user changes, so one user's screen previews never reach the next user's agent.
    void forgetThumbnails();

    // User edits.
    void setLayout(const QMap<QByteArray, QPoint> &offsets);
    // The version a new local edit of each register gets (above whatever was seen, taken or not).
    quint64 nextLayoutVersion() const { return std::max(m_layout.stamp.version, m_layoutFloor) + 1; }
    quint64 nextMasterVersion() const { return std::max(m_master.stamp.version, m_masterFloor) + 1; }
    void setRemoteView(const QByteArray &deviceId, bool on);
    bool isRemoteView(const QByteArray &deviceId) const { return m_layout.remoteView.contains(deviceId); }
    // Where this device shows a viewed device's screens: remote screen id -> local screen id.
    // A remote screen absent from the map is shown on the monitor where the edge was crossed;
    // kHiddenScreen ("-") means it is not shown and the cursor never enters it.
    using ViewPlan = QMap<QString, QString>;
    static constexpr const char *kHiddenScreen = "-";
    static constexpr int kMaxViewPlanEntries = df::kMaxViewPlanEntries; // viewtypes.h
    // An empty plan removes it; one with too many entries is ignored. Kept only for trusted peers.
    void setViewPlan(const QByteArray &deviceId, const ViewPlan &plan);
    ViewPlan viewPlan(const QByteArray &deviceId) const { return m_viewPlans.value(deviceId); }
    void setSharing(bool share);

    QList<ScreenInfo> screens(const QByteArray &deviceId) const { return m_screens.value(deviceId); }
    QByteArray thumbnail(const QByteArray &deviceId, const QString &screenId) const;
    QHash<QString, QByteArray> thumbnails(const QByteArray &deviceId) const { return m_thumbs.value(deviceId); }
    // The device answered that we may not preview its screens (no view grant).
    bool thumbnailsDenied(const QByteArray &deviceId) const { return m_thumbDenied.contains(deviceId); }
    QList<QByteArray> devices() const; // self first, then trusted peers with known screens
    // Effective pack offset: the stored one, or a default to the right of everything else.
    QMap<QByteArray, QPoint> effectiveLayout() const;
    const LayoutState &layout() const { return m_layout; }
    // The device the master register names. A guest is never accepted into it (onMessage rejects a
    // relayed `master` whose device is a guest), and every consumer also refuses a master that is a
    // guest (InputController, RemoteView, ClipboardSync, FileDrag: `peerId == masterId() &&
    // !isGuest(peerId)`), so a device can still refuse a guest master gracefully rather than ignore
    // it silently. For the same reason a master flagged guest here keeps the register (its claims
    // are refused, not dropped on the floor) until some other device shares.
    QByteArray masterId() const { return m_master.masterId; }

    static constexpr int kRefusedReplyMs = 10000;

signals:
    void changed();
    // A peer refused our layout edit or master claim (kind "layout" / "master"): it treats this
    // device as a guest (reason "guest").
    void editRefused(const QByteArray &peerId, const QString &kind, const QString &reason);
    // Also emitted with an empty thumbnail when a cached one is dropped.
    void thumbnailChanged(const QByteArray &deviceId, const QString &screenId);
    // The agent should capture new thumbnails of the local screens.
    void localThumbnailsRequested();

private:
    void onPeerConnected(const QByteArray &peerId);
    void onPeerDisconnected(const QByteArray &peerId);
    void onPeersChanged();
    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    void sendScreens(const QByteArray &peerId);
    void sendThumbnails(const QByteArray &peerId);
    void handleThumbnailRequest(const QByteArray &peerId);
    void dropRemoteThumbnails(const QByteArray &peerId);
    void broadcast(const QCborMap &msg, const QByteArray &except = {});
    QCborMap layoutMessage() const;
    QCborMap masterMessage() const;
    void load();
    void save() const;

    Node *m_node;
    QString m_path;
    QHash<QByteArray, QList<ScreenInfo>> m_screens; // includes self
    QHash<QByteArray, QHash<QString, QByteArray>> m_thumbs;
    QHash<QByteArray, qint64> m_thumbRequests; // peer -> time of its last thumbReq (source side)
    QSet<QByteArray> m_viewGrants;             // peers allowed to view us, to notice grant changes
    QSet<QByteArray> m_thumbDenied;            // peers that refused to send us their thumbnails
    QHash<QString, qint64> m_refusedReplyAt;   // ws.refused sent, by peer id + kind (nowMs)
    QHash<QString, qint64> m_refusedHeardAt;   // editRefused emitted, by peer id + kind (nowMs)
    void replyRefused(const QByteArray &peerId, const QString &kind);
    // True (and the time noted) when nothing was noted for `key` in the last kRefusedReplyMs.
    static bool throttle(QHash<QString, qint64> &noted, const QString &key);
    LayoutState m_layout;
    MasterState m_master;
    QHash<QByteArray, ViewPlan> m_viewPlans; // viewed device -> plan (local, see setViewPlan)
    // Versions of registers refused because a guest authored them (onMessage): a local edit is
    // stamped above them, so devices that trust that guest and took its version still adopt the
    // edit (last writer wins is strict on the version), while the guest's content is never kept.
    quint64 m_layoutFloor = 0;
    quint64 m_masterFloor = 0;
};

} // namespace df
