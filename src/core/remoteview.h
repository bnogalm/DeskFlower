#pragma once

// Remote view streaming between services over an encrypted TCP stream (SecureStream).
//
// Viewer (the master) -> source:  view {w, h, screen?}  (largest useful frame size, first frame
//                                 only), then ack {s} after each frame is shown, screen {x, y, w, h}
//                                 to show another of the source's screens, stop {}. Nothing else is
//                                 accepted on this stream: it carries frames, not commands. A screen
//                                 is the source's own geometry (native coordinates) of one of its
//                                 monitors; anything else is ignored (absent: the primary screen).
// Source -> viewer:               status {s: "pending", to (ms)}  the source's user is being asked
//                                 status {s: "accepted"}          capture starts
//                                 refused {c, r} / error {c, r}   then the stream closes
//                                 frame {s, w, h, full, n}, then n tile frames, then fend {s}.
//   Codes (c): notMaster, noUser, denied, timeout, busy, revoked, stopped, capture, stalled; r is a
//   hint.
// Stream frames start with a kind byte: 1 = CBOR control, 3 = tile
// ([x u16][y u16][w u16][h u16] + JPEG, little-endian).
//
// Frames are paced by the viewer's acknowledgements: the source captures the next frame only after
// the previous one was shown, so latency stays bounded and nothing piles up.
//
// A device only streams its screen to the current master, only while that peer is paired, and only
// with its local user's consent: either a persisted grant (Node::isViewAllowed) or an answer to
// authorizationRequested. Without a local user to ask, requests are refused.
//
// The viewer reports its progress through viewStateChanged and gives up with a reason (watchdog)
// when the source stops answering, so it never waits forever.
//
// Towards the local session helper frames are CBOR maps: {s, w, h, full, tiles: [{x, y, w, h, d}]}.

#include <QByteArray>
#include <QCborArray>
#include <QCborMap>
#include <QHash>
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

    RemoteView(Node *node, Workspace *workspace, QObject *parent = nullptr);
    ~RemoteView() override;

    // Viewer side.
    // screen: which of the peer's screens (its native geometry); empty: its primary screen.
    void startViewing(const QByteArray &peerId, QSize maxSize, const QRect &screen = {});
    // Shows another screen of the same peer, on the same stream (no new approval).
    void switchScreen(const QRect &screen);
    void stopViewing();
    void frameShown(quint32 seq);
    bool isViewing() const { return m_viewStream != nullptr; }
    void setViewTimeouts(int answerMs, int firstFrameMs);

    // Source side: a frame captured by the local helper.
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
    // The local helper cannot capture the screen.
    void captureFailed(const QString &reason);
    void setPromptTimeout(int ms) { m_promptTimeoutMs = ms; }
    void setAckTimeout(int ms) { m_ackTimeoutMs = ms; }

signals:
    // Viewer side: a complete frame for the local helper to show.
    void frameReceived(const QCborMap &frame);
    void viewStateChanged(const QByteArray &peerId, df::RemoteView::ViewState state, const QString &text);
    void viewingFailed(const QString &reason);
    // Source side: start capturing `screen` (empty: the primary one; frames no larger than
    // maxSize), capture another screen, next frame, stop.
    void captureRequested(QSize maxSize, const QRect &screen);
    void captureScreenChanged(const QRect &screen);
    void captureNext();
    void captureStopped();
    // Source side: ask the local user (answer() with the decision) / the question is gone.
    void authorizationRequested(const QByteArray &peerId, const QString &peerName, int timeoutMs);
    void authorizationClosed(const QByteArray &peerId);
    void streamingChanged(const QByteArray &peerId, bool on);

private:
    struct Pending {
        QByteArray peer;
        QPointer<SecureStream> stream;
        QSize size;
        QRect screen;
    };

    // Viewer side.
    void onViewerFrame(const QByteArray &frame);
    void setViewState(ViewState state, const QString &text);
    void armWatchdog(int ms, const QString &failure);
    void failViewing(const QString &reason);
    QString textFor(const QString &code, const QString &hint) const;
    QString peerName(const QByteArray &id) const;

    // Source side.
    void onStreamAccepted(SecureStream *stream);
    void onViewRequest(SecureStream *stream, QSize size, const QRect &screen);
    // One of this device's screens described by a viewer, or an empty rect.
    QRect localScreen(const QCborValue &value) const;
    void onSourceFrame(SecureStream *stream, const QByteArray &frame);
    void onSourceClosed(SecureStream *stream);
    void accept(SecureStream *stream, QSize size, const QRect &screen);
    std::unique_ptr<Pending> takePending();
    void dropPending(const QString &code, const QString &text);
    void endStream(const QString &code, const QString &text);
    void detachSource();
    void recheck();

    Node *m_node;
    Workspace *m_workspace;

    // Viewer side.
    QPointer<SecureStream> m_viewStream;
    QByteArray m_viewPeer;
    QSize m_requestedSize;
    QRect m_viewScreen; // the peer's screen being shown (empty: its primary one)
    ViewState m_viewState = ViewState::Failed;
    QTimer m_viewWatchdog;
    QString m_watchdogFailure;
    int m_answerTimeoutMs = kAnswerTimeoutMs;
    int m_firstFrameTimeoutMs = kFirstFrameTimeoutMs;
    QCborMap m_incoming;
    QCborArray m_incomingTiles;
    int m_expectedTiles = -1;

    // Source side.
    QPointer<SecureStream> m_sourceStream;
    std::unique_ptr<Pending> m_pending; // at most one open prompt
    QTimer m_promptTimer;
    QHash<QByteArray, qint64> m_deniedUntil;
    bool m_userPresent = false;
    bool m_frameInFlight = false; // sent and not yet acknowledged: acks only pace real frames
    int m_promptTimeoutMs = kPromptTimeoutMs;
    QTimer m_ackWatchdog; // accepted or frame sent -> ack (or next frame)
    int m_ackTimeoutMs = kAckTimeoutMs;
};

} // namespace df
