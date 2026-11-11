#include "platform.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace df::platform {

namespace {

int g_signalFds[2] = {-1, -1};

void signalHandler(int)
{
    const char c = 1;
    [[maybe_unused]] const auto n = ::write(g_signalFds[0], &c, 1);
}

bool writeFile(const QString &path, const QByteArray &content, QString *message)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(content) != content.size() || !f.commit()) {
        *message = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    return true;
}

int run(const QString &program, const QStringList &args, QString *output = nullptr)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForFinished(15000))
        return -1;
    if (output)
        *output = QString::fromLocal8Bit(p.readAll()).trimmed();
    return p.exitCode();
}

#ifdef Q_OS_MACOS
QString plistPath()
{
    return QDir::home().filePath(QStringLiteral("Library/LaunchAgents/com.deskflower.service.plist"));
}
QString agentPlistPath()
{
    return QDir::home().filePath(QStringLiteral("Library/LaunchAgents/com.deskflower.agent.plist"));
}
QString guiDomain() { return QStringLiteral("gui/%1").arg(::getuid()); }
// The agent of the same bundle (DeskFlower.app/Contents/MacOS, next to the service).
QString agentProgram() { return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("DeskFlower")); }
constexpr const char *kAgentBundleId = "com.deskflower.agent";
#else
QString unitPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("systemd/user/deskflower.service"));
}
#endif

} // namespace

int install(QString *message)
{
    const QString exe = QCoreApplication::applicationFilePath();
#ifdef Q_OS_MACOS
    const QString logDir = QDir::home().filePath(QStringLiteral("Library/Logs/Deskflower"));
    QDir().mkpath(logDir);
    LaunchAgent agent;
    agent.label = QStringLiteral("com.deskflower.service");
    agent.arguments = {exe};
    agent.keepAlive = true; // quitting from the agent exits cleanly: stopped until it starts it
    agent.associatedBundle = QString::fromLatin1(kAgentBundleId);
    agent.logPath = QDir(logDir).filePath(QStringLiteral("service.log"));
    // Qt's OpenSSL backend (the phone server) loads libssl by name at run time: the copy in the
    // bundle (Contents/Frameworks), then the system's usual places.
    const QString frameworks = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../Frameworks"));
    if (QFileInfo(frameworks).isDir())
        agent.environment.append({QStringLiteral("DYLD_FALLBACK_LIBRARY_PATH"),
                                  frameworks + QStringLiteral(":/usr/local/lib:/usr/lib")});
    if (!writeFile(plistPath(), launchAgentPlist(agent), message))
        return 1;
    run(QStringLiteral("launchctl"), {QStringLiteral("bootout"), guiDomain(), plistPath()});
    QString out;
    if (run(QStringLiteral("launchctl"), {QStringLiteral("bootstrap"), guiDomain(), plistPath()}, &out) != 0) {
        *message = QStringLiteral("launchctl bootstrap failed: %1").arg(out);
        return 1;
    }
    *message = QStringLiteral("LaunchAgent installed: %1").arg(plistPath());
#else
    const QByteArray unit = QStringLiteral(R"([Unit]
Description=DeskFlower service (shared keyboard, mouse and clipboard)
After=graphical-session.target
PartOf=graphical-session.target

[Service]
ExecStart="%1"
Restart=on-failure
RestartSec=3

[Install]
WantedBy=graphical-session.target
)").arg(exe).toUtf8();
    if (!writeFile(unitPath(), unit, message))
        return 1;
    QString out;
    run(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    if (run(QStringLiteral("systemctl"),
            {QStringLiteral("--user"), QStringLiteral("enable"), QStringLiteral("--now"),
             QStringLiteral("deskflower.service")},
            &out) != 0) {
        *message = QStringLiteral("systemctl --user enable failed: %1").arg(out);
        return 1;
    }
    *message = QStringLiteral("systemd user unit installed: %1").arg(unitPath());
#endif
    return 0;
}

int uninstall(QString *message)
{
#ifdef Q_OS_MACOS
    run(QStringLiteral("launchctl"), {QStringLiteral("bootout"), guiDomain(), plistPath()});
    QFile::remove(plistPath());
    QFile::remove(agentPlistPath()); // not booted out: that would end the running agent
    *message = QStringLiteral("LaunchAgent removed.");
#else
    run(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"), QStringLiteral("--now"),
                                      QStringLiteral("deskflower.service")});
    QFile::remove(unitPath());
    run(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    *message = QStringLiteral("systemd user unit removed.");
#endif
    return 0;
}

int runService(int argc, char **argv, const Body &body) { return body(argc, argv); }

void notifyStarted() {}

void installQuitHandlers()
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_signalFds) != 0)
        return;
    auto *notifier = new QSocketNotifier(g_signalFds[1], QSocketNotifier::Read, QCoreApplication::instance());
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [] {
        char c;
        [[maybe_unused]] const auto n = ::read(g_signalFds[1], &c, 1);
        QCoreApplication::quit();
    });
    struct sigaction sa = {};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

