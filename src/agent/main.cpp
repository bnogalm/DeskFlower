#include "backend.h"
#include "buildinfo.h"
#include "logging.h"
#include "crashreport.h"
#include "qrimageprovider.h"
#include "thumbnailprovider.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QStandardPaths>
#include <QQuickWindow>
#include <QSystemTrayIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QScreen>
#include <QStandardPaths>

#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// One agent per session (and service instance). Two would both own the clipboard, apply every
// clipboard the service sends and show two tray icons; the service starts one when none is
// connected, and the Run key (or the user) may start another meanwhile. The second one asks the
// running one to show its window (unless it was started minimized) and exits.
class SingleInstance
{
public:
    explicit SingleInstance(const QString &instance)
    {
        const QString key = QStringLiteral("deskflower-agent-")
                            + (instance.isEmpty() ? QStringLiteral("default") : instance);
#ifdef Q_OS_WIN
        // A Local\ mutex is per session; the pipe namespace is not, so its name carries the session.
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        m_serverName = key + QStringLiteral("-s") + QString::number(session);
        m_mutex = CreateMutexW(nullptr, FALSE, (QStringLiteral("Local\\") + key).toStdWString().c_str());
        m_first = !m_mutex || GetLastError() != ERROR_ALREADY_EXISTS; // no mutex: do not refuse to run
#else
        QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (dir.isEmpty())
            dir = QDir::tempPath();
        m_serverName = QDir(dir).filePath(key + QStringLiteral(".sock"));
        m_lock = std::make_unique<QLockFile>(QDir(dir).filePath(key + QStringLiteral(".lock")));
        m_first = m_lock->tryLock(0) || m_lock->error() == QLockFile::PermissionError;
#endif
    }
    ~SingleInstance()
    {
#ifdef Q_OS_WIN
        if (m_mutex)
            CloseHandle(m_mutex);
#endif
    }
    SingleInstance(const SingleInstance &) = delete;
    SingleInstance &operator=(const SingleInstance &) = delete;

    bool isFirst() const { return m_first; }

    // The running agent: asks it to show its window.
    void activateFirst() const
    {
#ifdef Q_OS_WIN
        AllowSetForegroundWindow(ASFW_ANY); // this launch is the user's: let it come to the front
#endif
        QLocalSocket socket;
        socket.connectToServer(m_serverName);
        if (!socket.waitForConnected(2000))
            return;
        socket.write("show\n");
        socket.waitForBytesWritten(1000);
        socket.disconnectFromServer();
    }

    // This agent: listens for later launches, calling `show` for each one that asks.
    void listen(QObject *parent, std::function<void()> show)
    {
        auto *server = new QLocalServer(parent);
        server->setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(m_serverName); // a stale socket file of a crashed agent
        if (!server->listen(m_serverName))
            return;
        QObject::connect(server, &QLocalServer::newConnection, server, [server, show] {
            while (QLocalSocket *client = server->nextPendingConnection()) {
                QObject::connect(client, &QLocalSocket::readyRead, client, [client, show] {
                    if (client->readAll().contains("show"))
                        show();
                });
                QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            }
        });
    }

private:
    QString m_serverName;
    bool m_first = true;
#ifdef Q_OS_WIN
    HANDLE m_mutex = nullptr;
#else
    std::unique_ptr<QLockFile> m_lock;
#endif
};

// A desktop monitor with a flower (resources/icons). Each size is a separate drawing, so the tray
// (16-32 px) gets the simplified variant instead of a downscaled one.
QIcon appIcon()
{
    QIcon icon;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/deskflower-%1.png").arg(size), QSize(size, size));
    return icon;
}

