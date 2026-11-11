#include "securestream.h"

#include <QtEndian>
#include <utility>

namespace df {

namespace {
const QByteArray kPrologue("deskflower/tcp/1");
constexpr int kMaxWireFrame = SecureStream::kMaxFrame + 1024;
} // namespace

TcpTransport::TcpTransport(QTcpSocket *socket, QObject *parent) : StreamTransport(parent), m_socket(socket)
{
    m_socket->setParent(this);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setReadBufferSize(kReadBufferBytes);
    connect(m_socket, &QTcpSocket::connected, this, &StreamTransport::connected);
    connect(m_socket, &QTcpSocket::readyRead, this, &StreamTransport::readyRead);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &StreamTransport::bytesWritten);
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpTransport::onEnded);
    // Qt reports the peer's close as an error, before `disconnected`: failing on it would abort
    // the socket and lose what a paused stream has not read yet (the end of a transfer).
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (error == QAbstractSocket::RemoteHostClosedError)
            onEnded();
        else
            emit errorOccurred();
    });
}

QByteArray TcpTransport::readAll()
{
    if (m_tail.isEmpty())
        return m_socket->readAll();
    QByteArray bytes = std::exchange(m_tail, {});
    bytes += m_socket->readAll();
    return bytes;
}

void TcpTransport::onEnded()
{
    if (m_ended)
        return;
    m_ended = true;
    // Kept here whatever the socket does with its buffer once it is unconnected. After a local
    // abort() the device is already closed (and nobody reads what it held).
    if (m_socket->isReadable())
        m_tail += m_socket->readAll();
    emit disconnected();
}

void TcpTransport::disconnectFromHost() { m_socket->disconnectFromHost(); }

SecureStream::SecureStream(StreamTransport *transport, const crypto::KeyPair &local, QObject *parent)
    : QObject(parent), m_local(local)
{
    m_handshakeTimer.setSingleShot(true);
    m_handshakeTimer.setInterval(kHandshakeTimeoutMs);
    // The reason says which step never finished: a connection nobody answers (a firewall on the
    // other device, a wrong port) reads differently from a device that took the connection and
    // then said nothing.
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Connecting)
            fail(QStringLiteral("No answer from %1 (is its firewall letting TCP through?)").arg(m_target));
        else if (m_state == State::Pending)
            fail(QStringLiteral("Could not reach the device"));
        else
            fail(QStringLiteral("Handshake timed out"));
    });

    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleTimeoutMs);
    // A sender held by the peer's flow control (the peer paused reading: a slow disk) sees no
    // bytesWritten for as long as the peer stays paused: that is not idle.
    connect(&m_idleTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Established && m_transport && m_transport->bytesToWrite() > 0) {
            m_idleTimer.start();
            return;
        }
        fail(QStringLiteral("Idle for too long"));
    });

    if (transport) {
        attach(transport);
        m_handshakeTimer.start();
    }
}

void SecureStream::attach(StreamTransport *transport)
{
    m_transport = transport;
    m_transport->setParent(this);
    connect(m_transport, &StreamTransport::readyRead, this, &SecureStream::onReadyRead);
    connect(m_transport, &StreamTransport::bytesWritten, this, &SecureStream::bytesWritten);
    connect(m_transport, &StreamTransport::disconnected, this, &SecureStream::onTransportEnded);
    connect(m_transport, &StreamTransport::errorOccurred, this, [this] { fail(m_transport->errorString()); });
    connect(m_transport, &StreamTransport::bytesWritten, this, &SecureStream::restartIdleTimer);
}

SecureStream::~SecureStream()
{
    crypto::wipe(m_sendKey);
    crypto::wipe(m_recvKey);
}

SecureStream *SecureStream::connectTo(const crypto::KeyPair &local, const QByteArray &remoteStatic,
                                      const QByteArray &peerId, const QHostAddress &addr, quint16 port,
                                      QObject *parent)
{
    auto *socket = new QTcpSocket;
    auto *transport = new TcpTransport(socket);
    auto *stream = new SecureStream(transport, local, parent);
    stream->m_peerId = peerId;
    stream->m_target = QStringLiteral("%1:%2").arg(addr.toString()).arg(port);
    stream->m_initiator = std::make_unique<noise::IKInitiator>(local, remoteStatic, kPrologue);
    connect(transport, &StreamTransport::connected, stream, &SecureStream::onConnected);
    socket->connectToHost(addr, port);
    return stream;
}

SecureStream *SecureStream::connectOver(StreamTransport *transport, const crypto::KeyPair &local,
                                        const QByteArray &remoteStatic, const QByteArray &peerId,
                                        const QString &target, QObject *parent)
{
    auto *stream = new SecureStream(transport, local, parent);
    stream->m_peerId = peerId;
    stream->m_target = target;
    stream->m_initiator = std::make_unique<noise::IKInitiator>(local, remoteStatic, kPrologue);
    stream->onConnected();
    return stream;
}

