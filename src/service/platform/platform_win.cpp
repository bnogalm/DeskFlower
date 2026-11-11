#include "platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QWinEventNotifier>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <atomic>
#include <optional>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(lcPlatform, "deskflower.platform")

namespace df::platform {

namespace {

const wchar_t kServiceName[] = L"Deskflower";
const wchar_t kDisplayName[] = L"DeskFlower";
const wchar_t kDescription[] = L"Shares keyboard, mouse and clipboard with paired computers on the local network.";
const QString kFirewallRule = QStringLiteral("Deskflower");
const QString kPublicFirewallRule = QStringLiteral("Deskflower (public networks, local subnet)");
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"Deskflower";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
Body g_body;
int g_argc = 0;
char **g_argv = nullptr;
int g_exitCode = 0;
std::atomic_bool g_stopRequested = false;

QString lastErrorString(DWORD error = GetLastError())
{
    wchar_t *buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    const QString text = buffer ? QString::fromWCharArray(buffer).trimmed() : QString::number(error);
    LocalFree(buffer);
    return QStringLiteral("%1 (error %2)").arg(text).arg(error);
}

void reportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0)
{
    static DWORD checkPoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode == NO_ERROR ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    g_status.dwServiceSpecificExitCode = exitCode;
    g_status.dwWaitHint = waitHintMs;
    g_status.dwControlsAccepted =
        state == SERVICE_START_PENDING ? 0
                                       : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    if (g_statusHandle)
        SetServiceStatus(g_statusHandle, &g_status);
}

void requestQuit()
{
    g_stopRequested = true;
    if (QCoreApplication *app = QCoreApplication::instance())
        QMetaObject::invokeMethod(app, &QCoreApplication::quit, Qt::QueuedConnection);
}

DWORD WINAPI controlHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        requestQuit();
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE: {
        const auto *n = static_cast<WTSSESSION_NOTIFICATION *>(eventData);
        const quint32 sessionId = n ? n->dwSessionId : 0;
        const auto event = SessionEvents::Event(eventType);
        SessionEvents *events = SessionEvents::instance();
        QMetaObject::invokeMethod(events, [events, event, sessionId] { emit events->sessionChanged(event, sessionId); },
                                  Qt::QueuedConnection);
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, controlHandler, nullptr);
    if (!g_statusHandle)
        return;
    reportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);
    // Qt runs on this thread: it becomes the application's main thread.
    g_exitCode = g_body(g_argc, g_argv);
    reportStatus(SERVICE_STOPPED, DWORD(g_exitCode));
}

BOOL WINAPI consoleCtrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        requestQuit();
        return TRUE;
    default:
        return FALSE;
    }
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE handle) : h(handle) {}
    ~ScHandle()
    {
        if (h)
            CloseServiceHandle(h);
    }
    operator SC_HANDLE() const { return h; }
};

QString executablePath() { return QDir::toNativeSeparators(QCoreApplication::applicationFilePath()); }

void runNetsh(const QStringList &args)
{
    QProcess p;
    p.start(QStringLiteral("netsh"), args);
    p.waitForFinished(10000);
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        qCWarning(lcPlatform).noquote() << "netsh" << args.join(QLatin1Char(' ')) << "failed:"
                                        << QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

void removeFirewallRules()
{
    for (const QString &rule : {kFirewallRule, kPublicFirewallRule})
        runNetsh({QStringLiteral("advfirewall"), QStringLiteral("firewall"), QStringLiteral("delete"),
                  QStringLiteral("rule"), QStringLiteral("name=%1").arg(rule)});
}

// A service has no desktop to show the firewall prompt on: add the rules explicitly (UDP for
// discovery and sessions, TCP for file transfers and phone access). Windows marks many home Wi-Fi
// networks as public, so they are covered too, but only for addresses on the local subnet (other
// computers, phones on the same Wi-Fi) unless the user allows other subnets; every connection
// still has to authenticate.
void addFirewallRules()
{
    removeFirewallRules();
    const QString program = QStringLiteral("program=%1").arg(executablePath());
    runNetsh({QStringLiteral("advfirewall"), QStringLiteral("firewall"), QStringLiteral("add"), QStringLiteral("rule"),
              QStringLiteral("name=%1").arg(kFirewallRule), QStringLiteral("dir=in"), QStringLiteral("action=allow"),
              program, QStringLiteral("profile=private,domain"), QStringLiteral("enable=yes")});
    runNetsh({QStringLiteral("advfirewall"), QStringLiteral("firewall"), QStringLiteral("add"), QStringLiteral("rule"),
              QStringLiteral("name=%1").arg(kPublicFirewallRule), QStringLiteral("dir=in"),
              QStringLiteral("action=allow"), program, QStringLiteral("profile=public"),
              QStringLiteral("remoteip=%1").arg(firewallPublicRemoteIps(false)), QStringLiteral("enable=yes")});
}

bool waitForState(SC_HANDLE service, DWORD state, int timeoutMs)
{
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    for (int waited = 0; waited <= timeoutMs; waited += 250) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                  &needed))
            return false;
        if (ssp.dwCurrentState == state)
            return true;
        QThread::msleep(250);
    }
    return false;
}

