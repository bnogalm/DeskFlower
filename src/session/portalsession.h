#pragma once

// The xdg-desktop-portal remote desktop session of a Wayland session helper, over D-Bus:
// RemoteDesktop (keyboard and pointer injection through libei, ConnectToEIS), ScreenCast on the
// same session (every monitor, for the remote view and the thumbnails, through PipeWire) and the
// Clipboard interface (the only clipboard a background program gets on Wayland).
//
// The desktop asks the user once: the session is created with persist_mode 2 and the restore token
// the portal returns is kept (portal-restore-token in the helper's configuration folder), so the
// next sessions start without a dialog until the user revokes it in the desktop's settings.
//
// Sequence: CreateSession → SelectDevices (keyboard | pointer, persist_mode, restore_token) →
// ScreenCast.SelectSources (monitors) → Clipboard.RequestClipboard → Start → (ConnectToEIS,
// OpenPipeWireRemote on demand).

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <map>

class PortalSession : public QObject, protected QDBusContext
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Started, Failed, Closed };

    // A monitor shared by the ScreenCast part: its PipeWire node and its logical geometry.
    struct Stream {
        quint32 node = 0;
        QRect geometry;
    };

    // What the session asks for.
    struct Options {
        bool input = true;      // keyboard and pointer (RemoteDesktop devices)
        bool screens = true;    // every monitor (ScreenCast sources)
        bool clipboard = true;  // the portal clipboard
        QString tokenFile;      // where the restore token is kept (empty: the default)
    };

    explicit PortalSession(const Options &options, QObject *parent = nullptr);
    ~PortalSession() override;

    // The helper's session (created on first use).
    static PortalSession *instance();

    void start(); // idempotent while starting or started
    State state() const { return m_state; }
    // Why it failed or ended, for the user (empty while fine).
    QString problem() const { return m_problem; }
    // The user said no in the desktop's dialog (asking again would only bring it back).
    bool refused() const { return m_refused; }
    // The session ended (the user stopped sharing, the portal restarted) or failed for another
    // reason than a refusal: the helper starts over (the restore token avoids a new dialog).
    bool worthRestarting() const;
    // Whether the keyboard or the mouse was granted (the rest may be without them).
    bool inputGranted() const { return m_inputGranted; }
    QString handle() const { return m_session; }
    bool clipboardEnabled() const { return m_clipboardEnabled; }
    QList<Stream> streams() const { return m_streams; }
    // New file descriptors (the caller owns them), -1 on failure.
    int connectToEis();
    int openPipeWireRemote();

    static QString defaultTokenFile();

signals:
    void started();
    void failed(const QString &problem);
    void closed();

private slots:
    void onResponse(uint response, const QVariantMap &results);
    void onClosed(const QVariantMap &details);

private:
    using Callback = std::function<void(const QVariantMap &)>;
    // Calls a portal method returning a Request handle; `callback` gets the results of its
    // Response (anything but success fails the session with `step` in the message).
    void request(const QString &interface, const QString &method, QVariantList args, const QString &step,
                 Callback callback);
    void fail(const QString &problem);
    void selectDevices();
    void selectSources();
    void requestClipboard();
    void startSession();
    QString readToken() const;
    void writeToken(const QString &token) const;

    Options m_options;
    State m_state = State::Idle;
    QString m_problem;
    QString m_session;
    bool m_refused = false;
    bool m_inputGranted = false;
    bool m_clipboardEnabled = false;
    QList<Stream> m_streams;
    std::map<QString, std::pair<QString, Callback>> m_pending; // request path → (step, callback)
};