void showWindow(QQmlApplicationEngine &engine)
{
    if (engine.rootObjects().isEmpty())
        return;
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        // show() leaves a minimized window minimized: a prompt there would go unseen.
        window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
        window->show();
        window->raise();
        window->requestActivate();
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv); // widgets only for the system tray menu
    QApplication::setOrganizationName(QStringLiteral("Deskflower"));
    QApplication::setApplicationName(QStringLiteral("deskflower-agent"));
    QApplication::setApplicationDisplayName(QStringLiteral("DeskFlower"));
    QApplication::setDesktopFileName(QStringLiteral("deskflower")); // Wayland app id: deskflower.desktop
    QApplication::setApplicationVersion(df::build::version());
    QApplication::setWindowIcon(appIcon());
    // Always on (the agent writes no other log): a crash leaves a line and, on Windows, a
    // minidump under the user's local application data (Deskflower/deskflower-agent/logs).
    df::crash::install(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                           .filePath(QStringLiteral("logs")),
                       QStringLiteral("agent"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption instanceOpt(QStringLiteral("instance"), QStringLiteral("Service instance to control."),
                                   QStringLiteral("name"));
    QCommandLineOption pageOpt(QStringLiteral("page"),
                               QStringLiteral("Page to open: devices, screens, files or settings (advanced)."),
                               QStringLiteral("page"), QStringLiteral("devices"));
    QCommandLineOption minimizedOpt(QStringLiteral("minimized"),
                                    QStringLiteral("Start in the system tray (used at logon)."));
    parser.addOptions({instanceOpt, pageOpt, minimizedOpt});
    parser.process(app);

    SingleInstance single(parser.value(instanceOpt));
    if (!single.isFirst()) {
        if (!parser.isSet(minimizedOpt))
            single.activateFirst();
        return 0;
    }

    // File drags fail in ways nobody can reconstruct afterwards: the agent's side of them (Explorer,
    // the drop target, the effect it reports) is always kept, small and without names or paths.
    df::installDiagnosticLog(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                 .filePath(QStringLiteral("logs")),
                             QStringLiteral("drag.log"),
                             {QStringLiteral("deskflower.agent.drag"), QStringLiteral("deskflower.clipboard.virtual")});

    QQuickStyle::setStyle(QStringLiteral("Material"));

    QQmlApplicationEngine engine;
    auto *thumbnails = new ThumbnailProvider; // owned by the engine
    engine.addImageProvider(QStringLiteral("thumbs"), thumbnails);
    engine.addImageProvider(QStringLiteral("qr"), new QrImageProvider);
    Backend backend(parser.value(instanceOpt), thumbnails);
    const bool tray = QSystemTrayIcon::isSystemTrayAvailable();
    engine.setInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&backend)},
                                 {QStringLiteral("startHidden"), tray && parser.isSet(minimizedOpt)},
                                 {QStringLiteral("hideOnClose"), tray},
                                 {QStringLiteral("initialPage"), parser.value(pageOpt) == QLatin1String("screens") ? 1
                                     : parser.value(pageOpt) == QLatin1String("files")    ? 2
                                     : parser.value(pageOpt) == QLatin1String("settings") ? 3
                                                                                          : 0}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Deskflower", "Main");
    single.listen(&app, [&engine] { showWindow(engine); });

    // System tray: the agent keeps running there when its window is closed.
    QSystemTrayIcon trayIcon(appIcon());
    QMenu menu;
    if (tray) {
        trayIcon.setToolTip(QStringLiteral("DeskFlower"));
        menu.addAction(QObject::tr("Open DeskFlower"), &app, [&engine] { showWindow(engine); });
        QAction *share = menu.addAction(QObject::tr("Share this keyboard and mouse"));
        share->setCheckable(true);
        QObject::connect(share, &QAction::triggered, &backend, &Backend::setSharing);
        QObject::connect(&backend, &Backend::stateChanged, share, [share, &backend] {
            share->setChecked(backend.sharing());
            share->setEnabled(backend.serviceConnected());
        });
        menu.addSeparator();
        // Quitting stops DeskFlower altogether (the service too), until it is opened again.
        menu.addAction(QObject::tr("Quit DeskFlower"), &backend, &Backend::stopServiceAndQuit);
        trayIcon.setContextMenu(&menu);
        QObject::connect(&trayIcon, &QSystemTrayIcon::activated, &app, [&engine](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
                showWindow(engine);
        });
        trayIcon.show();
        QApplication::setQuitOnLastWindowClosed(false);
    }

    // Requests that need this user's answer bring up the window, even when it lives in the tray
    // (a --minimized agent has a hidden window).
    const auto notify = [&trayIcon, tray](const QString &title, const QString &text) {
        if (tray)
            trayIcon.showMessage(title, text, QSystemTrayIcon::Information, 10000);
    };
    QObject::connect(&backend, &Backend::viewPromptRequested, &app,
                     [&engine, notify](const QString &, const QString &peerName, int) {
                         showWindow(engine);
                         notify(QObject::tr("Screen view request"),
                                QObject::tr("%1 wants to see this screen.").arg(peerName));
                     });
    QObject::connect(&backend, &Backend::pinShown, &app,
                     [&engine, notify](const QString &, const QString &peerName, const QString &) {
                         showWindow(engine);
                         notify(QObject::tr("Pairing request"), QObject::tr("%1 wants to pair.").arg(peerName));
                     });
    QObject::connect(&backend, &Backend::pinRequested, &app, [&engine] { showWindow(engine); });
    QString screenViewer;
    QObject::connect(&backend, &Backend::stateChanged, &app, [&backend, &screenViewer, notify] {
        const QString name = backend.screenViewerName();
        if (!name.isEmpty() && name != screenViewer)
            notify(QObject::tr("Screen shared"), QObject::tr("Your screen is being shown on %1.").arg(name));
        screenViewer = name;
    });
    // With the window hidden in the tray, transfers show in the tooltip and problems as a
    // notification (the window shows both itself).
    const auto windowShown = [&engine] {
        auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        return window && window->isVisible() && !(window->windowStates() & Qt::WindowMinimized);
    };
    // The guests (either way) stay in the tooltip too, so that a guest connection is never forgotten.
    const auto updateTooltip = [&backend, &trayIcon, tray] {
        if (!tray)
            return;
        QStringList lines{QStringLiteral("DeskFlower")};
        for (const QString &line : {backend.guestsSummary(), backend.transfersSummary(), backend.waitingSummary()}) {
            if (!line.isEmpty())
                lines.append(line);
        }
        trayIcon.setToolTip(lines.join(QLatin1Char('\n')));
    };
    QObject::connect(&backend, &Backend::transfersChanged, &app, updateTooltip);
    QObject::connect(&backend, &Backend::waitingChanged, &app, updateTooltip);
    // The progress window (TransferWindow.qml) sits at the bottom right of the primary screen's
    // free area (above the taskbar or dock), whatever its height; Wayland places it itself.
    if (!engine.rootObjects().isEmpty()) {
        if (auto *progress = qobject_cast<QQuickWindow *>(
                engine.rootObjects().first()->property("progressWindow").value<QObject *>())) {
            const auto place = [progress] {
                const QScreen *screen = QGuiApplication::primaryScreen();
                if (!screen)
                    return;
                constexpr int kMargin = 16;
                const QRect free = screen->availableGeometry();
                progress->setPosition(free.x() + free.width() - progress->width() - kMargin,
                                      free.y() + free.height() - progress->height() - kMargin);
            };
            QObject::connect(progress, &QWindow::widthChanged, progress, place);
            QObject::connect(progress, &QWindow::heightChanged, progress, place);
            QObject::connect(progress, &QWindow::visibleChanged, progress, [place](bool on) {
                if (on)
                    place();
            });
            QObject::connect(qApp, &QGuiApplication::primaryScreenChanged, progress, place);
            place();
        }
    }
    QObject::connect(&backend, &Backend::stateChanged, &app, updateTooltip);
    // A paired device of another protocol: told once in the tray (the banner stays in the window).
    QObject::connect(&backend, &Backend::protocolAlert, &app,
                     [notify](const QString &text) { notify(QStringLiteral("DeskFlower"), text); });
    QObject::connect(&backend, &Backend::notice, &app, [windowShown, notify](const QString &message, bool problem) {
        if (problem && !windowShown())
            notify(QStringLiteral("DeskFlower"), message);
    });
    const int rc = app.exec();
    df::crash::markCleanExit(rc);
    return rc;
}
