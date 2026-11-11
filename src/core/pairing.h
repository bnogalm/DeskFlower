#pragma once

// PIN pairing state machine.
//
//   A (initiator)                          B (responder, must be in exploration mode)
//   PairRequest {i,n,k,o,s,tl}   ->
//                                <-        PairChallenge {i,n,k,o,s}   B shows a 6-digit PIN
//   user types the PIN on A
//   PairMsg1 {i,s,y=Ya}          ->
//                                <-        PairMsg2 {i,s,y=Yb,c=confirmB}
//   A checks confirmB (wrong PIN => PairAbort)
//   PairMsg3 {i,s,c=confirmA}    ->
//                                <-        PairDone {i,s}              B trusts A
//   A trusts B
//
// `tl` (0.1.22) is the level A chose for B ("guest"/"device"/"group", trustName): a suggestion
// only, so that B's PIN dialog can offer the same choice (a group is formed only when both sides
// choose it). It decides nothing by itself: B's user sees it preselected and may change it, and
// Node never lets it promote a device B has already made a guest. Older responders ignore it;
// an absent or unknown value is no suggestion.
//
// The PIN is single use: B discards it after the first PairMsg1. Packets are retransmitted by the
// initiator; the responder answers duplicates from a cache.
//
// Guessing: each request gets a new PIN, so an attacker on the network gets one guess in a million
// per attempt. B counts attempts in which a guess was made (a PairMsg1 arrived) and that do not
// end in pairing (wrong PIN, abandoned, timed out; not the local user cancelling): after
// kMaxFailedAttempts it refuses requests for kLockoutMs, and once an attempt failed it answers the
// next guesses (PairMsg2) only after kRetryDelayMs. A challenge nobody answers does not count (a
// stranger could request and walk away three times to lock the device), and a device that already
// has a challenge pending gets that same one again, never a second PIN.
//
// The session id is the pairing's secret, so the responder repeats the challenge only to the
// address and port the request came from, and takes PairMsg1 and PairMsg3 only from there: a
// stranger who repeats the requester's public ids gets a `busy` refusal, not the challenge, and
// one who got hold of the session id some other way can neither spend the PIN nor end the pairing.
//
// Aborts: PairAbort {i,s,r,e,m} ends a pairing only when it carries the secret `s` that identifies
// it (the session id from the challenge, or before it, the random `s` of the PairRequest, echoed
// by the responder's refusals) and comes from the address the pairing talks to; a stranger who
// knows only the public ids cannot cancel it. `r` is the reason text for older versions, `e` a
// code (with `m` minutes for a lockout) shown in the local language and capped.

#include "cpace.h"
#include "store.h"
#include "wire.h"

#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace df {

class PairingManager : public QObject
{
    Q_OBJECT
public:
    using SendFn = std::function<void(const QByteArray &packet, const QHostAddress &addr, quint16 port)>;

    PairingManager(const Identity &self, SendFn send, QObject *parent = nullptr);
    ~PairingManager() override;

    void setAcceptingRequests(bool accepting) { m_accepting = accepting; }
    // How long both sides wait for the PIN (kPinTimeoutMs; tests shorten it).
    void setPinTimeout(int ms) { m_pinTimeoutMs = ms; }

    // Initiator side. suggested: the level chosen for the peer, sent as `tl`.
    bool start(const QByteArray &peerId, const QHostAddress &addr, quint16 port,
               std::optional<TrustLevel> suggested = std::nullopt);
    bool submitPin(const QByteArray &peerId, const QString &pin);
    void cancel(const QByteArray &peerId);

    void handlePacket(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port);
    // A pairing with `peerId` is under way, on either side (not one that already ended).
    bool isPairingWith(const QByteArray &peerId) const;

    static constexpr int kPinTimeoutMs = 120000;
    static constexpr int kMaxFailedAttempts = 3;
    static constexpr qint64 kLockoutMs = 5 * 60 * 1000;
    static constexpr int kRetryDelayMs = 300;

    // Responder side: requests are refused until then (nowMs() time; 0: not locked).
    qint64 lockedUntil() const { return m_lockedUntil; }

    // What a peer's abort means, in local words: a known code is translated, anything else is
    // the capped text it sent (kMaxReasonLength).
    static QString reasonText(const QString &code, const QString &text, int minutes);
    static constexpr int kMaxReasonLength = 200;

signals:
    // suggested: the level the requester chose for this device (`tl`), if it sent one.
    void pinGenerated(const QByteArray &peerId, const QString &peerName, const QString &pin,
                      std::optional<df::TrustLevel> suggested);
    void pinRequired(const QByteArray &peerId, const QString &peerName);
    void succeeded(const df::TrustedPeer &peer, const QHostAddress &addr, quint16 port);
    void failed(const QByteArray &peerId, const QString &reason);

private:
    struct Outgoing;
    struct Incoming;

    void onTimer();
    void handleAsResponder(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port);
    void handleAsInitiator(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port);
    void sendOut(const QByteArray &packet);
    // code: what the peer shows (reasonText); text: the same in our words, for older versions.
    void abortTo(const QByteArray &sid, const QString &code, const QString &text, const QHostAddress &addr,
                 quint16 port, int minutes = 0);
    void failOutgoing(const QString &reason, bool notifyPeer, const QString &code = QString());
    // attempt: may count as a failed pairing attempt (not when the local user cancels); it does
    // only when a guess was made.
    void failIncoming(const QString &reason, bool notifyPeer, bool attempt = true, const QString &code = QString());
    QCborMap selfInfo() const;
    QByteArray channelId(const QByteArray &idA, const QByteArray &pubA, const QByteArray &idB,
                         const QByteArray &pubB) const;

    const Identity &m_self;
    SendFn m_send;
    bool m_accepting = false;
    QTimer m_timer;
    std::unique_ptr<Outgoing> m_out;
    std::unique_ptr<Incoming> m_in;
    int m_failedAttempts = 0;
    qint64 m_lockedUntil = 0;
    int m_pinTimeoutMs = kPinTimeoutMs;
};

} // namespace df