SecureStream *SecureStream::accept(QTcpSocket *socket, const crypto::KeyPair &local, TrustCheck trust,
                                   QObject *parent)
{
    return acceptOver(new TcpTransport(socket), local, std::move(trust), parent);
}

SecureStream *SecureStream::acceptOver(StreamTransport *transport, const crypto::KeyPair &local, TrustCheck trust,
                                       QObject *parent)
{
    auto *stream = new SecureStream(transport, local, parent);
    stream->m_state = State::AwaitingInit;
    stream->m_trust = std::move(trust);
    stream->m_silent = true;
    stream->m_handshakeTimer.start(kSilentTimeoutMs);
    return stream;
}

SecureStream *SecureStream::pending(const crypto::KeyPair &local, const QByteArray &peerId, int timeoutMs,
                                    QObject *parent)
{
    auto *stream = new SecureStream(nullptr, local, parent);
    stream->m_state = State::Pending;
    stream->m_peerId = peerId;
    if (timeoutMs > 0)
        stream->m_handshakeTimer.start(timeoutMs);
    return stream;
}

bool SecureStream::takeOver(SecureStream *from)
{
    if (m_state != State::Pending || !from || from == this || from->m_state != State::Established
        || !from->m_transport || from->m_peerId != m_peerId)
        return false;
    StreamTransport *transport = from->m_transport;
    from->m_transport = nullptr;
    from->m_state = State::Closed; // no signals: nothing ends, the connection moves
    from->m_handshakeTimer.stop();
    from->m_idleTimer.stop();
    disconnect(transport, nullptr, from, nullptr);
    disconnect(transport, nullptr, &from->m_idleTimer, nullptr);
    attach(transport);
    m_sendKey = std::exchange(from->m_sendKey, {});
    m_recvKey = std::exchange(from->m_recvKey, {});
    m_sendCounter = from->m_sendCounter;
    m_recvCounter = from->m_recvCounter;
    m_buffer = std::exchange(from->m_buffer, {});
    m_target = from->m_target;
    // The transport may have ended already (`from` was reading what came before the end): it will
    // not say so again. m_readPaused stays this stream's own (its callers pause it).
    m_remoteEnded = std::exchange(from->m_remoteEnded, false);
    m_state = State::Established;
    m_handshakeTimer.stop();
    restartIdleTimer();
    emit established();
    // Bytes that came with the frame that established `from` belong to this stream now, then the
    // end of the connection if it came: next turn, as from the transport.
    QMetaObject::invokeMethod(this, [this] {
        if (m_state != State::Closed && m_transport)
            onReadyRead();
    }, Qt::QueuedConnection);
    return true;
}

void SecureStream::onConnected()
{
    const QByteArray message1 = m_initiator->writeMessage1({});
    if (message1.isEmpty()) {
        fail(QStringLiteral("Invalid peer key"));
        return;
    }
    m_state = State::AwaitingResponse;
    writeFrame(message1);
}

void SecureStream::writeFrame(const QByteArray &frame)
{
    char len[4];
    qToBigEndian<quint32>(quint32(frame.size()), len);
    m_transport->write(QByteArray(len, 4) + frame);
}

bool SecureStream::send(const QByteArray &plaintext)
{
    if (m_state != State::Established || plaintext.size() > kMaxFrame)
        return false;
    writeFrame(crypto::aeadEncrypt(m_sendKey, m_sendCounter++, {}, plaintext));
    return true;
}

void SecureStream::close()
{
    if (m_state == State::Closed || m_state == State::Closing)
        return;
    if (!m_transport) {
        fail(QStringLiteral("Closed"));
        return;
    }
    m_state = State::Closing;
    m_handshakeTimer.stop();
    m_transport->disconnectFromHost(); // flushes what is queued, then `disconnected` -> closed
    if (m_state != State::Closed && m_transport->isUnconnected())
        fail(QStringLiteral("Closed"));
}

void SecureStream::abort(const QString &reason) { fail(reason); }

void SecureStream::setReadPaused(bool paused)
{
    if (m_readPaused == paused)
        return;
    m_readPaused = paused;
    // Paused, the stream gets no events (over TCP the socket stops reading): not idle either.
    if (paused) {
        m_idleTimer.stop();
        return;
    }
    if (m_state == State::Closed)
        return;
    restartIdleTimer();
    // Not from inside the caller (often a slot of `received`): what waited comes next turn.
    QMetaObject::invokeMethod(this, [this] {
        if (!m_readPaused && m_state != State::Closed && m_transport)
            onReadyRead();
    }, Qt::QueuedConnection);
}

