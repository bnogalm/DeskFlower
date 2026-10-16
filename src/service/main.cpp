#include "buildinfo.h"
#include "logging.h"
#include "node.h"
#include "platform/platform.h"
#include "clipboardsync.h"
#include "filetransfer.h"
#include "phoneserver.h"
#include "remoteview.h"
#include "settings.h"
#include "inputcontroller.h"
#include "servicecontroller.h"
#include "sessionlauncher.h"
#include "updater.h"
#include "workspace.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

void setApplicationInfo()
{
    QCoreApplication::setOrganizationName(QStringLiteral("Deskflower"));
    QCoreApplication::setApplicationName(QStringLiteral("deskflower"));
    QCoreApplication::setApplicationVersion(df::build::version());
}

// Verified update packages go to the OS installer (Windows: msiexec, as LocalSystem).
class PlatformInstaller : public df::PackageInstaller
{
public:
    PlatformInstaller(bool serviceMode, QString logPath) : m_serviceMode(serviceMode), m_logPath(std::move(logPath)) {}
    bool isSupported() const override { return df::platform::canInstallPackages(m_serviceMode); }
    bool launch(const QString &packagePath, std::function<void(bool, const QString &)> onExit,
                QString *error) override
    {
        return df::platform::launchPackageInstaller(packagePath, m_logPath, std::move(onExit), error);
    }

private:
    bool m_serviceMode;
    QString m_logPath;
};

