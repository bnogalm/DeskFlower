// deskflower-session: helper started by the service inside the interactive user session.
// It owns everything that needs the user's desktop: screen enumeration, thumbnails, and
// keyboard/mouse capture and injection.

#include "inputbackend.h"
#include "logging.h"
#include "sessionclient.h"

#include <QCommandLineParser>
#include <QGuiApplication>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Deskflower"));
    QGuiApplication::setApplicationName(QStringLiteral("deskflower-session"));
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption instanceOpt(QStringLiteral("instance"), QStringLiteral("Service instance."),
                                   QStringLiteral("name"));
    QCommandLineOption logDirOpt(QStringLiteral("log-dir"), QStringLiteral("Directory for session.log."),
                                 QStringLiteral("dir"));
    parser.addOptions({instanceOpt, logDirOpt});
    parser.process(app);

    if (parser.isSet(logDirOpt))
        df::installFileLogging(parser.value(logDirOpt), QStringLiteral("session.log"), true);

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
    return app.exec();
}
