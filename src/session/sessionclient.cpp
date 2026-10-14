#include "sessionclient.h"

#include "inputbackend.h"
#include "localscreens.h"
#include "screenstreamer.h"
#include "viewerwindow.h"

#include <QCborArray>

#include <algorithm>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QScreen>

Q_LOGGING_CATEGORY(lcSession, "deskflower.session")

namespace {
// Without the service's heartbeat for this long, a capturing helper gives the input back.
constexpr int kWatchdogMs = 3000;
// Initial connection attempts (one per second) before giving up.
constexpr int kMaxConnectAttempts = 10;

int i(const QCborMap &m, const char *key) { return int(m.value(QLatin1String(key)).toInteger()); }
} // namespace

SessionClient::SessionClient(const QString &instance, const QByteArray &token, InputBackend *input,
                             QObject *parent)
    : QObject(parent),
      m_serverName(df::ipc::serverName(instance)),
      m_token(token),
      m_input(input),
      m_conn(&m_socket),
      m_streamer(new ScreenStreamer(this))
{
    connect(m_streamer, &ScreenStreamer::frameReady, this, [this](const QCborMap &frame) {
        QCborMap f = frame;
        send(QStringLiteral("vframe"), f);
    });
    connect(m_streamer, &ScreenStreamer::failed, this, [this](const QString &reason) {
        QCborMap a;
        a.insert(QStringLiteral("r"), reason);
        send(QStringLiteral("vfail"), a);
    });
    connect(&m_conn, &df::ipc::Connection::messageReceived, this, &SessionClient::onMessage);
    connect(&m_socket, &QLocalSocket::connected, this, &SessionClient::onConnected);
    connect(&m_socket, &QLocalSocket::disconnected, this, &SessionClient::onDisconnected);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_socket.state() == QLocalSocket::UnconnectedState && ++m_connectAttempts > kMaxConnectAttempts) {
            qCWarning(lcSession) << "The service is not reachable: exiting";
            QCoreApplication::exit(1);
            return;
        }
        m_reconnect.start();
    });

    m_reconnect.setInterval(1000);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &SessionClient::connectToService);

    m_screensChanged.setInterval(500);
    m_screensChanged.setSingleShot(true);
    connect(&m_screensChanged, &QTimer::timeout, this, [this] {
        sendScreens();
        m_lastCapture = 0;
        captureThumbnails();
    });
    watchScreens();

    // Clipboard (only when the service gives it to this helper, see ClipboardBridge).
    connect(&m_clipboard, &ClipboardBridge::changed, this, [this](const QCborMap &formats, const QStringList &files) {
        QCborMap a;
        a.insert(QStringLiteral("f"), formats);
        a.insert(QStringLiteral("files"), QCborArray::fromStringList(files));
        send(QStringLiteral("clip"), a);
    });

    m_watchdog.setInterval(500);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_input->isCapturing() && m_sinceLastPing.elapsed() > kWatchdogMs) {
            qCWarning(lcSession) << "No heartbeat from the service: releasing the keyboard and mouse";
            m_input->setCapture(false, primaryCenter());
        }
    });
    m_watchdog.start();
    m_sinceLastPing.start();

    connect(m_input, &InputBackend::edgeHit, this, [this](int side, QPoint at) {
        QCborMap a;
        a.insert(QStringLiteral("s"), side);
        a.insert(QStringLiteral("x"), at.x());
        a.insert(QStringLiteral("y"), at.y());
        a.insert(QStringLiteral("lb"), m_input->leftButtonHeld()); // a drag may be crossing
        send(QStringLiteral("edge"), a);
    });
    connect(m_input, &InputBackend::localRelease, this, [this](QPoint at) {
        QCborMap a;
        a.insert(QStringLiteral("x"), at.x());
        a.insert(QStringLiteral("y"), at.y());
        send(QStringLiteral("dragRelease"), a);
    });
    connect(m_input, &InputBackend::moved, this, [this](int dx, int dy) {
        QCborMap a;
        a.insert(QStringLiteral("x"), dx);
        a.insert(QStringLiteral("y"), dy);
        send(QStringLiteral("mm"), a);
    });
    connect(m_input, &InputBackend::buttonEvent, this, [this](int button, bool down) {
        QCborMap a;
        a.insert(QStringLiteral("b"), button);
        a.insert(QStringLiteral("d"), down);
        send(QStringLiteral("mb"), a);
    });
    connect(m_input, &InputBackend::wheelEvent, this, [this](int dx, int dy) {
        QCborMap a;
        a.insert(QStringLiteral("x"), dx);
        a.insert(QStringLiteral("y"), dy);
        send(QStringLiteral("mw"), a);
    });
    connect(m_input, &InputBackend::keyEvent, this, [this](quint32 sc, bool down) {
        QCborMap a;
        a.insert(QStringLiteral("s"), qint64(sc));
        a.insert(QStringLiteral("d"), down);
        send(QStringLiteral("key"), a);
    });

    connectToService();
}