int runNode(int argc, char **argv, bool serviceMode)
{
    QCoreApplication app(argc, argv);
    setApplicationInfo();
    df::platform::SessionEvents::instance(); // created on the Qt thread

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DeskFlower service"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption instanceOpt(QStringLiteral("instance"),
                                   QStringLiteral("Instance name (to run several on one machine)."),
                                   QStringLiteral("name"));
    QCommandLineOption configOpt(QStringLiteral("config-dir"), QStringLiteral("Configuration directory."),
                                 QStringLiteral("dir"));
    QCommandLineOption portOpt(QStringLiteral("port"), QStringLiteral("UDP data port."), QStringLiteral("port"),
                               QString::number(df::wire::kDefaultDataPort));
    QCommandLineOption noDiscoveryOpt(QStringLiteral("no-discovery"), QStringLiteral("Disable LAN discovery."));
    QCommandLineOption serviceOpt(QStringLiteral("service"),
                                  QStringLiteral("Run under the Windows Service Control Manager."));
    QCommandLineOption installOpt(QStringLiteral("install"),
                                  QStringLiteral("Register and start the service (Windows: as administrator)."));
    QCommandLineOption uninstallOpt(QStringLiteral("uninstall"), QStringLiteral("Stop and unregister the service."));
    QCommandLineOption noSessionOpt(QStringLiteral("no-session"),
                                    QStringLiteral("Do not start the session helper (no screens, no input)."));
    parser.addOptions(
        {instanceOpt, configOpt, portOpt, noDiscoveryOpt, serviceOpt, installOpt, uninstallOpt, noSessionOpt});
    parser.process(app);

    const QString instance = parser.value(instanceOpt);
    QString configDir = parser.value(configOpt);
    if (configDir.isEmpty()) {
        configDir = df::platform::defaultConfigDir(serviceMode);
        if (!instance.isEmpty())
            configDir = QDir(configDir).filePath(instance);
    }
    QDir().mkpath(configDir);
    // As LocalSystem, a directory that others can change is not used at all (it holds the identity,
    // and the updater deletes and installs from it).
    if (!df::platform::secureDirectory(configDir, serviceMode) && serviceMode) {
        qCritical().noquote() << "Cannot secure" << configDir;
        return 1;
    }
    df::Settings settings(configDir);
    // Log files only when asked for in settings.json ("logging": true).
    const bool logToFile = settings.value(QStringLiteral("logging"), false).toBool();
    const QString logDir = QDir(configDir).filePath(QStringLiteral("logs"));
    df::installFileLogging(logDir, QStringLiteral("service.log"), !serviceMode, logToFile);
    if (!logToFile) {
        QFile::remove(QDir(logDir).filePath(QStringLiteral("session.log")));
        QFile::remove(QDir(logDir).filePath(QStringLiteral("session.log.1")));
        QFile::remove(QDir(logDir).filePath(QStringLiteral("update-install.log")));
    }
    df::platform::installQuitHandlers();
    df::Node::Options options;
    options.configDir = configDir;
    // Ports: Advanced settings in the agent (applied at the next start); --port wins.
    options.dataPort = parser.isSet(portOpt)
                           ? quint16(parser.value(portOpt).toUInt())
                           : quint16(settings.value(QStringLiteral("dataPort"), df::wire::kDefaultDataPort).toInt());
    options.discoveryPort = quint16(settings.value(QStringLiteral("discoveryPort"), df::wire::kDiscoveryPort).toInt());
    options.enableDiscovery = !parser.isSet(noDiscoveryOpt);

    df::Node node(options);
    QString error;
    if (!node.start(&error)) {
        qCritical().noquote() << error;
        return 1;
    }
    // Each pairing works only while the user who made it is signed in at the console (also on
    // their lock screen, not on the sign-in screen or in another user's session).
    QTimer userWatch;
    if (df::platform::separatesUsers(serviceMode)) {
        auto followUser = [&node, serviceMode] { node.setLocalUser(df::platform::interactiveUserId(serviceMode)); };
        followUser();
        QObject::connect(df::platform::SessionEvents::instance(), &df::platform::SessionEvents::sessionChanged,
                         &node, followUser);
        userWatch.setInterval(2000); // session notifications can be missed
        QObject::connect(&userWatch, &QTimer::timeout, &node, followUser);
        userWatch.start();
    }
    df::Workspace workspace(&node, configDir);
    df::InputController input(&node, &workspace);
    // Keys only go where the cursor is, and a device that locks (or switches sessions) takes the
    // cursor back: the password typed on its lock screen is typed there, not on the device the
    // cursor happened to be on.
    QObject::connect(df::platform::SessionEvents::instance(), &df::platform::SessionEvents::sessionChanged, &input,
                     [&input](df::platform::SessionEvents::Event event, quint32) {
                         using E = df::platform::SessionEvents;
                         if (event == E::Lock || event == E::Logoff || event == E::ConsoleDisconnect)
                             input.returnHome();
                     });
    df::FileTransfer transfer(&node);
    // Shared folder: chosen in the agent, or <user home>/Deskflower (looked up each time: users log
    // on and off). Phone uploads go there and phones see it.
    // A chosen folder must still be acceptable (inside the signed-in user's folder; see
    // isAllowedSharedFolder), and it is created with that user's rights.
    auto sharedFolder = [&settings, serviceMode, configDir] {
        return df::platform::sharedFolder(settings.value(QStringLiteral("sharedFolder")).toString(),
                                          QDir(configDir).filePath(QStringLiteral("Shared")), serviceMode);
    };
    // The clipboard follows the shared cursor (ClipboardSync).
    df::ClipboardSync::Presence presence{[&input](qint64 graceMs) { return input.cursorHere(graceMs); },
                                         [&workspace] { return workspace.masterId(); }};
    // Files received from paired devices wait apart, where phones do not see them.
    auto receivedFolder = [serviceMode, configDir] {
        return df::platform::receivedFolder(QDir(configDir).filePath(QStringLiteral("Received")), serviceMode);
    };
    df::ClipboardSync clipboard(&node, &transfer, &settings, presence, receivedFolder);
    QObject::connect(&input, &df::InputController::cursorMoved, &clipboard, &df::ClipboardSync::cursorMoved);
    df::web::PhoneServer::Options phoneOptions;
    phoneOptions.configDir = configDir;
    phoneOptions.port = quint16(settings.value(QStringLiteral("phonePort"), df::web::PhoneServer::kDefaultPort).toInt());
    phoneOptions.folder = sharedFolder;
    phoneOptions.deviceName = node.name();
    phoneOptions.asUser = [serviceMode] { return df::platform::actAsInteractiveUser(serviceMode); };
    df::web::PhoneServer phone(phoneOptions);
    df::RemoteView remoteView(&node, &workspace);
    PlatformInstaller installer(serviceMode,
                                logToFile ? QDir(logDir).filePath(QStringLiteral("update-install.log")) : QString());
    df::Updater updater(&node, &installer,
                        df::Updater::Options::defaults(QDir(configDir).filePath(QStringLiteral("updates"))));
    if (serviceMode) {
        // Agents quit while an update is installed (ServiceController). The next service starts the
        // agent again, or this one if the installer fails.
        const QString agentMarker = QDir(configDir).filePath(QStringLiteral("updates/restart-agent"));
        QObject::connect(&updater, &df::Updater::aboutToInstall, &updater, [agentMarker] {
            QFile marker(agentMarker);
            if (!marker.open(QIODevice::WriteOnly))
                qWarning().noquote() << "Cannot write" << agentMarker;
        });
        QObject::connect(&updater, &df::Updater::finished, &updater, [agentMarker](const QByteArray &, bool ok) {
            if (!ok && QFile::remove(agentMarker))
                df::platform::relaunchAgent();
        });
        if (QFile::remove(agentMarker))
            df::platform::relaunchAgent();
    }
    std::unique_ptr<df::SessionLauncher> launcher;
    if (!parser.isSet(noSessionOpt))
        launcher = std::make_unique<df::SessionLauncher>(instance, logToFile ? logDir : QString(), serviceMode);
    // With --no-session the helper can be started by hand (e.g. under a debugger) using a token
    // agreed through DESKFLOWER_SESSION_TOKEN.
    const QByteArray sessionToken = launcher ? launcher->token() : qgetenv("DESKFLOWER_SESSION_TOKEN");
    df::ServiceController controller(&node, &workspace, &input, &clipboard, &transfer, &settings, &phone, &remoteView,
                                     &updater, sharedFolder, receivedFolder, instance, serviceMode,
                                     sessionToken);
    if (!controller.listen(&error)) {
        qCritical().noquote() << error;
        return 1;
    }
    if (serviceMode) {
        // The Run entry follows the user's choice (also after an upgrade, which removes the old one).
        const bool autostart = settings.value(QStringLiteral("agentAutostart"), true).toBool();
        if (df::platform::agentAutostartSupported(serviceMode))
            df::platform::setAgentAutostart(autostart);
        // First start of a new version: an installer (or an update from a paired device) closed the
        // agent to replace its files, so start it again for the signed-in user.
        const QString version = QCoreApplication::applicationVersion();
        if (settings.value(QStringLiteral("lastVersion")).toString() != version) {
            settings.setValue(QStringLiteral("lastVersion"), version);
            df::platform::relaunchAgent();
        }
    }
    // Discovery mode is on at launch so new devices can find and pair with us.
    node.setExploring(true);
    if (launcher)
        launcher->start();

    qInfo().noquote() << "DeskFlower service" << QCoreApplication::applicationVersion()
                      << (serviceMode ? "(service mode)" : "(console mode)") << "config:" << configDir;
    df::platform::notifyStarted();
    const int rc = app.exec();
    qInfo() << "DeskFlower service stopped";
    return rc;
}