// Terminates `pid` and the processes it started (the session helpers), and waits for them to end.
void terminateProcessTree(DWORD pid)
{
    if (pid == 0 || pid == GetCurrentProcessId())
        return;
    std::vector<DWORD> children;
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
            if (entry.th32ParentProcessID == pid && entry.th32ProcessID != GetCurrentProcessId())
                children.push_back(entry.th32ProcessID);
        }
        CloseHandle(snapshot);
    }
    children.insert(children.begin(), pid);
    for (const DWORD id : children) {
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id)) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
    }
}

// The process of a running (or stopping) service, 0 when it has none.
DWORD serviceProcessId(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed))
        return 0;
    return ssp.dwCurrentState == SERVICE_STOPPED ? 0 : ssp.dwProcessId;
}

// Stops the service for an uninstall or an upgrade, whatever its state: the failure actions go first
// (a service that crashes while stopping would otherwise be restarted by the SCM a few seconds later,
// and outlive the uninstall), then a stop request, and if it does not stop in time, its process.
// `service` needs SERVICE_CHANGE_CONFIG, SERVICE_STOP and SERVICE_QUERY_STATUS.
void stopServiceForGood(SC_HANDLE service, int timeoutMs)
{
    SC_ACTION none = {SC_ACTION_NONE, 0};
    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.cActions = 0;
    failure.lpsaActions = &none; // non-null with no actions: removes them
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    SERVICE_STATUS status = {};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status) && waitForState(service, SERVICE_STOPPED, timeoutMs))
        return;
    if (const DWORD pid = serviceProcessId(service)) {
        qCWarning(lcPlatform) << "The service did not stop: ending its process" << pid;
        terminateProcessTree(pid);
        waitForState(service, SERVICE_STOPPED, 5000);
    }
}

// After DeleteService: the service is gone once its process ends and the last handle closes. A
// restart the SCM had already scheduled may still start it meanwhile: end such a process too.
void waitUntilServiceGone(SC_HANDLE scm, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 250) {
        ScHandle service(OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS));
        if (!service)
            return;
        if (const DWORD pid = serviceProcessId(service))
            terminateProcessTree(pid);
        QThread::msleep(250);
    }
}

// Opens and updates the existing service, or creates it. A service marked for deletion (the previous
// version's, while an upgrade removes it) disappears once its last handle is closed and its process
// ended: end that process if it still runs (an older uninstaller can leave it running), and wait.
SC_HANDLE createOrUpdateService(SC_HANDLE scm, const std::wstring &command, QString *message)
{
    for (int waited = 0; waited <= 30000; waited += 500) {
        if (waited > 0)
            QThread::msleep(500);
        DWORD error = NO_ERROR;
        if (SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS)) {
            stopServiceForGood(service, 15000);
            if (ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                     command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kDisplayName))
                return service;
            error = GetLastError();
            CloseServiceHandle(service);
            *message = QStringLiteral("Cannot update the service: %1").arg(lastErrorString(error));
        } else {
            service = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                     SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr,
                                     nullptr, nullptr /* LocalSystem */, nullptr);
            if (service)
                return service;
            error = GetLastError();
            *message = QStringLiteral("Cannot create the service: %1").arg(lastErrorString(error));
        }
        if (error != ERROR_SERVICE_MARKED_FOR_DELETE)
            return nullptr;
    }
    return nullptr;
}

} // namespace