SessionClient::~SessionClient() = default;

void SessionClient::connectToService()
{
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        m_socket.abort();
    m_socket.connectToServer(m_serverName);
}

void SessionClient::onConnected()
{
    qCInfo(lcSession) << "Connected to the service";
    QCborMap hello;
    hello.insert(QStringLiteral("role"), QStringLiteral("session"));
    hello.insert(QStringLiteral("token"), QString::fromLatin1(m_token));
    send(QStringLiteral("hello"), hello);
    m_sinceLastPing.restart();
    sendScreens();
    m_lastCapture = 0;
    captureThumbnails();
}

void SessionClient::onDisconnected()
{
    qCInfo(lcSession) << "Disconnected from the service";
    // Never keep the user's keyboard and mouse without a service to forward them to.
    if (m_input->isCapturing())
        m_input->setCapture(false, primaryCenter());
    m_input->setReportEdges(false);
    m_input->releaseAll();
    m_streamer->stop();
    m_viewer.reset();
    // The service starts a fresh helper (with a new token) whenever it runs: an orphan would only
    // keep hooks installed, so leave.
    QCoreApplication::exit(0);
}

void SessionClient::send(const QString &cmd, QCborMap args)
{
    if (m_socket.state() != QLocalSocket::ConnectedState)
        return;
    args.insert(QStringLiteral("cmd"), cmd);
    m_conn.send(args);
}

