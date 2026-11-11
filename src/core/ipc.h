#pragma once

// Local IPC between the service and the agent: length-prefixed (u32 big-endian) CBOR maps over a
// QLocalSocket. Requests carry "cmd", events pushed by the service carry "ev".

#include <QCborMap>
#include <QLocalSocket>
#include <QObject>

namespace df::ipc {

QString serverName(const QString &instance);
// A second server for the owner of the local clipboard to render a paste (clipUnseal, clipFetch):
// a connection there gets only the answer it asks for, not the state, prompts and thumbnails every
// connection to serverName() is sent first (clipboard readers can ask many times per second).
QString renderServerName(const QString &instance);

// How long the owner of the local clipboard waits for a sealed format: longer than the service's
// own wait for the master (ClipboardSync::kUnsealTimeoutMs plus the master's kKeyWaitMs), so a late
// grant is not lost to a render that gave up first ("the first paste fails, the second works").
inline constexpr int kClipboardRenderTimeoutMs = 5000;

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
