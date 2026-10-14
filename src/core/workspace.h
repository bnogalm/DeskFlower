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
// every connected peer, so concurrent edits converge everywhere.
//
// Messages (CBOR maps, field "t"):
//   screens  {s: [screen...]}           thumb {s: screenId, d: jpeg}      thumbReq {}    thumbDenied {}
//   layout   {v, a, p: [{i, x, y}], rv: [id]}     master {e, m, a}
//
// Remote view ("rv"): crossing into one of these devices does not move the cursor to its screens;
// its primary screen is streamed and shown full screen on the local monitor instead (F7).

#include <QByteArray>
#include <QCborMap>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>

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

    // User edits.
    void setLayout(const QMap<QByteArray, QPoint> &offsets);
    void setRemoteView(const QByteArray &deviceId, bool on);
    bool isRemoteView(const QByteArray &deviceId) const { return m_layout.remoteView.contains(deviceId); }
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
    QByteArray masterId() const { return m_master.masterId; }

signals:
    void changed();
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
    LayoutState m_layout;
    MasterState m_master;
};

} // namespace df
