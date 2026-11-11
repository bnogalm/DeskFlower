#pragma once

// Remote view streaming between services over an encrypted TCP stream (SecureStream).
//
// Viewer (the master) -> source:  view {v: 2, screens: [{i, x, y, w, h, mw, mh}]}  stream all the
//                                 listed screens: each one is the source's own geometry (native
//                                 coordinates) of one of its monitors, i a distinct index the frames
//                                 are tagged with, mw x mh the largest useful frame for it (the local
//                                 monitor showing it). Then ack {s, i} after each frame is shown,
//                                 screens {screens: [...]} to replace the set on the same stream
//                                 (screens no longer listed stop, new ones start with a full frame),
//                                 stop {}. Nothing else is accepted on this stream: it carries frames,
//                                 not commands. A `view` with more than kMaxViewScreens entries, a
//                                 repeated index or screen, or a rect that is not one of the source's
//                                 screens is refused with code "bad".
//                                 Legacy (viewers older than kMultiViewVersion): view {w, h, screen?}
//                                 (absent screen: the primary one), screen {screen: {x, y, w, h}} and
//                                 ack {s}: a one-entry set with i = 0. The viewer sends these forms
//                                 to a source older than kMultiViewVersion.
// Source -> viewer:               status {s: "pending", to (ms)}  the source's user is being asked
//                                 status {s: "accepted"}          capture starts
//                                 refused {c, r} / error {c, r}   then the stream closes
//                                 frame {i, s, w, h, full, n}, then n tile frames, then fend {s}.
//   Codes (c): notMaster, noUser, denied, timeout, busy, revoked, disabled (remote view is off
//   there), guest (the viewer is a guest there), bad (malformed request), stopped, capture, stalled;
//   r is a hint.
// Stream frames start with a kind byte: 1 = CBOR control, 3 = tile
// ([x u16][y u16][w u16][h u16] + JPEG, little-endian).
//
// Frames are paced per screen by the viewer's acknowledgements: the source captures the next frame
// of a screen only after the previous one was shown, so latency stays bounded and nothing piles up.
// The source gives up when any screen's frame goes unacknowledged for kAckTimeoutMs. The viewer
// accepts a frame of a screen no larger than the limit it gave for it, and at most kMaxFrameBytes
// of tiles per frame (the helper's IPC refuses larger messages).
//
// A device only streams its screens to the current master, only while that peer is paired, and
// only with its local user's consent: either a persisted grant (Node::isViewAllowed) or an answer to
// authorizationRequested. Without a local user to ask, or with remote view turned off
// (Node::isRemoteViewEnabled), requests are refused.
//
// The viewer reports its progress through viewStateChanged and gives up with a reason (watchdog)
// when the source stops answering, so it never waits forever.
//
// Towards the local session helper frames are CBOR maps: {i, g, s, w, h, full, tiles: [{x, y, w, h, d}]},
// g being the generation of the capture (CaptureSpec) they belong to: a frame of an earlier one,
// still on its way when the screen started afresh, is dropped rather than shown as the new one.

#include "viewtypes.h"

#include <QByteArray>
#include <QCborArray>
#include <QCborMap>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <memory>

namespace df {

class Node;
class SecureStream;
class Workspace;

class RemoteView : public QObject
{
    Q_OBJECT
public:
    enum class ViewState { Connecting, AwaitingApproval, Starting, Streaming, Failed };
    Q_ENUM(ViewState)
    enum class Decision { Once, Always, Deny };

    static constexpr int kAnswerTimeoutMs = 10000;     // established -> status or first frame
    static constexpr int kApprovalGraceMs = 10000;     // added to the source's prompt timeout
    static constexpr int kMaxApprovalWaitMs = 120000;
    static constexpr int kFirstFrameTimeoutMs = 10000; // accepted -> frame, frame -> next frame
    static constexpr int kPromptTimeoutMs = 30000;     // source user's time to answer
    static constexpr int kDenyCooldownMs = 10000;      // requests refused straight away after a deny
    static constexpr int kAckTimeoutMs = 30000;        // source: a viewer silent this long is gone
    static constexpr int kMaxFrameSide = 8192;
    static constexpr int kMaxFrameBytes = 12 * 1024 * 1024; // tiles of one frame (ipc::kMaxMessage is 16 MB)
    static constexpr int kMaxViewScreens = df::kMaxViewScreens; // screens streamed at once (viewtypes.h)
    static constexpr int kMaxScreenIndex = 255;

    RemoteView(Node *node, Workspace *workspace, QObject *parent = nullptr);
    ~RemoteView() override;

    // Viewer side.
    // screens: which of the peer's screens (their native geometry) to show, each on the local
    // monitor that sizes its frames; at most kMaxViewScreens, at least one.
    void startViewing(const QByteArray &peerId, const QList<ViewScreen> &screens);
    // Replaces the set of shown screens of the same peer, on the same stream (no new approval).
    void setScreens(const QList<ViewScreen> &screens);
    void stopViewing();
    void frameShown(quint32 seq, int index);
    bool isViewing() const { return m_viewStream != nullptr; }
    void setViewTimeouts(int answerMs, int firstFrameMs);
    // Whether the peer streams several screens at once (kMultiViewVersion); an older one, or one
    // whose version is not known yet, gets the single-screen protocol.
    bool peerSupportsMultiView(const QByteArray &peerId) const;

