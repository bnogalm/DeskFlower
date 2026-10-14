#pragma once

#include "noise.h"

#include <QByteArray>
#include <optional>

namespace df {

// Sliding 64-packet anti-replay window (RFC 6479 style, simplified).
class ReplayWindow
{
public:
    bool check(quint64 counter) const;
    void update(quint64 counter);

private:
    bool m_any = false;
    quint64 m_top = 0;
    quint64 m_bitmap = 0;
};

// Transport keys established by one Noise handshake.
class SecureSession
{
public:
    SecureSession(quint32 localIndex, quint32 remoteIndex, const noise::TransportKeys &keys, bool initiator,
                  qint64 nowMs);
    ~SecureSession();

    // Encrypts an inner packet into a complete Data UDP packet.
    QByteArray seal(const QByteArray &inner, qint64 nowMs);
    // Decrypts a complete Data UDP packet addressed to localIndex().
    std::optional<QByteArray> open(const QByteArray &packet, qint64 nowMs);

    quint32 localIndex() const { return m_localIndex; }
    quint32 remoteIndex() const { return m_remoteIndex; }
    bool isInitiator() const { return m_initiator; }
    qint64 createdAt() const { return m_createdAt; }
    qint64 lastReceived() const { return m_lastRecv; }
    qint64 lastSent() const { return m_lastSend; }
    // A responder session is only used for sending after the initiator proved it holds the keys.
    bool isConfirmed() const { return m_confirmed; }

private:
    quint32 m_localIndex;
    quint32 m_remoteIndex;
    QByteArray m_sendKey;
    QByteArray m_recvKey;
    quint64 m_sendCounter = 0;
    ReplayWindow m_replay;
    bool m_initiator;
    bool m_confirmed;
    qint64 m_createdAt;
    qint64 m_lastRecv;
    qint64 m_lastSend;
};

} // namespace df
