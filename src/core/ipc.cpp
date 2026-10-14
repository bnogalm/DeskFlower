#include "ipc.h"

#include <QCborValue>
#include <QtEndian>

namespace df::ipc {

namespace {
constexpr quint32 kMaxMessage = 16 * 1024 * 1024;
}

QString serverName(const QString &instance)
{
    return QStringLiteral("deskflower-%1").arg(instance.isEmpty() ? QStringLiteral("default") : instance);
}

Connection::Connection(QLocalSocket *socket, QObject *parent) : QObject(parent), m_socket(socket)
{
    connect(m_socket, &QLocalSocket::readyRead, this, &Connection::onReadyRead);
}

void Connection::send(const QCborMap &message)
{
    const QByteArray body = QCborValue(message).toCbor();
    char len[4];
    qToBigEndian<quint32>(quint32(body.size()), len);
    m_socket->write(len, 4);
    m_socket->write(body);
}

void Connection::onReadyRead()
{
    m_buffer += m_socket->readAll();
    while (m_buffer.size() >= 4) {
        const quint32 len = qFromBigEndian<quint32>(m_buffer.constData());
        if (len > kMaxMessage) {
            m_socket->abort();
            m_buffer.clear();
            return;
        }
        if (quint32(m_buffer.size()) < 4 + len)
            return;
        const QCborValue v = QCborValue::fromCbor(m_buffer.mid(4, len));
        m_buffer.remove(0, 4 + len);
        if (v.isMap())
            emit messageReceived(v.toMap());
    }
}

} // namespace df::ipc