int install(QString *message)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (!scm) {
        *message = QStringLiteral("Cannot open the Service Control Manager: %1. Run this from an elevated prompt.")
                       .arg(lastErrorString());
        return 1;
    }
    const std::wstring command = (QStringLiteral("\"%1\" --service").arg(executablePath())).toStdWString();
    // Idempotent: an existing service (previous version, other folder) is updated and restarted.
    ScHandle service(createOrUpdateService(scm, command, message));
    if (!service)
        return 1;

    SERVICE_DESCRIPTIONW description = {const_cast<wchar_t *>(kDescription)};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    // The default service permissions, plus starting it for signed-in users: the agent starts the
    // service again after the user quit DeskFlower from the tray (which stops it). Stopping stays
    // with administrators, and with the agent of the console session through the service's pipe.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)"
            L"(A;;CCLCSWLOCRRC;;;SU)",
            SDDL_REVISION_1, &sd, nullptr)) {
        if (!SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, sd))
            qCWarning(lcPlatform) << "Cannot let users start the service:" << lastErrorString();
        LocalFree(sd);
    }

    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 3000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.dwResetPeriod = 24 * 3600;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    addFirewallRules();

    // Not fatal: the installer would roll back a whole upgrade over a slow start. The service starts
    // again by itself (failure actions, next boot).
    if (!StartServiceW(service, 0, nullptr)) {
        *message = QStringLiteral("Service installed, but it did not start: %1").arg(lastErrorString());
        return 0;
    }
    waitForState(service, SERVICE_RUNNING, 10000);
    *message = QStringLiteral("Service installed and started.");
    return 0;
}

int uninstall(QString *message)
{
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        *message = QStringLiteral("Cannot open the Service Control Manager: %1").arg(lastErrorString());
        return 1;
    }
    {
        ScHandle service(
            OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG | DELETE));
        if (!service) {
            const DWORD error = GetLastError();
            if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
                *message = QStringLiteral("The service is not installed.");
                return 0; // nothing to do (uninstallers call this unconditionally)
            }
            *message = QStringLiteral("Cannot open the service: %1").arg(lastErrorString(error));
            return 1;
        }
        stopServiceForGood(service, 15000);
        if (!DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
            *message = QStringLiteral("Cannot delete the service: %1").arg(lastErrorString());
            return 1;
        }
    }
    // Our handle is closed: the service disappears once no process of it runs.
    waitUntilServiceGone(scm, 10000);
    removeFirewallRules();
    setAgentAutostart(false);
    *message = QStringLiteral("Service removed.");
    return 0;
}

int runService(int argc, char **argv, const Body &body)
{
    g_body = body;
    g_argc = argc;
    g_argv = argv;
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t *>(kServiceName), serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        fprintf(stderr, "Not started by the Service Control Manager. Use --install, or run without --service.\n");
        return 1;
    }
    return g_exitCode;
}

void notifyStarted()
{
    if (g_statusHandle)
        reportStatus(SERVICE_RUNNING);
    if (g_stopRequested)
        requestQuit(); // a stop arrived before the event loop existed
}

void installQuitHandlers() { SetConsoleCtrlHandler(consoleCtrlHandler, TRUE); }

QString defaultConfigDir(bool serviceMode)
{
    if (!serviceMode)
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QString programData = qEnvironmentVariable("ProgramData");
    if (programData.isEmpty())
        programData = QStringLiteral("C:/ProgramData");
    return QDir(programData).filePath(QStringLiteral("Deskflower"));
}

void createConfigDir(const QString &path, bool serviceMode)
{
    if (!serviceMode) {
        QDir().mkpath(path);
        return;
    }
    // Build the protected DACL (SYSTEM and Administrators only) that the folder must have, and
    // create every missing component with it in one step, so a pre-created folder cannot linger
    // owned by whoever made it. The owner of a folder a LocalSystem service creates is SYSTEM.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                                                              &sd, nullptr)) {
        QDir().mkpath(path); // secureDirectory() still has to pass, or the service refuses to start
        return;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    const QString clean = QDir::cleanPath(path);
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString grown;
    for (const QString &part : parts) {
        grown += (grown.isEmpty() && !clean.startsWith(QLatin1Char('/'))) ? part : QStringLiteral("/") + part;
        if (part.endsWith(QLatin1Char(':')))
            continue; // the drive letter ("C:") is not a directory to create
        const std::wstring native = QDir::toNativeSeparators(grown).toStdWString();
        // The intermediate %ProgramData% exists already; only the ones we make get our descriptor.
        CreateDirectoryW(native.c_str(), &sa);
    }
    LocalFree(sd);
}