QString defaultConfigDir(bool) { return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation); }

void createConfigDir(const QString &path, bool) { QDir().mkpath(path); } // the service runs as the user

bool secureDirectory(const QString &path, bool) { return ::chmod(QFile::encodeName(path).constData(), 0700) == 0; }

bool isClientAllowed(QLocalSocket *, bool) { return true; } // socket file is owner-only

quint32 clientSessionId(QLocalSocket *, bool) { return kNoSession; } // one service per user here

// The service runs inside the user session on these platforms: the helper is a plain child.
quint32 activeConsoleSession() { return kNoSession; }

QString interactiveUserHome(bool) { return QDir::homePath(); }

bool separatesUsers(bool) { return false; } // the service runs as the user

QString interactiveUserId(bool) { return {}; }

std::shared_ptr<void> actAsInteractiveUser(bool)
{
    return std::make_shared<int>(0); // the service runs as the user
}

QString impersonatedUserId() { return {}; } // nobody is impersonated

void setUserTokenSourceForTests(UserTokenSource) {} // no tokens: the service runs as the user

bool canInstallPackages(bool) { return false; }

bool launchPackageInstaller(const QString &, const QString &, std::function<void(bool, const QString &)>,
                            QString *error)
{
    *error = QStringLiteral("Installing updates is not supported on this platform yet");
    return false;
}

void relaunchAgent() {}

#ifdef Q_OS_MACOS
bool agentAutostartSupported(bool)
{
    // Only the service of an installed DeskFlower.app: one run from a build tree (tests, e2e,
    // development) never touches the user's LaunchAgent.
    return QCoreApplication::applicationDirPath().endsWith(QLatin1String(".app/Contents/MacOS"));
}
#else
bool agentAutostartSupported(bool) { return false; }
#endif

bool clipboardInAgent(bool) { return false; }

#ifdef Q_OS_MACOS
void setAgentAutostart(bool on)
{
    // Read by launchd at the next sign-in; never loaded now (that would start a second agent, and
    // unloading would end the running one).
    if (!on) {
        QFile::remove(agentPlistPath());
        return;
    }
    const QString agent = agentProgram();
    if (!QFileInfo(agent).isExecutable()) {
        qWarning().noquote() << QStringLiteral("No agent next to the service (%1): it cannot start at sign-in").arg(agent);
        return;
    }
    LaunchAgent entry;
    entry.label = QStringLiteral("com.deskflower.agent");
    entry.arguments = {agent, QStringLiteral("--minimized")};
    entry.associatedBundle = QString::fromLatin1(kAgentBundleId);
    QString message;
    if (!writeFile(agentPlistPath(), launchAgentPlist(entry), &message))
        qWarning().noquote() << message;
}
#else
void setAgentAutostart(bool) {}
#endif

bool firewallScopeSupported(bool) { return false; }

void setFirewallOtherSubnets(bool) {}

bool launchInActiveSession(const QString &, const QStringList &, const QByteArray &, const QByteArray &,
                           SessionProcess *, QString *error)
{
    *error = QStringLiteral("Not supported on this platform");
    return false;
}

void terminateSessionProcess(SessionProcess *, int) {}

} // namespace df::platform