bool hasArg(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0)
            return true;
    }
    return false;
}

// Run by the MSI (as LocalSystem) with the package being installed: when a signed manifest sits next
// to it, the package is kept so that this device can serve it to paired devices.
int importUpdate(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    setApplicationInfo();
    QCommandLineParser parser;
    QCommandLineOption importOpt(QStringLiteral("import-update"), QStringLiteral("Package to keep for paired devices."),
                                 QStringLiteral("package"));
    QCommandLineOption configOpt(QStringLiteral("config-dir"), QStringLiteral("Configuration directory."),
                                 QStringLiteral("dir"));
    parser.addOptions({importOpt, configOpt});
    parser.process(app);

    QString configDir = parser.value(configOpt);
    if (configDir.isEmpty())
        configDir = df::platform::defaultConfigDir(true);
    QDir().mkpath(configDir);
    if (!df::platform::secureDirectory(configDir, true)) {
        std::fprintf(stderr, "Update package not kept: cannot secure %s\n", qPrintable(configDir));
        return 1;
    }
    QString error;
    const auto options = df::Updater::Options::defaults(QDir(configDir).filePath(QStringLiteral("updates")));
    if (!df::Updater::importPackage(options, parser.value(importOpt), &error)) {
        std::fprintf(stderr, "Update package not kept: %s\n", qPrintable(error));
        return 1;
    }
    std::fprintf(stdout, "Update package kept for paired devices.\n");
    return 0;
}

int manageService(int argc, char **argv, bool install)
{
    QCoreApplication app(argc, argv);
    setApplicationInfo();
    QString message;
    const int rc = install ? df::platform::install(&message) : df::platform::uninstall(&message);
    std::fprintf(rc == 0 ? stdout : stderr, "%s\n", qPrintable(message));
    return rc;
}

} // namespace

int main(int argc, char *argv[])
{
    // Decided before QCoreApplication exists: the Windows SCM expects the dispatcher right away.
    if (hasArg(argc, argv, "--install"))
        return manageService(argc, argv, true);
    if (hasArg(argc, argv, "--uninstall"))
        return manageService(argc, argv, false);
    if (hasArg(argc, argv, "--import-update"))
        return importUpdate(argc, argv);
    if (hasArg(argc, argv, "--service"))
        return df::platform::runService(argc, argv, [](int c, char **v) { return runNode(c, v, true); });
    return runNode(argc, argv, false);
}