bool secureDirectory(const QString &path, bool serviceMode)
{
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    if (serviceMode) {
        // Created before us by another user (anyone may create folders in ProgramData), it would stay
        // theirs: an owner can always rewrite the DACL. A link could lead anywhere.
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            qCCritical(lcPlatform) << path << "is missing or is a link";
            return false;
        }
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR current = nullptr;
        const DWORD rc = GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                               nullptr, nullptr, nullptr, &current);
        const bool trusted = rc == ERROR_SUCCESS
                             && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
        LocalFree(current);
        if (!trusted) {
            qCCritical(lcPlatform).noquote() << QDir::toNativeSeparators(path)
                                             << "is owned by another account: delete it, then start DeskFlower again";
            return false;
        }
    }
    // Protected DACL inherited by children. Service: LocalSystem and Administrators only.
    // Console: the owner (OW = Owner Rights) plus LocalSystem.
    const wchar_t *sddl = serviceMode ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)" : L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    bool ok = false;
    if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
        const DWORD rc = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, dacl, nullptr);
        ok = rc == ERROR_SUCCESS;
        if (!ok)
            qCWarning(lcPlatform) << "Cannot restrict" << path << lastErrorString(rc);
    }
    LocalFree(sd);
    if (ok && serviceMode) {
        // A subfolder a user slipped in before we secured the parent keeps its own owner (the
        // protected DACL above is inherited, but ownership is not rewritten, and an owner can grant
        // itself access again). The service writes logs and installs updates under these, so refuse
        // to start if one exists and is a link or is owned by another account.
        for (const char *sub : {"logs", "updates"}) {
            const QString child = QDir(path).filePath(QString::fromLatin1(sub));
            const std::wstring childNative = QDir::toNativeSeparators(child).toStdWString();
            const DWORD attributes = GetFileAttributesW(childNative.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
                continue; // not there yet: it will be created under the protected parent
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                qCCritical(lcPlatform).noquote() << QDir::toNativeSeparators(child) << "is a link: delete it, then start DeskFlower again";
                return false;
            }
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR current = nullptr;
            const DWORD rc = GetNamedSecurityInfoW(childNative.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                                   &owner, nullptr, nullptr, nullptr, &current);
            const bool trusted = rc == ERROR_SUCCESS
                                 && (IsWellKnownSid(owner, WinLocalSystemSid)
                                     || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
            LocalFree(current);
            if (!trusted) {
                qCCritical(lcPlatform).noquote() << QDir::toNativeSeparators(child)
                                                 << "is owned by another account: delete it, then start DeskFlower again";
                return false;
            }
        }
    }
    return ok;
}

bool isClientAllowed(QLocalSocket *socket, bool serviceMode)
{
    if (!serviceMode)
        return true; // the pipe is already restricted to the current user
    ULONG pid = 0;
    DWORD sessionId = 0;
    const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !ProcessIdToSessionId(pid, &sessionId))
        return false;
    const DWORD console = WTSGetActiveConsoleSessionId();
    if (sessionId != console) {
        qCWarning(lcPlatform) << "Rejected IPC client pid" << pid << "from session" << sessionId
                              << "(active console session is" << console << ")";
        return false;
    }
    return true;
}

quint32 clientSessionId(QLocalSocket *socket, bool serviceMode)
{
    if (!serviceMode)
        return kNoSession;
    const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
    // Preferred: the client's own token, which cannot be spoofed by PID reuse.
    quint32 session = kNoSession;
    if (ImpersonateNamedPipeClient(pipe)) {
        HANDLE token = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
            DWORD id = 0;
            DWORD size = 0;
            if (GetTokenInformation(token, TokenSessionId, &id, sizeof(id), &size))
                session = id;
            CloseHandle(token);
        }
        RevertToSelf();
    } else {
        // Before anything was read from the pipe this is the usual outcome (ERROR_CANNOT_IMPERSONATE).
        qCDebug(lcPlatform) << "Cannot impersonate the IPC client:" << lastErrorString()
                            << "(its session comes from its process id)";
    }
    if (session != kNoSession)
        return session;
    // Fall back to the connect-time PID if impersonation was not possible.
    ULONG pid = 0;
    DWORD sessionId = 0;
    if (GetNamedPipeClientProcessId(pipe, &pid) && ProcessIdToSessionId(pid, &sessionId))
        return sessionId;
    return kNoSession;
}

quint32 activeConsoleSession() { return WTSGetActiveConsoleSessionId(); }