void SessionClient::onMessage(const QCborMap &msg)
{
    const QString ev = msg.value(QStringLiteral("ev")).toString();
    if (ev == QLatin1String("mabs")) {
        qCDebug(lcSession) << "move to" << i(msg, "x") << i(msg, "y");
        m_input->setCursorHidden(false);
        m_input->moveTo(QPoint(i(msg, "x"), i(msg, "y")));
    } else if (ev == QLatin1String("dragWatch")) {
        m_input->setReportReleases(msg.value(QStringLiteral("on")).toBool());
    } else if (ev == QLatin1String("hideCursor")) {
        m_input->setCursorHidden(true);
    } else if (ev == QLatin1String("mb")) {
        m_input->setCursorHidden(false);
        m_input->button(i(msg, "b"), msg.value(QStringLiteral("d")).toBool(), QPoint(i(msg, "x"), i(msg, "y")));
    } else if (ev == QLatin1String("mw")) {
        m_input->wheel(i(msg, "x"), i(msg, "y"));
    } else if (ev == QLatin1String("key")) {
        m_input->key(quint32(i(msg, "s")), msg.value(QStringLiteral("d")).toBool());
    } else if (ev == QLatin1String("ping")) {
        m_sinceLastPing.restart();
    } else if (ev == QLatin1String("capture")) {
        m_sinceLastPing.restart();
        m_input->setCapture(msg.value(QStringLiteral("on")).toBool(), QPoint(i(msg, "x"), i(msg, "y")));
    } else if (ev == QLatin1String("config")) {
        m_input->setReportEdges(msg.value(QStringLiteral("master")).toBool());
    } else if (ev == QLatin1String("releaseAll")) {
        m_input->releaseAll();
    } else if (ev == QLatin1String("captureThumbs")) {
        captureThumbnails();
    } else if (ev == QLatin1String("viewCapture")) {
        if (msg.value(QStringLiteral("on")).toBool())
            m_streamer->start(QSize(i(msg, "w"), i(msg, "h")), QRect(i(msg, "sx"), i(msg, "sy"), i(msg, "sw"), i(msg, "sh")));
        else
            m_streamer->stop();
    } else if (ev == QLatin1String("viewCaptureScreen")) {
        m_streamer->setScreen(QRect(i(msg, "sx"), i(msg, "sy"), i(msg, "sw"), i(msg, "sh")));
    } else if (ev == QLatin1String("viewScreen")) {
        if (m_viewer)
            m_viewer->setRemoteSize(QSize(i(msg, "rw"), i(msg, "rh")));
    } else if (ev == QLatin1String("viewNext")) {
        m_streamer->next();
    } else if (ev == QLatin1String("viewOpen")) {
        m_viewDevice = msg.value(QStringLiteral("device")).toString();
        m_viewer = std::make_unique<ViewerWindow>(QRect(i(msg, "x"), i(msg, "y"), i(msg, "w"), i(msg, "h")),
                                                  QSize(i(msg, "rw"), i(msg, "rh")));
    } else if (ev == QLatin1String("viewStatus")) {
        if (m_viewer && msg.value(QStringLiteral("device")).toString() == m_viewDevice)
            m_viewer->setStatus(msg.value(QStringLiteral("text")).toString(),
                                msg.value(QStringLiteral("state")).toString() == QLatin1String("failed"));
    } else if (ev == QLatin1String("viewFrame")) {
        if (m_viewer)
            m_viewer->applyFrame(msg);
        QCborMap ack;
        ack.insert(QStringLiteral("s"), msg.value(QStringLiteral("s")));
        send(QStringLiteral("vack"), ack); // even without a window: never stall the source
    } else if (ev == QLatin1String("viewCursor")) {
        if (m_viewer)
            m_viewer->setRemoteCursor(QPoint(i(msg, "x"), i(msg, "y")));
    } else if (ev == QLatin1String("viewClose")) {
        if (m_viewer && m_viewer->hasFailed()) {
            // Keep the reason readable a moment; the window is transparent for input, so it blocks
            // nothing meanwhile.
            QTimer::singleShot(3000, m_viewer.release(), &QObject::deleteLater);
        }
        m_viewer.reset();
    } else if (ev == QLatin1String("clipConfig")) {
        m_clipboard.setEnabled(msg.value(QStringLiteral("on")).toBool());
    } else if (ev == QLatin1String("clipSet")) {
        QStringList files;
        for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
            files.append(v.toString());
        m_clipboard.apply(msg.value(QStringLiteral("f")).toMap(), files);
    }
}

void SessionClient::watchScreens()
{
    auto restart = [this] { m_screensChanged.start(); };
    auto watch = [this, restart](QScreen *screen) {
        connect(screen, &QScreen::geometryChanged, this, restart);
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, restart);
    };
    for (QScreen *screen : QGuiApplication::screens())
        watch(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [watch, restart](QScreen *screen) {
        watch(screen);
        restart();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, restart);
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, restart);
}

void SessionClient::sendScreens()
{
    QCborMap a;
    a.insert(QStringLiteral("screens"), LocalScreens::describe());
    send(QStringLiteral("localScreens"), a);
}

void SessionClient::captureThumbnails()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_socket.state() != QLocalSocket::ConnectedState || now - m_lastCapture < 2000)
        return;
    m_lastCapture = now;
    for (QScreen *screen : QGuiApplication::screens()) {
        const QByteArray jpeg = LocalScreens::captureThumbnail(screen);
        if (jpeg.isEmpty())
            continue;
        QCborMap a;
        a.insert(QStringLiteral("screen"), LocalScreens::screenId(screen));
        a.insert(QStringLiteral("jpeg"), jpeg);
        send(QStringLiteral("localThumb"), a);
    }
}

QPoint SessionClient::primaryCenter() const
{
    const QCborArray screens = LocalScreens::describe();
    for (const QCborValue &v : screens) {
        const QCborMap s = v.toMap();
        if (s.value(QStringLiteral("p")).toBool())
            return QRect(i(s, "x"), i(s, "y"), i(s, "w"), i(s, "h")).center();
    }
    return {};
}
