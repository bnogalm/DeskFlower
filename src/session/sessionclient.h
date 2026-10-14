#pragma once

#include "clipboardbridge.h"
#include "ipc.h"

#include <QElapsedTimer>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

#include <memory>

class InputBackend;
class ScreenStreamer;
class ViewerWindow;

// Connects the session helper to the service: reports this session's screens and thumbnails,
// forwards captured input and executes injected input.
class SessionClient : public QObject
{
    Q_OBJECT
public:
    SessionClient(const QString &instance, const QByteArray &token, InputBackend *input, QObject *parent = nullptr);
    ~SessionClient() override;

private:
    void connectToService();
    void onConnected();
    void onDisconnected();
    void onMessage(const QCborMap &msg);
    void send(const QString &cmd, QCborMap args = {});
    void sendScreens();
    void captureThumbnails();
    void watchScreens();
    QPoint primaryCenter() const;

    QString m_serverName;
    QByteArray m_token;
    InputBackend *m_input;
    QLocalSocket m_socket;
    df::ipc::Connection m_conn;
    QTimer m_reconnect;
    QTimer m_screensChanged;
    QTimer m_watchdog;
    QElapsedTimer m_sinceLastPing;
    qint64 m_lastCapture = 0;
    int m_connectAttempts = 0;

    // Remote view: this screen streamed to the master / a remote screen shown here.
    ScreenStreamer *m_streamer;
    std::unique_ptr<ViewerWindow> m_viewer;
    QString m_viewDevice; // hex id of the device shown in m_viewer

    // Clipboard sharing, when the service gives the clipboard to this helper.
    ClipboardBridge m_clipboard;
};
