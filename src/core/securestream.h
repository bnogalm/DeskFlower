#pragma once

// Encrypted stream between paired devices, for bulk data (files, large clipboard content).
//
// Same cryptography as the UDP channel: a Noise_IK_25519_ChaChaPoly_SHA256 handshake with the
// long-term keys exchanged at pairing (prologue "deskflower/tcp/1"), then every frame is
// ChaCha20-Poly1305 with a per-direction counter as nonce (the transport keeps order, so no
// replay window).
//
// Wire: [length u32 big-endian][bytes]. The first frame each way is a Noise handshake message,
// then frames are AEAD ciphertexts of at most kMaxFrame plaintext bytes.
//
// Noise IK message 1 can be replayed by anyone who recorded it, and it authenticates the initiator
// only once its first encrypted frame decrypts. So the accepting side reports `established` only
// then (every protocol on these streams starts with a request from the side that connects), and
// until then the connection counts as unauthenticated (handshake timeout). Streams quiet in both
// directions for kIdleTimeoutMs are closed; neither a paused stream (setReadPaused) nor a sender
// held by its paused peer (bytes queued) counts as quiet.
//
// The bytes travel over a StreamTransport: a TCP connection, or a tunnel through the encrypted UDP
// session when no TCP connection gets through a NAT or firewall (Node). A stream can also start
// without a transport (pending()) and take over one that another stream established (takeOver):
// Node::openStream returns such a stream at once and races several ways of reaching the peer
// behind it, so that its callers see one stream whatever path won.
//
// A receiver that cannot keep up (a slow disk) pauses reading (setReadPaused): frames stay in the
// transport, TCP's flow control holds the sender, and a tunnel (which has none) is bounded by
// kMaxPausedBacklog. When the peer ends the connection, the frames it sent before are still
// delivered, after a pause if need be, and only then `closed`.

#include "crypto.h"
#include "noise.h"

#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace df {

// An ordered, reliable byte pipe (the subset of QTcpSocket a SecureStream uses).
class StreamTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void write(const QByteArray &bytes) = 0;
    virtual QByteArray readAll() = 0;
    virtual qint64 bytesToWrite() const = 0;
    // Bytes received and not read yet.
    virtual qint64 bytesAvailable() const = 0;
    // Sends what is queued, then ends the connection; `disconnected` follows. `disconnected` also
    // says that the peer ended it; what it sent before stays readable.
    virtual void disconnectFromHost() = 0;
    // Drops the connection at once, without signals.
    virtual void abort() = 0;
    virtual bool isUnconnected() const = 0;
    virtual QString errorString() const = 0;

signals:
    void connected();
    void readyRead();
    void bytesWritten();
    void disconnected();
    void errorOccurred();
};

// The peer closing the connection (RemoteHostClosedError) is not an error here: it is reported as
// `disconnected`, and the bytes that came before it stay readable.
class TcpTransport : public StreamTransport
{
    Q_OBJECT
public:
    // What the socket reads ahead of the stream: past it, a paused stream holds the sender.
    static constexpr qint64 kReadBufferBytes = 1024 * 1024;

    explicit TcpTransport(QTcpSocket *socket, QObject *parent = nullptr);
    QTcpSocket *socket() const { return m_socket; }
    void write(const QByteArray &bytes) override { m_socket->write(bytes); }
    QByteArray readAll() override;
    qint64 bytesToWrite() const override { return m_socket->bytesToWrite(); }
    qint64 bytesAvailable() const override { return m_tail.size() + m_socket->bytesAvailable(); }
    void disconnectFromHost() override;
    void abort() override { m_socket->abort(); }
    bool isUnconnected() const override { return m_socket->state() == QAbstractSocket::UnconnectedState; }
    QString errorString() const override { return m_socket->errorString(); }

private:
    void onEnded();

    QTcpSocket *m_socket;
    QByteArray m_tail; // what the socket held when the connection ended
    bool m_ended = false;
};

