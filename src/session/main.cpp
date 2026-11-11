// deskflower-session: helper started by the service inside the interactive user session.
// It owns everything that needs the user's desktop: screen enumeration, thumbnails, and
// keyboard/mouse capture and injection.

#include "buildinfo.h"
#include "crashreport.h"
#include "inputbackend.h"
#include "logging.h"
#include "sessionclient.h"

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTimer>

#if defined(Q_OS_LINUX)
#include "linuxbackends.h"
#include "localscreens.h"
#ifdef DESKFLOWER_HAVE_PORTAL
#include "portalclipboard.h"
#include "portalsession.h"
#endif
#ifdef DESKFLOWER_HAVE_PIPEWIRE
#include "pipewirecapture.h"
#endif
#include <X11/Xlib.h> // last: its macros clash with Qt
#endif

#if defined(Q_OS_LINUX) && defined(DESKFLOWER_HAVE_PORTAL)
// Wayland: one remote desktop portal session gives the clipboard, the screens (PipeWire) and,
// through the input backend, the keyboard and mouse.
static void usePortal(SessionClient *client)
{
    PortalSession *portal = PortalSession::instance();
    client->setClipboardBackend(new PortalClipboard(portal));
#ifdef DESKFLOWER_HAVE_PIPEWIRE
    QObject::connect(portal, &PortalSession::started, portal, [portal] {
        QList<PipeWireCapture::Source> sources;
        for (const PortalSession::Stream &s : portal->streams())
            sources.append({s.node, s.geometry});
        const int fd = sources.isEmpty() ? -1 : portal->openPipeWireRemote();
        if (fd < 0)
            return;
        PipeWireCapture::setInstance(PipeWireCapture::connect(fd, sources));
        LocalScreens::setCaptureSource([](const QRect &screen) {
            QImage image;
            quint64 serial = 0;
            if (PipeWireCapture *capture = PipeWireCapture::instance())
                capture->frame(screen, &image, &serial, 0);
            return image;
        });
    });
#endif
    // Without its portal session the helper has no keyboard, mouse, clipboard or screens: when the
    // desktop ends it (the user stopped sharing, the portal restarted) or it failed for another
    // reason than a refusal, the helper leaves and the service starts a new one (with a growing
    // delay while that keeps failing), whose session is restored without a dialog. The input
    // backend has reported the problem to the service meanwhile.
    auto restartIfWorthIt = [portal] {
        if (!portal->worthRestarting())
            return;
        qWarning("The remote desktop portal session is gone: exiting so that the service starts over");
        QTimer::singleShot(1000, qApp, [] { QCoreApplication::exit(3); }); // the problem reaches the service first
    };
    QObject::connect(portal, &PortalSession::closed, portal, restartIfWorthIt);
    QObject::connect(portal, &PortalSession::failed, portal, restartIfWorthIt);
    portal->start();
}
#endif

int main(int argc, char *argv[])
{
#if defined(Q_OS_LINUX)
    // The input backend, the screen capture threads and Qt each use Xlib connections.
    XInitThreads();
#endif
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Deskflower"));
    QGuiApplication::setApplicationName(QStringLiteral("deskflower-session"));
    QGuiApplication::setApplicationVersion(df::build::version());
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption instanceOpt(QStringLiteral("instance"), QStringLiteral("Service instance."),
                                   QStringLiteral("name"));
    QCommandLineOption logDirOpt(QStringLiteral("log-dir"), QStringLiteral("Directory for session.log."),
                                 QStringLiteral("dir"));
    QCommandLineOption crashDirOpt(QStringLiteral("crash-dir"),
                                   QStringLiteral("Directory for problems.log and crash dumps."), QStringLiteral("dir"));
    parser.addOptions({instanceOpt, logDirOpt, crashDirOpt});
    parser.process(app);

    if (parser.isSet(logDirOpt))
        df::installFileLogging(parser.value(logDirOpt), QStringLiteral("session.log"), true, true);
    // Always on, like the service's and the agent's: where the service says (<logs>/session),
    // or, started by hand, under the local application data (deskflower-session/logs).
    df::crash::install(parser.isSet(crashDirOpt)
                           ? parser.value(crashDirOpt)
                           : QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                 .filePath(QStringLiteral("logs")),
                       QStringLiteral("session"));

    // The service passes a secret so that only the helper it started can claim the session role.
    const QByteArray token = qgetenv("DESKFLOWER_SESSION_TOKEN");
    if (token.isEmpty()) {
        qCritical("DESKFLOWER_SESSION_TOKEN is not set: this program is started by deskflower-service");
        return 1;
    }

    InputBackend *input = InputBackend::create(&app);
    if (!input->start())
        qWarning("Input capture is not available; screens and thumbnails are still reported");
    SessionClient client(parser.value(instanceOpt), token, input);
#if defined(Q_OS_LINUX) && defined(DESKFLOWER_HAVE_PORTAL)
    if (isWaylandSession())
        usePortal(&client);
#endif
    const int rc = app.exec();
    df::crash::markCleanExit(rc);
    return rc;
}
