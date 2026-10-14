#include "securestream.h"

#include <QtEndian>
#include <utility>

namespace df {

namespace {
const QByteArray kPrologue("deskflower/tcp/1");
constexpr int kMaxWireFrame = SecureStream::kMaxFrame + 1024;
} // namespace

SecureStream::SecureStream(QTcpSocket *socket, const crypto::KeyPair &local, QObject *parent)
    : QObject(parent), m_socket(socket), m_local(local)
{
    m_socket->setParent(this);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(m_socket, &QTcpSocket::readyRead, this, &SecureStream::onReadyRead);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &SecureStream::bytesWritten);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] { fail(QStringLiteral("Connection closed")); });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this] { fail(m_socket->errorString()); });

    m_handshakeTimer.setSingleShot(true);
    m_handshakeTimer.setInterval(kHandshakeTimeoutMs);
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this] { fail(QStringLiteral("Handshake timed out")); });
    m_handshakeTimer.start();

    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] { fail(QStringLiteral("Idle for too long")); });
    connect(m_socket, &QTcpSocket::bytesWritten, &m_idleTimer, qOverload<>(&QTimer::start));
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
    auto *stream = new SecureStream(new QTcpSocket, local, parent);
    stream->m_peerId = peerId;
    stream->m_initiator = std::make_unique<noise::IKInitiator>(local, remoteStatic, kPrologue);
    connect(stream->m_socket, &QTcpSocket::connected, stream, &SecureStream::onConnected);
    stream->m_socket->connectToHost(addr, port);
    return stream;
}

SecureStream *SecureStream::accept(QTcpSocket *socket, const crypto::KeyPair &local, TrustCheck trust,
                                   QObject *parent)
{
    auto *stream = new SecureStream(socket, local, parent);
    stream->m_state = State::AwaitingInit;
    stream->m_trust = std::move(trust);
    return stream;
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
    m_socket->write(len, 4);
    m_socket->write(frame);
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
    m_state = State::Closing;
    m_handshakeTimer.stop();
    m_socket->disconnectFromHost(); // flushes what is queued, then `disconnected` -> closed
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        fail(QStringLiteral("Closed"));
}

void SecureStream::fail(const QString &reason)
{
    if (m_state == State::Closed)
        return;
    m_state = State::Closed;
    m_handshakeTimer.stop();
    m_idleTimer.stop();
    m_socket->abort();
    emit closed(reason);
}

void SecureStream::onReadyRead()
{
    if (m_state == State::Established)
        m_idleTimer.start();
    m_buffer += m_socket->readAll();
    while (m_buffer.size() >= 4 && m_state != State::Closed && m_state != State::Closing) {
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
        m_idleTimer.start();
        emit established();
        if (m_state == State::Established)
            emit received(*plaintext);
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
        m_idleTimer.start();
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