class SecureStream : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxFrame = 64 * 1024;
    static constexpr int kHandshakeTimeoutMs = 10000;
    // An accepted connection that has sent nothing yet gets only this long (a client sends its
    // handshake message as soon as it connects; strangers holding sockets open get little).
    static constexpr int kSilentTimeoutMs = 2000;
    static constexpr int kIdleTimeoutMs = 5 * 60 * 1000;
    // A paused stream fails when its transport holds more than this unread (a tunnel: TCP holds
    // the sender long before).
    static constexpr qint64 kMaxPausedBacklog = 64 * 1024 * 1024;

    // Returns the peer id when the remote static key belongs to a trusted device.
    using TrustCheck = std::function<std::optional<QByteArray>(const QByteArray &remoteStatic)>;

    // Client side: connects over TCP and authenticates the device whose static key is `remoteStatic`.
    static SecureStream *connectTo(const crypto::KeyPair &local, const QByteArray &remoteStatic,
                                   const QByteArray &peerId, const QHostAddress &addr, quint16 port,
                                   QObject *parent = nullptr);
    // Client side over a transport that is already connected (the stream takes ownership);
    // `target` names it in failure reasons.
    static SecureStream *connectOver(StreamTransport *transport, const crypto::KeyPair &local,
                                     const QByteArray &remoteStatic, const QByteArray &peerId,
                                     const QString &target, QObject *parent = nullptr);
    // Server side: takes ownership of an accepted socket (or transport).
    static SecureStream *accept(QTcpSocket *socket, const crypto::KeyPair &local, TrustCheck trust,
                                QObject *parent = nullptr);
    static SecureStream *acceptOver(StreamTransport *transport, const crypto::KeyPair &local, TrustCheck trust,
                                    QObject *parent = nullptr);
    // A stream with no transport yet: `established` when takeOver() gives it one, `closed` when
    // abort() says nothing will. Its handshake timer is `timeoutMs` (0: none).
    static SecureStream *pending(const crypto::KeyPair &local, const QByteArray &peerId, int timeoutMs,
                                 QObject *parent = nullptr);

    ~SecureStream() override;

    QByteArray peerId() const { return m_peerId; }
    bool isEstablished() const { return m_state == State::Established; }
    bool isClosed() const { return m_state == State::Closed; }
    // Accepted and nothing received yet (a stranger holding a socket, or a client not yet heard).
    bool isSilent() const { return m_silent; }
    // Encrypts and queues one frame (at most kMaxFrame bytes). Returns false if not established.
    bool send(const QByteArray &plaintext);
    // Bytes queued in the transport and not yet written: used for back-pressure.
    qint64 pendingBytes() const { return m_transport ? m_transport->bytesToWrite() : 0; }
    // Flushes queued frames and disconnects; `closed` follows when the transport is gone.
    void close();
    // Drops the connection at once (queued frames included); `closed` is emitted before it returns.
    void abort(const QString &reason);
    // While paused, no frame is read or emitted (the one being emitted when it is called still is):
    // the peer is held by flow control. Resuming delivers what waited, in order, and then the end
    // of the connection if the peer ended it meanwhile.
    void setReadPaused(bool paused);
    bool isReadPaused() const { return m_readPaused; }

    // Accepting side, inside a slot connected to `established`: the first frame the peer sent
    // (what authenticated it). discardFirstFrame() keeps it from being emitted as `received`.
    QByteArray firstFrame() const { return m_firstFrame; }
    void discardFirstFrame() { m_firstFrameDiscarded = true; }

    // Moves an established stream `from` (same peer) into this pending one: its transport, keys,
    // counters and unread bytes. `from` is left closed without signals (the caller deletes it);
    // this one emits `established`. False (nothing changes) when this one is not pending, or
    // `from` is not established or belongs to another peer.
    bool takeOver(SecureStream *from);

signals:
    void established();
    void received(const QByteArray &plaintext);
    void bytesWritten();
    void closed(const QString &reason);

private:
    enum class State { Pending, Connecting, AwaitingResponse, AwaitingInit, AwaitingFirstFrame, Established, Closing, Closed };

    SecureStream(StreamTransport *transport, const crypto::KeyPair &local, QObject *parent);
    void attach(StreamTransport *transport);
    void onConnected();
    void onReadyRead();
    void processBuffer();
    void handleFrame(const QByteArray &frame);
    void onTransportEnded();
    void finishIfEnded();
    void restartIdleTimer(); // Established and not paused
    void writeFrame(const QByteArray &frame);
    void fail(const QString &reason);

    StreamTransport *m_transport = nullptr;
    crypto::KeyPair m_local;
    State m_state = State::Connecting;
    TrustCheck m_trust;
    std::unique_ptr<noise::IKInitiator> m_initiator;
    QByteArray m_peerId;
    QString m_target; // client side: "address:port", for the failure reasons
    QByteArray m_sendKey;
    QByteArray m_recvKey;
    quint64 m_sendCounter = 0;
    quint64 m_recvCounter = 0;
    QByteArray m_buffer;
    QByteArray m_firstFrame;
    bool m_firstFrameDiscarded = false;
    bool m_silent = false; // accepted and nothing received yet (kSilentTimeoutMs)
    bool m_readPaused = false;
    bool m_remoteEnded = false; // the peer ended the connection; closed once what it sent is delivered
    QTimer m_handshakeTimer;
    QTimer m_idleTimer;
};

} // namespace df
