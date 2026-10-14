#pragma once

// Local IPC between the service and the agent: length-prefixed (u32 big-endian) CBOR maps over a
// QLocalSocket. Requests carry "cmd", events pushed by the service carry "ev".

#include <QCborMap>
#include <QLocalSocket>
#include <QObject>

namespace df::ipc {

QString serverName(const QString &instance);

class Connection : public QObject
{
    Q_OBJECT
public:
    explicit Connection(QLocalSocket *socket, QObject *parent = nullptr);

    QLocalSocket *socket() const { return m_socket; }
    void send(const QCborMap &message);

signals:
    void messageReceived(const QCborMap &message);

private:
    void onReadyRead();

    QLocalSocket *m_socket;
    QByteArray m_buffer;
};

} // namespace df::ipc
