#pragma once

// PIN pairing state machine.
//
//   A (initiator)                          B (responder, must be in exploration mode)
//   PairRequest {i,n,k,o}        ->
//                                <-        PairChallenge {i,n,k,o,s}   B shows a 6-digit PIN
//   user types the PIN on A
//   PairMsg1 {i,s,y=Ya}          ->
//                                <-        PairMsg2 {i,s,y=Yb,c=confirmB}
//   A checks confirmB (wrong PIN => PairAbort)
//   PairMsg3 {i,s,c=confirmA}    ->
//                                <-        PairDone {i,s}              B trusts A
//   A trusts B
//
// The PIN is single use: B discards it after the first PairMsg1. Packets are retransmitted by the
// initiator; the responder answers duplicates from a cache.

#include "cpace.h"
#include "store.h"
#include "wire.h"

#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

namespace df {

class PairingManager : public QObject
{
    Q_OBJECT
public:
    using SendFn = std::function<void(const QByteArray &packet, const QHostAddress &addr, quint16 port)>;

    PairingManager(const Identity &self, SendFn send, QObject *parent = nullptr);
    ~PairingManager() override;

    void setAcceptingRequests(bool accepting) { m_accepting = accepting; }

    // Initiator side.
    bool start(const QByteArray &peerId, const QHostAddress &addr, quint16 port);
    bool submitPin(const QByteArray &peerId, const QString &pin);
    void cancel(const QByteArray &peerId);

    void handlePacket(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port);

    static constexpr int kPinTimeoutMs = 120000;

signals:
    void pinGenerated(const QByteArray &peerId, const QString &peerName, const QString &pin);
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
    void abortTo(const QByteArray &sid, const QString &reason, const QHostAddress &addr, quint16 port);
    void failOutgoing(const QString &reason, bool notifyPeer);
    void failIncoming(const QString &reason, bool notifyPeer);
    QCborMap selfInfo() const;
    QByteArray channelId(const QByteArray &idA, const QByteArray &pubA, const QByteArray &idB,
                         const QByteArray &pubB) const;

    const Identity &m_self;
    SendFn m_send;
    bool m_accepting = false;
    QTimer m_timer;
    std::unique_ptr<Outgoing> m_out;
    std::unique_ptr<Incoming> m_in;
};

} // namespace df
