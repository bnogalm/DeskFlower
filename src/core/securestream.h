#pragma once

// Encrypted TCP stream between paired devices, for bulk data (files, large clipboard content).
//
// Same cryptography as the UDP channel: a Noise_IK_25519_ChaChaPoly_SHA256 handshake with the
// long-term keys exchanged at pairing (prologue "deskflower/tcp/1"), then every frame is
// ChaCha20-Poly1305 with a per-direction counter as nonce (TCP keeps order, so no replay window).
//
// Wire: [length u32 big-endian][bytes]. The first frame each way is a Noise handshake message,
// then frames are AEAD ciphertexts of at most kMaxFrame plaintext bytes.
//
// Noise IK message 1 can be replayed by anyone who recorded it, and it authenticates the initiator
// only once its first encrypted frame decrypts. So the accepting side reports `established` only
// then (every protocol on these streams starts with a request from the side that connects), and
// until then the connection counts as unauthenticated (handshake timeout). Streams quiet in both
// directions for kIdleTimeoutMs are closed.

#include "crypto.h"
#include "noise.h"

#include <QHostAddress>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace df {

class SecureStream : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxFrame = 64 * 1024;
    static constexpr int kHandshakeTimeoutMs = 10000;
    static constexpr int kIdleTimeoutMs = 5 * 60 * 1000;

    // Returns the peer id when the remote static key belongs to a trusted device.
    using TrustCheck = std::function<std::optional<QByteArray>(const QByteArray &remoteStatic)>;

    // Client side: connects and authenticates the device whose static key is `remoteStatic`.
    static SecureStream *connectTo(const crypto::KeyPair &local, const QByteArray &remoteStatic,
                                   const QByteArray &peerId, const QHostAddress &addr, quint16 port,
                                   QObject *parent = nullptr);
    // Server side: takes ownership of an accepted socket.
    static SecureStream *accept(QTcpSocket *socket, const crypto::KeyPair &local, TrustCheck trust,
                                QObject *parent = nullptr);

    ~SecureStream() override;

    QByteArray peerId() const { return m_peerId; }
    bool isEstablished() const { return m_state == State::Established; }
    // Encrypts and queues one frame (at most kMaxFrame bytes). Returns false if not established.
    bool send(const QByteArray &plaintext);
    // Bytes queued in the socket and not yet written: used for back-pressure.
    qint64 pendingBytes() const { return m_socket->bytesToWrite(); }
    // Flushes queued frames and disconnects; `closed` follows when the socket is gone.
    void close();

signals:
    void established();
    void received(const QByteArray &plaintext);
    void bytesWritten();
    void closed(const QString &reason);

private:
    enum class State { Connecting, AwaitingResponse, AwaitingInit, AwaitingFirstFrame, Established, Closing, Closed };

    SecureStream(QTcpSocket *socket, const crypto::KeyPair &local, QObject *parent);
    void onConnected();
    void onReadyRead();
    void handleFrame(const QByteArray &frame);
    void writeFrame(const QByteArray &frame);
    void fail(const QString &reason);

    QTcpSocket *m_socket;
    crypto::KeyPair m_local;
    State m_state = State::Connecting;
    TrustCheck m_trust;
    std::unique_ptr<noise::IKInitiator> m_initiator;
    QByteArray m_peerId;
    QByteArray m_sendKey;
    QByteArray m_recvKey;
    quint64 m_sendCounter = 0;
    quint64 m_recvCounter = 0;
    QByteArray m_buffer;
    QTimer m_handshakeTimer;
    QTimer m_idleTimer;
};

} // namespace df