void SecureStream::restartIdleTimer()
{
    if (m_state == State::Established && !m_readPaused)
        m_idleTimer.start();
}

void SecureStream::onTransportEnded()
{
    if (m_state == State::Closed)
        return;
    // The peer ended the connection: what it sent before is delivered first (now, or when the
    // stream is resumed), then `closed`.
    m_remoteEnded = true;
    if (m_state == State::Closing || m_readPaused)
        return finishIfEnded();
    onReadyRead();
}

void SecureStream::finishIfEnded()
{
    if (!m_remoteEnded || m_state == State::Closed)
        return;
    if (m_readPaused && m_state != State::Closing)
        return;
    fail(QStringLiteral("Connection closed"));
}

void SecureStream::fail(const QString &reason)
{
    if (m_state == State::Closed)
        return;
    m_state = State::Closed;
    m_handshakeTimer.stop();
    m_idleTimer.stop();
    if (m_transport)
        m_transport->abort();
    emit closed(reason);
}

void SecureStream::onReadyRead()
{
    restartIdleTimer();
    if (m_silent) {
        m_silent = false;
        m_handshakeTimer.start(kHandshakeTimeoutMs); // the full handshake time, from the first byte
    }
    if (m_readPaused) {
        // Over TCP the socket's read buffer holds the sender long before this; a tunnel has no
        // flow control, so a receiver that stays behind ends the stream rather than its memory.
        if (m_buffer.size() + m_transport->bytesAvailable() > kMaxPausedBacklog)
            fail(QStringLiteral("The receiving disk is too slow for this connection"));
        return;
    }
    m_buffer += m_transport->readAll();
    processBuffer();
    finishIfEnded();
}

void SecureStream::processBuffer()
{
    while (m_buffer.size() >= 4 && m_state != State::Closed && m_state != State::Closing && !m_readPaused) {
        const quint32 len = qFromBigEndian<quint32>(m_buffer.constData());
        if (len > quint32(kMaxWireFrame)) {
            fail(QStringLiteral("Oversized frame"));
            return;
        }
        if (quint32(m_buffer.size()) < 4 + len)
            return;
        const QByteArray frame = m_buffer.mid(4, len);
        m_buffer.remove(0, 4 + len);
        handleFrame(frame);
    }
}

void SecureStream::handleFrame(const QByteArray &frame)
{
    switch (m_state) {
    case State::AwaitingInit: {
        noise::IKResponder responder(m_local, kPrologue);
        const auto remoteStatic = responder.readMessage1(frame, nullptr);
        const auto peerId = remoteStatic && m_trust ? m_trust(*remoteStatic) : std::nullopt;
        if (!peerId) {
            fail(QStringLiteral("Untrusted or invalid handshake"));
            return;
        }
        QByteArray message2;
        const auto keys = responder.writeMessage2({}, &message2);
        if (!keys) {
            fail(QStringLiteral("Handshake failed"));
            return;
        }
        m_peerId = *peerId;
        m_sendKey = keys->sendKey;
        m_recvKey = keys->recvKey;
        writeFrame(message2);
        m_state = State::AwaitingFirstFrame; // still under the handshake timeout
        break;
    }
    case State::AwaitingFirstFrame: {
        const auto plaintext = crypto::aeadDecrypt(m_recvKey, m_recvCounter++, {}, frame);
        if (!plaintext) {
            fail(QStringLiteral("Peer authentication failed"));
            return;
        }
        m_state = State::Established;
        m_handshakeTimer.stop();
        restartIdleTimer();
        m_firstFrame = *plaintext;
        emit established();
        // Taken over (moved to another stream) or discarded in a slot: not this stream's data.
        if (m_state == State::Established && !m_firstFrameDiscarded)
            emit received(*plaintext);
        m_firstFrame.clear();
        break;
    }
    case State::AwaitingResponse: {
        const auto keys = m_initiator->readMessage2(frame, nullptr);
        m_initiator.reset();
        if (!keys) {
            fail(QStringLiteral("Peer authentication failed"));
            return;
        }
        m_sendKey = keys->sendKey;
        m_recvKey = keys->recvKey;
        m_state = State::Established;
        m_handshakeTimer.stop();
        restartIdleTimer();
        emit established();
        break;
    }
    case State::Established: {
        const auto plaintext = crypto::aeadDecrypt(m_recvKey, m_recvCounter++, {}, frame);
        if (!plaintext) {
            fail(QStringLiteral("Corrupted or forged frame"));
            return;
        }
        emit received(*plaintext);
        break;
    }
    default:
        break;
    }
}

} // namespace df