namespace {
// The thread's impersonation token, if any (to put back later).
HANDLE currentThreadToken()
{
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY, TRUE, &token))
        return nullptr;
    return token;
}

// The service's own rights for the lifetime of this object, also on a thread that acts as the
// user (actAsInteractiveUser): WTSQueryUserToken needs them, and failing there made the phone
// server lose the shared folder.
class AsService
{
public:
    AsService() : m_token(currentThreadToken())
    {
        if (m_token)
            RevertToSelf();
    }
    ~AsService()
    {
        if (m_token) {
            SetThreadToken(nullptr, m_token);
            CloseHandle(m_token);
        }
    }
    AsService(const AsService &) = delete;
    AsService &operator=(const AsService &) = delete;

private:
    HANDLE m_token;
};

UserTokenSource &userTokenSource()
{
    static UserTokenSource source;
    return source;
}

// The console user's primary token (closed by the caller), with the service's own rights.
HANDLE consoleUserToken()
{
    AsService self;
    if (const UserTokenSource &source = userTokenSource())
        return source();
    HANDLE token = nullptr;
    return WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token) ? token : nullptr;
}
} // namespace

void setUserTokenSourceForTests(UserTokenSource source) { userTokenSource() = std::move(source); }

QString interactiveUserHome(bool serviceMode)
{
    if (!serviceMode)
        return QDir::homePath();
    const HANDLE token = consoleUserToken();
    if (!token)
        return {};
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = GetUserProfileDirectoryW(token, path, &size);
    CloseHandle(token);
    return ok ? QDir::fromNativeSeparators(QString::fromWCharArray(path)) : QString();
}

bool separatesUsers(bool serviceMode) { return serviceMode; }

namespace {
// The user SID of `token`, as text ("S-1-5-21-..."); empty if it cannot be read.
QString tokenUserSid(HANDLE token)
{
    QString sid;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size);
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
            sid = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    return sid;
}
} // namespace

QString interactiveUserId(bool serviceMode)
{
    if (!serviceMode)
        return {};
    const HANDLE token = consoleUserToken();
    if (!token)
        return {};
    const QString sid = tokenUserSid(token);
    CloseHandle(token);
    return sid;
}

QString impersonatedUserId()
{
    // The thread's impersonation token, opened with the service's own rights (as currentThreadToken);
    // none when the thread impersonates nobody.
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
        return {};
    const QString sid = tokenUserSid(token);
    CloseHandle(token);
    return sid;
}

std::shared_ptr<void> actAsInteractiveUser(bool serviceMode)
{
    if (!serviceMode)
        return std::make_shared<int>(0);
    // Nested calls (the phone server acts as the user, then looks up the shared folder, which
    // creates it as the user too) put back whatever the thread was before.
    HANDLE previous = currentThreadToken();
    const HANDLE token = consoleUserToken();
    BOOL ok = token != nullptr;
    if (ok) {
        ok = ImpersonateLoggedOnUser(token);
        CloseHandle(token); // the thread keeps its own impersonation token
        if (!ok)
            qCWarning(lcPlatform) << "Cannot act as the signed-in user:" << lastErrorString();
    }
    if (!ok) {
        if (previous)
            CloseHandle(previous);
        return nullptr;
    }
    return std::shared_ptr<void>(previous ? previous : INVALID_HANDLE_VALUE, [](void *p) {
        if (p != INVALID_HANDLE_VALUE) {
            SetThreadToken(nullptr, p);
            CloseHandle(p);
        } else {
            RevertToSelf();
        }
    });
}

bool canInstallPackages(bool serviceMode) { return serviceMode; }

