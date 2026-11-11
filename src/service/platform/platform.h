#pragma once

// OS integration for the service process:
//  - Windows: runs under the Service Control Manager as LocalSystem (--install/--uninstall/--service).
//  - Linux:   systemd user unit (--install writes ~/.config/systemd/user/deskflower.service).
//  - macOS:   LaunchAgent (--install writes ~/Library/LaunchAgents/com.deskflower.service.plist).
// Without any of these flags the service runs in the foreground (console mode).

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QLocalSocket;

namespace df::platform {

using Body = std::function<int(int argc, char **argv)>;

// A macOS LaunchAgent (~/Library/LaunchAgents/<label>.plist): what launchd starts at every sign-in
// of the user. Built here, not in the macOS-only code, so that it is tested everywhere.
struct LaunchAgent {
    QString label;
    QStringList arguments;             // the program first
    QList<QPair<QString, QString>> environment;
    bool keepAlive = false;            // started again unless it exits cleanly (the service)
    QString associatedBundle;          // shown under this app in System Settings › Login Items
    QString logPath;                   // standard error (empty: none)
};
QByteArray launchAgentPlist(const LaunchAgent &agent);

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

// Creates the configuration directory (and missing parents). In Windows service mode it is created
// in one step with a protected DACL (SYSTEM and Administrators), so there is no window between
// creation and securing in which another user could create a subfolder and keep ownership of it.
// Elsewhere a plain mkpath. secureDirectory() still runs afterwards to verify and enforce.
void createConfigDir(const QString &path, bool serviceMode);

// Home folder of the user logged on at the console (service mode: found through their token).
// Empty if nobody is logged on.
QString interactiveUserHome(bool serviceMode);

// Whether this service serves several users of the device (Windows service mode), so that each
// pairing belongs to the user who made it (Node::setLocalUser).
bool separatesUsers(bool serviceMode);
// Who is signed in at the console now, as a stable id (Windows: the user's SID). Empty if nobody
// is (or when the service does not separate users).
QString interactiveUserId(bool serviceMode);

// Whether `path` may be the shared folder (sharedFolderProblem). Windows service mode: inside the
// console user's home, and reached without links, because the service writes and serves files there
// as LocalSystem. Otherwise also outside the home folder (the service runs as the user), but never a
// system folder. `why` says what is wrong.
bool isAllowedSharedFolder(const QString &path, bool serviceMode, QString *why = nullptr);
// The rule behind it, without the file system: why `path` may not be shared by the user whose
// home folder is `home` (empty if it may). Never the whole home folder, a hidden folder, AppData
// (Windows) or Library (macOS), a whole drive or a system folder; with insideHomeOnly, nothing
// outside `home`.
QString sharedFolderProblem(const QString &path, const QString &home, bool insideHomeOnly);

// File access with the console user's rights for as long as the returned guard lives. Windows
// service mode: impersonates that user on the calling thread; null if nobody is logged on (or it
// failed). Elsewhere a guard that does nothing: the service already runs as the user.
// Nested guards put back what the thread was before. Looking up the user (interactiveUserHome,
// interactiveUserId) works while acting as the user: it is done with the service's own rights.
std::shared_ptr<void> actAsInteractiveUser(bool serviceMode);
// Who the calling thread acts as while impersonating (actAsInteractiveUser), as a stable id
// (Windows: the SID of the thread's token). Empty when the thread impersonates nobody, and on the
// other platforms. FileTransfer::setUserIdentity: a transfer that sees it change stops.
QString impersonatedUserId();

// The shared folder (phone uploads; what phones see): `chosen` if it is
// allowed (isAllowedSharedFolder), else <console user's home>/Deskflower, else `fallback` when
// nobody is signed in. Created with the console user's rights.
QString sharedFolder(const QString &chosen, const QString &fallback, bool serviceMode);

// Where files received from paired devices wait (copied files: Clipboard/, dragged files:
// Dragged/), apart from the shared folder so that phones never see them: a hidden folder of the
// console user (Windows: <home>/AppData/Local/Deskflower/Received; Linux: ~/.cache/deskflower/received;
// macOS: ~/Library/Caches/Deskflower/Received), else `fallback` when nobody is signed in. Created
// with the console user's rights.
QString receivedFolder(const QString &fallback, bool serviceMode);

// Tests: where the console user's token comes from in Windows service mode (by default
// WTSQueryUserToken, which only works with the service's own rights). The source returns a token
// handle the caller closes, or null. An empty function puts the default back.
using UserTokenSource = std::function<void *()>;
void setUserTokenSourceForTests(UserTokenSource source);

// Restricts a directory to the account running the process: on Windows LocalSystem and
// Administrators in service mode, otherwise the owner (the current user). False if that failed, or
// (Windows service mode) if the directory is a link or is owned by another account: anyone may
// create folders under ProgramData, and an owner can always give itself access again.
bool secureDirectory(const QString &path, bool serviceMode);

// Whether an IPC client may control this service. In Windows service mode the pipe is reachable by
// every local user, so only processes in the active console session are accepted.
bool isClientAllowed(QLocalSocket *socket, bool serviceMode);

// The Windows session an IPC client's process belongs to. Preferably through
// ImpersonateNamedPipeClient + the client token's session id (robust against PID reuse), which the
// pipe only allows once something has been read from it, so the service asks at connect time (the
// GetNamedPipeClientProcessId fallback answers then) and again at the client's first message.
// kNoSession when it cannot be told, or outside Windows service mode. The service stores it and,
// on every console-session change, drops the clients that no longer belong to the active console
// session (a fast-user switch leaves the previous user's agent otherwise receiving the new user's
// clipboard, PINs and thumbnails).
quint32 clientSessionId(QLocalSocket *socket, bool serviceMode);

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
// Waits up to graceMs for the helper to end on its own (it does once the service's pipe closes,
// putting the cursor and the keyboard back first), then terminates it.
void terminateSessionProcess(SessionProcess *process, int graceMs = 0);

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
// --uninstall. macOS: a LaunchAgent of the user (com.deskflower.agent) that starts the agent of the
// same bundle at the next sign-in (only for a service inside DeskFlower.app). Elsewhere not supported here (Linux packages install an XDG
// autostart entry).
bool agentAutostartSupported(bool serviceMode);

// Which process shares the user's clipboard: the agent (true) or the session helper. On Windows in
// service mode the helper runs as LocalSystem, and Windows does not reliably give such a process
// the user's clipboard (changes are not seen, pasted files need calls into the owner); the agent
// runs as the user. Elsewhere the helper runs as the user anyway.
bool clipboardInAgent(bool serviceMode);
void setAgentAutostart(bool on);

// Windows service mode: the inbound firewall rule for public networks (added by --install) lets
// in the local subnet only; with `on`, also the other private ranges (10.0.0.0/8, 172.16.0.0/12,
// 192.168.0.0/16, the carrier-grade NAT range 100.64.0.0/10 and link-local 169.254.0.0/16), so that
// a paired device on another network of the premises (behind a second router, a VM host's
// network) can connect too. Every connection still authenticates. Applied in the background.
// Elsewhere the OS firewall is the user's to configure (not supported).
// Only the console session's clients reach the service (isClientAllowed), so the signed-in
// user decides, as with the other machine-wide settings (agentAutostart). Changes run one at a
// time; the last one asked for wins.
bool firewallScopeSupported(bool serviceMode);
void setFirewallOtherSubnets(bool on);
#ifdef Q_OS_WIN
// The rule's remoteip list (netsh syntax).
QString firewallPublicRemoteIps(bool otherSubnets);
#endif

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
