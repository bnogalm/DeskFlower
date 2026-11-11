#pragma once

#include "platform/platform.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

class QWinEventNotifier;

namespace df {

// Keeps deskflower-session running in the interactive session and restarts it when it exits
// or when the active console session changes (fast user switching, logon). A helper that keeps
// exiting right away (no display yet, a portal that keeps failing...) is restarted less and less
// often, up to once a minute.
//  - Console mode / Linux: a child QProcess of the service.
//  - macOS: a child process too, but spawned as responsible for itself (posix_spawn, disclaiming
//    the responsibility), so that macOS asks for and records the privacy permissions
//    (Accessibility, Screen Recording) for the helper's own bundle, not for the service.
//  - Windows service mode: LocalSystem inside the active console session. When that session
//    changes, the helper is asked to end first (helperStopping: the service closes its pipe, on
//    which it puts the system cursors back and releases injected keys) and only terminated if it
//    is still there after kExitGraceMs, without ever blocking the service's event loop.
class SessionLauncher : public QObject
{
    Q_OBJECT
public:
    // logDir: where the helper writes session.log (empty: no log file). crashDir: where it keeps
    // its problems.log and minidumps (crashreport.h), whatever the logging setting.
    SessionLauncher(const QString &instance, const QString &logDir, const QString &crashDir, bool serviceMode,
                    QObject *parent = nullptr);
    ~SessionLauncher() override;

    // Secret the helper must present in its IPC hello.
    QByteArray token() const { return m_token; }
    void start();
    // Linux: the graphical session's variables (DISPLAY, WAYLAND_DISPLAY, XAUTHORITY, the
    // session bus...) as the agent sees them, laid over the service's own environment for the
    // helper; a running helper that got other values is restarted with these.
    void setSessionEnvironment(const QHash<QString, QString> &env);
    // Tests: another program than deskflower-session (empty: that one).
    void setProgram(const QString &program) { m_program = program; }

signals:
    // The console session changed: the helper must go (ServiceController closes its connection).
    void helperStopping();

private:
    void launch();
    void scheduleRestart();
    void stopNative(int graceMs = 0);
    static constexpr int kExitGraceMs = 2000;
    QString program() const;
    static QProcessEnvironment helperEnvironment(const QHash<QString, QString> &sessionEnv);
    QStringList arguments() const;

    QString m_instance;
    QString m_logDir;
    QString m_crashDir;
    bool m_serviceMode;
    QByteArray m_token;
    bool m_stopping = false;
    QTimer m_restart;
    int m_restartDelayMs;
    QElapsedTimer m_running; // since the current helper started
    QHash<QString, QString> m_sessionEnv; // from the agent (setSessionEnvironment)
    QProcessEnvironment m_helperEnv;      // what the running helper was given (without the token)
    QString m_program;

    QProcess m_process;
#ifdef Q_OS_MACOS
    qint64 m_pid = 0; // the helper spawned (0: none)
    QTimer m_reap;    // notices its exit (waitpid)
#endif

    platform::SessionProcess m_native;
    QWinEventNotifier *m_notifier = nullptr;
    QTimer m_sessionCheck;
    QTimer m_exitGrace; // the helper of a session no longer at the console has this long to leave
};

} // namespace df