    // Source side: a frame captured by the local helper ({i, g, s, w, h, full, tiles}); one of a
    // generation no longer captured is dropped.
    void frameCaptured(const QCborMap &frame);
    bool isStreaming() const { return m_sourceStream != nullptr; }
    QByteArray streamingTo() const;
    // The local user's answer to authorizationRequested.
    void answer(const QByteArray &peerId, Decision decision);
    // Whether someone is signed in locally who can answer a prompt.
    void setLocalUserPresent(bool present);
    // The local user stops the current stream.
    void stopStreaming();
    // The grant for `peerId` was withdrawn: stops streaming to it.
    void revoke(const QByteArray &peerId);
    // The local helper cannot capture a screen (any failure ends the whole stream).
    void captureFailed(const QString &reason);
    void setPromptTimeout(int ms) { m_promptTimeoutMs = ms; }
    void setAckTimeout(int ms) { m_ackTimeoutMs = ms; }

signals:
    // Viewer side: a complete frame for the local helper to show ({i, s, w, h, full, tiles}).
    void frameReceived(const QCborMap &frame);
    void viewStateChanged(const QByteArray &peerId, df::RemoteView::ViewState state, const QString &text);
    void viewingFailed(const QString &reason);
    // Source side: capture these screens (start, or replace the set: screens no longer listed stop,
    // new ones start with a full frame), the next frame of one screen, stop them all.
    void captureRequested(const QList<df::CaptureSpec> &screens);
    void captureNext(int index);
    void captureStopped();
    // Source side: ask the local user (answer() with the decision) / the question is gone.
    void authorizationRequested(const QByteArray &peerId, const QString &peerName, int timeoutMs);
    void authorizationClosed(const QByteArray &peerId);
    void streamingChanged(const QByteArray &peerId, bool on);

private:
    struct Pending {
        QByteArray peer;
        QPointer<SecureStream> stream;
        QList<CaptureSpec> screens;
    };

    // Viewer side.
    void sendScreens();
    void onViewerFrame(const QByteArray &frame);
    // The largest frame accepted for screen `index` (invalid: no such screen is shown).
    QSize frameBound(int index) const;
    void setViewState(ViewState state, const QString &text);
    void armWatchdog(int ms, const QString &failure);
    void failViewing(const QString &reason);
    QString textFor(const QString &code, const QString &hint) const;
    QString peerName(const QByteArray &id) const;

    // Source side.
    void onStreamAccepted(SecureStream *stream);
    void onViewRequest(SecureStream *stream, const QList<CaptureSpec> &screens);
    // The screens of a `view` or `screens` message, validated against this device's own screens
    // (a legacy `view` becomes a one-entry set with index 0); empty when the request is bad.
    QList<CaptureSpec> requestedScreens(const QCborMap &msg) const;
    // One of this device's screens described by a viewer, or an empty rect.
    QRect localScreen(const QCborValue &value) const;
    void onSourceFrame(SecureStream *stream, const QByteArray &frame);
    void onSourceClosed(SecureStream *stream);
    void accept(SecureStream *stream, const QList<CaptureSpec> &screens);
    std::unique_ptr<Pending> takePending();
    void dropPending(const QString &code, const QString &text);
    void endStream(const QString &code, const QString &text);
    void detachSource();
    void recheck();
    // Gives `spec` a generation of its own: its capture starts afresh.
    void restart(CaptureSpec *spec);
    // Arms the ack watchdog from the oldest unacknowledged frame (or from now, with none).
    void armAckWatchdog();

    Node *m_node;
    Workspace *m_workspace;

    // Viewer side.
    QPointer<SecureStream> m_viewStream;
    QByteArray m_viewPeer;
    QList<ViewScreen> m_viewScreens; // the peer's screens being shown
    QList<ViewScreen> m_sentScreens; // what the source was told (resent once it accepted, if stale)
    bool m_legacySource = false;     // single-screen protocol (peerSupportsMultiView)
    QSize m_legacyBound;             // the frame limit a legacy source was given (it cannot change)
    ViewState m_viewState = ViewState::Failed;
    QTimer m_viewWatchdog;
    QString m_watchdogFailure;
    int m_answerTimeoutMs = kAnswerTimeoutMs;
    int m_firstFrameTimeoutMs = kFirstFrameTimeoutMs;
    // The frame being reassembled (a source sends one frame at a time: header, tiles, fend).
    QCborMap m_incoming;
    QCborArray m_incomingTiles;
    int m_expectedTiles = -1;
    qint64 m_incomingBytes = 0;
    bool m_incomingDiscarded = false; // of a screen no longer shown: read through, not forwarded

    // Source side.
    QPointer<SecureStream> m_sourceStream;
    QList<CaptureSpec> m_captured; // the set being captured for m_sourceStream
    std::unique_ptr<Pending> m_pending; // at most one open prompt
    QTimer m_promptTimer;
    QHash<QByteArray, qint64> m_deniedUntil;
    bool m_userPresent = false;
    // Screen -> its frame sent and not yet acknowledged (sequence, and when it went out): acks only
    // pace real frames, a stale one (of a frame before the screen started afresh) paces nothing,
    // and the watchdog runs from the oldest of them.
    struct InFlight {
        qint64 seq = 0;
        qint64 sentMs = 0;
    };
    QHash<int, InFlight> m_inFlight;
    quint32 m_generation = 0; // last one given to a capture (restart)
    int m_promptTimeoutMs = kPromptTimeoutMs;
    QTimer m_ackWatchdog; // accepted, frame sent or acknowledged -> ack of the oldest frame in flight
    int m_ackTimeoutMs = kAckTimeoutMs;
};

} // namespace df
