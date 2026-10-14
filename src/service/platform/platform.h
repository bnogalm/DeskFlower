#pragma once

// OS integration for the service process:
//  - Windows: runs under the Service Control Manager as LocalSystem (--install/--uninstall/--service).
//  - Linux:   systemd user unit (--install writes ~/.config/systemd/user/deskflower.service).
//  - macOS:   LaunchAgent (--install writes ~/Library/LaunchAgents/com.deskflower.service.plist).
// Without any of these flags the service runs in the foreground (console mode).

#include <QObject>
#include <QString>
#include <functional>

class QLocalSocket;

namespace df::platform {

using Body = std::function<int(int argc, char **argv)>;

// Registers the service with the OS service manager and starts it. Returns a process exit code.
int install(QString *message);
int uninstall(QString *message);

// Runs body under the OS service manager (Windows SCM). On other platforms body runs directly,
// because systemd/launchd manage plain foreground processes.
int runService(int argc, char **argv, const Body &body);

// Called by the body once it is fully initialised and about to enter the event loop.
void notifyStarted();

// Quits the Qt event loop on SIGINT/SIGTERM (Unix) or Ctrl+C / console close (Windows).
void installQuitHandlers();

// Where identity, trusted peers and layout live.
//  - Windows service: %ProgramData%\Deskflower, restricted to SYSTEM and Administrators.
//  - otherwise: the per-user application config location.
QString defaultConfigDir(bool serviceMode);

// Home folder of the user logged on at the console (service mode: found through their token).
// Empty if nobody is logged on.
QString interactiveUserHome(bool serviceMode);

// Restricts a directory to the account running the process: on Windows LocalSystem and
// Administrators in service mode, otherwise the owner (the current user). False if that failed, or
// (Windows service mode) if the directory is a link or is owned by another account: anyone may
// create folders under ProgramData, and an owner can always give itself access again.
bool secureDirectory(const QString &path, bool serviceMode);

// Whether an IPC client may control this service. In Windows service mode the pipe is reachable by
// every local user, so only processes in the active console session are accepted.
bool isClientAllowed(QLocalSocket *socket, bool serviceMode);

// Windows service mode: the session helper runs as LocalSystem inside the active console session
// (on winsta0\default; it follows the input desktop to reach UAC and the lock screen).
struct SessionProcess {
    void *handle = nullptr;
    quint32 sessionId = 0xFFFFFFFF;
};
constexpr quint32 kNoSession = 0xFFFFFFFF;
quint32 activeConsoleSession();
bool launchInActiveSession(const QString &program, const QStringList &args, const QByteArray &envName,
                           const QByteArray &envValue, SessionProcess *out, QString *error);
void terminateSessionProcess(SessionProcess *process);

// Installs a verified update package with the OS installer, detached so that it outlives this
// process (installing stops and replaces the service). Windows: "msiexec /i <msi> /qn", which needs
// the service's LocalSystem rights, so console mode cannot do it. Other platforms: not yet.
// onExit is called (on this thread) if the installer ends while this process still runs.
bool canInstallPackages(bool serviceMode);
bool launchPackageInstaller(const QString &package, const QString &logPath,
                            std::function<void(bool ok, const QString &error)> onExit, QString *error);

// Windows service mode: starts the agent (deskflower-agent --minimized) as the user logged on at the
// console, unless it already runs there. Used after an update, which asks agents to quit so that
// their files can be replaced. Elsewhere the agent is not affected by updates.
void relaunchAgent();

// Whether the agent starts at every sign-in. Windows service mode: a machine-wide Run entry that the
// service writes (not the installer), so that upgrades keep the user's choice; removed on
// --uninstall. Elsewhere not supported here (Linux packages install an XDG autostart entry).
bool agentAutostartSupported(bool serviceMode);

// Which process shares the user's clipboard: the agent (true) or the session helper. On Windows in
// service mode the helper runs as LocalSystem, and Windows does not reliably give such a process
// the user's clipboard (changes are not seen, pasted files need calls into the owner); the agent
// runs as the user. Elsewhere the helper runs as the user anyway.
bool clipboardInAgent(bool serviceMode);
void setAgentAutostart(bool on);

// Session notifications from the OS (Windows: logon, logoff, lock, unlock, console switch).
class SessionEvents : public QObject
{
    Q_OBJECT
public:
    enum Event {
        ConsoleConnect = 1,
        ConsoleDisconnect = 2,
        RemoteConnect = 3,
        RemoteDisconnect = 4,
        Logon = 5,
        Logoff = 6,
        Lock = 7,
        Unlock = 8,
    };
    Q_ENUM(Event)

    static SessionEvents *instance();

signals:
    void sessionChanged(df::platform::SessionEvents::Event event, quint32 sessionId);
};

} // namespace df::platform
