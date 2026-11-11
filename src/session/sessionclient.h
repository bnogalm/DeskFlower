#pragma once

#include "clipboardbackend.h"
#include "clipboardbridge.h"
#include "ipc.h"

#include <QCborArray>
#include <QElapsedTimer>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class InputBackend;
class ScreenStreamer;
class ServiceRequest;
class ViewerWindow;

// Connects the session helper to the service: reports this session's screens and thumbnails,
// forwards captured input and executes injected input.
class SessionClient : public QObject
{
    Q_OBJECT
public:
    SessionClient(const QString &instance, const QByteArray &token, InputBackend *input, QObject *parent = nullptr);
    ~SessionClient() override;

    // Replaces how the shared clipboard is read and written (a Wayland helper: the portal's).
    void setClipboardBackend(ClipboardBackend *backend) { m_clipboard.setBackend(backend); }

private:
    void connectToService();
    void onConnected();
    void onDisconnected();
    void onMessage(const QCborMap &msg);
    // The latest position from the service, applied once the messages read together are handled:
    // a helper that fell behind jumps there instead of replaying every move.
    void flushMove();
    void send(const QString &cmd, QCborMap args = {});
    void sendScreens();
    // The input backend's problem (InputBackend::problem), shown by the agents.
    void sendInputStatus();
    void captureThumbnails();
    void watchScreens();
    QPoint primaryCenter() const;
    // Remote view, source side: capture the listed screens (viewCapture: {i, g, sx, sy, sw, sh, w,
    // h} each), replacing the current set; a changed entry, or one given a new generation (g),
    // starts afresh (full frame).
    void setCaptures(const QCborArray &screens);
    void stopCaptures();
    // Remote view, viewer side: one window per listed local monitor (viewOpen/viewShow: {i, x, y,
    // w, h, rw, rh} each), bound to the remote screen it shows; the others close.
    void showViewers(const QCborArray &screens);
    void closeViewers();
    ViewerWindow *viewerFor(int index) const;
    // A request answered on a connection of its own with blocking reads (called from inside the
    // X11/Wayland/macOS clipboard request, no event loop): the reply event's map, empty on timeout
    // or if the service is not running (*error says).
    QCborMap requestFromService(const QString &cmd, QCborMap args, const QString &replyEvent, int timeoutMs,
                                QString *error) const;
    // Decrypts one sealed clipboard format for a paste. Empty on refusal or timeout.
    QByteArray unsealFromService(const QByteArray &kid, const QString &mime, QString *error) const;
    // The copied files of the sealed clipboard for a paste (clipboard `generation`): their local
    // paths, empty on refusal, failure, or while the transfer still runs. The transfer starts at
    // the first paste and goes on in the background (progress in the agent); the paste waits for
    // it only kPasteWaitMs, so that the application pasting never gives up on a long transfer
    // (Qt applications wait 5 s for the clipboard): the user is told to paste again once it ended.
    // A read that is no paste (no key or click just before, see ClipboardSync::fetchFiles) is
    // refused with ClipboardBridge::kNotAPaste and told to nobody. kid: the clipboard's id.
    QStringList pastedFiles(quint64 generation, const QByteArray &kid, int count, QString *error);
    void notice(const QString &text, bool problem);

    QString m_serverName;
    QString m_renderServerName; // pastes (clipUnseal, clipFetch), see ipc::renderServerName
    QByteArray m_token;
    InputBackend *m_input;
    QLocalSocket m_socket;
    df::ipc::Connection m_conn;
    QTimer m_reconnect;
    QTimer m_screensChanged;
    QTimer m_watchdog;
    QElapsedTimer m_sinceLastPing;
    qint64 m_lastCapture = -1; // df::nowMs() of the last thumbnail capture (-1: none)
    int m_connectAttempts = 0;
    std::optional<QPoint> m_pendingMove;
    bool m_moveFlushQueued = false;

    // Remote view: these screens streamed to the master / remote screens shown here.
    struct Capture {
        ScreenStreamer *streamer = nullptr;
        QRect screen;
        QSize maxSize;
        quint32 generation = 0;
    };
    QHash<int, Capture> m_captures; // by the index the viewer gave the screen
    std::vector<std::unique_ptr<ViewerWindow>> m_viewers; // one per local monitor in the set
    QString m_viewDevice; // hex id of the device shown in m_viewers
    // The last status of that view, for a window opened after it arrived.
    QString m_viewStatus;
    bool m_viewFailed = false;

    // Clipboard sharing, when the service gives the clipboard to this helper.
    ClipboardBridge m_clipboard;
    quint64 m_clipGeneration = 0; // one per clipSet: a paste fetch belongs to one clipboard
    struct PasteFetch {
        quint64 generation = 0;
        ServiceRequest *request = nullptr; // the clipFetch in progress
        qint64 startedAt = 0;              // df::nowMs()
        bool done = false;
        bool toldToWait = false; // the user was told to paste again once it ends
        QStringList files;
        QString error;
    };
    PasteFetch m_paste;
};
