#include "platform.h"

#include <QCoreApplication>
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
QString guiDomain() { return QStringLiteral("gui/%1").arg(::getuid()); }
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
    const QByteArray plist = QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.deskflower.service</string>
    <key>ProgramArguments</key><array><string>%1</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
    <key>ProcessType</key><string>Interactive</string>
    <key>StandardErrorPath</key><string>%2/service.log</string>
</dict>
</plist>
)").arg(exe.toHtmlEscaped(), logDir.toHtmlEscaped()).toUtf8();
    if (!writeFile(plistPath(), plist, message))
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

bool secureDirectory(const QString &path, bool) { return ::chmod(QFile::encodeName(path).constData(), 0700) == 0; }

bool isClientAllowed(QLocalSocket *, bool) { return true; } // socket file is owner-only

// The service runs inside the user session on these platforms: the helper is a plain child.
quint32 activeConsoleSession() { return kNoSession; }

QString interactiveUserHome(bool) { return QDir::homePath(); }

bool separatesUsers(bool) { return false; } // the service runs as the user

QString interactiveUserId(bool) { return {}; }

std::shared_ptr<void> actAsInteractiveUser(bool)
{
    return std::make_shared<int>(0); // the service runs as the user
}

void setUserTokenSourceForTests(UserTokenSource) {} // no tokens: the service runs as the user

bool canInstallPackages(bool) { return false; }

bool launchPackageInstaller(const QString &, const QString &, std::function<void(bool, const QString &)>,
                            QString *error)
{
    *error = QStringLiteral("Installing updates is not supported on this platform yet");
    return false;
}

void relaunchAgent() {}

bool agentAutostartSupported(bool) { return false; }

bool clipboardInAgent(bool) { return false; }

void setAgentAutostart(bool) {}

bool launchInActiveSession(const QString &, const QStringList &, const QByteArray &, const QByteArray &,
                           SessionProcess *, QString *error)
{
    *error = QStringLiteral("Not supported on this platform");
    return false;
}

void terminateSessionProcess(SessionProcess *) {}

} // namespace df::platform