bool launchPackageInstaller(const QString &package, const QString &logPath,
                            std::function<void(bool ok, const QString &error)> onExit, QString *error)
{
    if (!package.endsWith(QLatin1String(".msi"), Qt::CaseInsensitive)) {
        *error = QStringLiteral("Not an MSI package");
        return false;
    }
    wchar_t system[MAX_PATH] = {};
    if (!GetSystemDirectoryW(system, MAX_PATH)) {
        *error = QStringLiteral("GetSystemDirectory: %1").arg(lastErrorString());
        return false;
    }
    // Full path: never whatever "msiexec" the search path finds. Its own process, so it keeps going
    // when the package's custom actions stop this service; the MajorUpgrade replaces the old version.
    const QString msiexec =
        QDir::toNativeSeparators(QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("msiexec.exe")));
    QString commandLine =
        QStringLiteral("\"%1\" /i \"%2\" /qn /norestart").arg(msiexec, QDir::toNativeSeparators(package));
    if (!logPath.isEmpty()) // only with logging on
        commandLine += QStringLiteral(" /l*v \"%1\"").arg(QDir::toNativeSeparators(logPath));
    const std::wstring application = msiexec.toStdWString();
    std::wstring cmd = commandLine.toStdWString();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(application.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        *error = QStringLiteral("Cannot start msiexec: %1").arg(lastErrorString());
        return false;
    }
    CloseHandle(pi.hThread);
    qCInfo(lcPlatform).noquote() << "Started" << commandLine;

    // Normally the installer stops this service before it ends; if not, report how it ended.
    HANDLE process = pi.hProcess;
    auto *notifier = new QWinEventNotifier(process, QCoreApplication::instance());
    QObject::connect(notifier, &QWinEventNotifier::activated, notifier, [notifier, process, onExit] {
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        notifier->setEnabled(false);
        notifier->deleteLater();
        CloseHandle(process);
        qCInfo(lcPlatform) << "msiexec exited with" << code;
        // 3010 and 1641: installed, a restart completes it.
        if (code == ERROR_SUCCESS || code == ERROR_SUCCESS_REBOOT_REQUIRED || code == ERROR_SUCCESS_REBOOT_INITIATED)
            onExit(true, QString());
        else
            onExit(false, QStringLiteral("The installer failed (error %1, see logs\\update-install.log)").arg(code));
    });
    return true;
}

bool agentAutostartSupported(bool serviceMode) { return serviceMode; }

bool firewallScopeSupported(bool serviceMode) { return serviceMode; }

QString firewallPublicRemoteIps(bool otherSubnets)
{
    return otherSubnets ? QStringLiteral("localsubnet,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,100.64.0.0/10,169.254.0.0/16")
                        : QStringLiteral("localsubnet");
}

// One netsh at a time: a change asked for while one runs waits for it, and only the last one
// asked for is applied then, so that the rule always ends as the setting says.
static QProcess *s_firewallNetsh = nullptr;
static std::optional<bool> s_firewallNext;

void setFirewallOtherSubnets(bool on)
{
    if (s_firewallNetsh) {
        s_firewallNext = on;
        return;
    }
    // netsh takes a while: not on the service's event loop.
    auto *p = new QProcess;
    s_firewallNetsh = p;
    const QStringList args{QStringLiteral("advfirewall"), QStringLiteral("firewall"), QStringLiteral("set"),
                           QStringLiteral("rule"), QStringLiteral("name=%1").arg(kPublicFirewallRule),
                           QStringLiteral("new"), QStringLiteral("remoteip=%1").arg(firewallPublicRemoteIps(on))};
    const auto done = [p] {
        p->deleteLater();
        s_firewallNetsh = nullptr;
        if (const std::optional<bool> next = std::exchange(s_firewallNext, std::nullopt))
            setFirewallOtherSubnets(*next);
    };
    QObject::connect(p, &QProcess::finished, p, [p, args, done](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0)
            qCWarning(lcPlatform).noquote() << "netsh" << args.join(QLatin1Char(' ')) << "failed:"
                                            << QString::fromLocal8Bit(p->readAllStandardOutput()).trimmed();
        else
            qCInfo(lcPlatform).noquote() << "Firewall rule for public networks now admits" << args.last();
        done();
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qCWarning(lcPlatform) << "Cannot run netsh to change the firewall rule";
            done();
        }
    });
    p->start(QStringLiteral("netsh"), args);
}

bool clipboardInAgent(bool serviceMode) { return serviceMode; }

void setAgentAutostart(bool on)
{
    LSTATUS rc = ERROR_SUCCESS;
    if (on) {
        const QString agent = QDir::toNativeSeparators(
            QFileInfo(QCoreApplication::applicationFilePath()).dir().filePath(QStringLiteral("deskflower-agent.exe")));
        const std::wstring command = QStringLiteral("\"%1\" --minimized").arg(agent).toStdWString();
        rc = RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, kRunValue, REG_SZ, command.c_str(),
                             DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        HKEY key = nullptr;
        rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        if (rc == ERROR_SUCCESS) {
            rc = RegDeleteValueW(key, kRunValue);
            RegCloseKey(key);
        }
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;
    }
    if (rc != ERROR_SUCCESS)
        qCWarning(lcPlatform) << "Cannot change the agent's autostart entry:" << lastErrorString(DWORD(rc));
}

