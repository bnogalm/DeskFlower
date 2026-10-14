#include "backend.h"
#include "buildinfo.h"
#include "qrimageprovider.h"
#include "thumbnailprovider.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMenu>
#include <QQuickWindow>
#include <QSystemTrayIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

namespace {

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
    return app.exec();
}