void relaunchAgent()
{
    const DWORD session = WTSGetActiveConsoleSessionId();
    if (session == kNoSession)
        return; // it starts at the next logon (Run key)
    const QString agent = QDir::toNativeSeparators(
        QFileInfo(QCoreApplication::applicationFilePath()).dir().filePath(QStringLiteral("deskflower-agent.exe")));

    // Not a second one if the user already started it again.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        bool running = false;
        for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
            DWORD processSession = kNoSession;
            running = _wcsicmp(entry.szExeFile, L"deskflower-agent.exe") == 0
                      && ProcessIdToSessionId(entry.th32ProcessID, &processSession) && processSession == session;
        }
        CloseHandle(snapshot);
        if (running)
            return;
    }

    // As the user (unlike the session helper), with their environment.
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session, &token)) {
        qCWarning(lcPlatform) << "Cannot restart the agent: WTSQueryUserToken" << lastErrorString();
        return;
    }
    void *environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, token, FALSE))
        environment = nullptr;
    std::wstring cmd = QStringLiteral("\"%1\" --minimized").arg(agent).toStdWString();
    std::wstring desktop = L"winsta0\\default";
    const std::wstring workDir = QDir::toNativeSeparators(QFileInfo(agent).absolutePath()).toStdWString();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = desktop.data();
    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessAsUserW(token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                              CREATE_UNICODE_ENVIRONMENT, environment, workDir.c_str(), &si, &pi);
    const DWORD error = GetLastError();
    if (environment)
        DestroyEnvironmentBlock(environment);
    CloseHandle(token);
    if (!created) {
        qCWarning(lcPlatform) << "Cannot restart the agent:" << lastErrorString(error);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    qCInfo(lcPlatform) << "Restarted the agent in session" << session;
}

bool launchInActiveSession(const QString &program, const QStringList &args, const QByteArray &envName,
                           const QByteArray &envValue, SessionProcess *out, QString *error)
{
    const DWORD session = WTSGetActiveConsoleSessionId();
    if (session == kNoSession) {
        *error = QStringLiteral("No active console session");
        return false;
    }
    HANDLE self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &self)) {
        *error = QStringLiteral("OpenProcessToken: %1").arg(lastErrorString());
        return false;
    }
    HANDLE token = nullptr;
    const BOOL duplicated = DuplicateTokenEx(self, TOKEN_ALL_ACCESS, nullptr, SecurityIdentification, TokenPrimary, &token);
    CloseHandle(self);
    if (!duplicated) {
        *error = QStringLiteral("DuplicateTokenEx: %1").arg(lastErrorString());
        return false;
    }
    DWORD sessionId = session;
    if (!SetTokenInformation(token, TokenSessionId, &sessionId, sizeof(sessionId))) {
        *error = QStringLiteral("SetTokenInformation: %1").arg(lastErrorString());
        CloseHandle(token);
        return false;
    }

    // Environment: ours plus the helper's secret.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(envName), QString::fromLatin1(envValue));
    std::wstring block;
    for (const QString &entry : env.toStringList()) {
        block += entry.toStdWString();
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    QString commandLine = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(program));
    for (const QString &arg : args)
        commandLine += QStringLiteral(" \"%1\"").arg(arg);
    std::wstring cmd = commandLine.toStdWString();
    std::wstring desktop = L"winsta0\\default";
    const std::wstring workDir = QDir::toNativeSeparators(QFileInfo(program).absolutePath()).toStdWString();

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = desktop.data();
    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessAsUserW(token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.data(),
                                              workDir.c_str(), &si, &pi);
    CloseHandle(token);
    if (!created) {
        *error = QStringLiteral("CreateProcessAsUser: %1").arg(lastErrorString());
        return false;
    }
    CloseHandle(pi.hThread);
    out->handle = pi.hProcess;
    out->sessionId = session;
    return true;
}

void terminateSessionProcess(SessionProcess *process, int graceMs)
{
    if (!process->handle)
        return;
    if (graceMs <= 0 || WaitForSingleObject(process->handle, DWORD(graceMs)) != WAIT_OBJECT_0)
        TerminateProcess(process->handle, 0);
    WaitForSingleObject(process->handle, 3000);
    CloseHandle(process->handle);
    process->handle = nullptr;
    process->sessionId = kNoSession;
}

} // namespace df::platform
