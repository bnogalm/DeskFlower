#include "sessionclient.h"

#include "buildinfo.h"
#include "clock.h"
#include "inputbackend.h"
#include "localscreens.h"
#include "screenstreamer.h"
#include "viewerwindow.h"

#include "clock.h"
#include "viewtypes.h"

#include <QCborArray>
#include <QEventLoop>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <utility>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QScreen>

Q_LOGGING_CATEGORY(lcSession, "deskflower.session")

namespace {
// Without the service's heartbeat for this long, a capturing helper gives the input back.
constexpr int kWatchdogMs = 3000;
// Initial connection attempts (one per second) before giving up.
constexpr int kMaxConnectAttempts = 10;

// A paste of copied files waits this long for their transfer, then fails ("paste again"): Qt
// applications give up on the clipboard after 5 s; Finder waits longer.
#ifdef Q_OS_MACOS
constexpr int kPasteWaitMs = 8000;
#else
constexpr int kPasteWaitMs = 2500;
#endif
// Thumbnails at most this often.
constexpr qint64 kThumbnailIntervalMs = 2000;

int i(const QCborMap &m, const char *key) { return int(m.value(QLatin1String(key)).toInteger()); }
} // namespace

// One request to the service on a connection of its own (clipUnseal, clipFetch): answered by the
// `replyEvent` event, which `done` gets (empty map + error when the service is gone). Deletes
// itself after `done`; deleting it earlier drops the answer.
class ServiceRequest : public QObject
{
public:
    using Done = std::function<void(const QCborMap &reply, const QString &error)>;

    // serverName: where the request goes (the render server, which sends nothing but the answer);
    // fallback: where it goes instead when nothing listens there (a service without one).
    ServiceRequest(const QString &serverName, const QString &fallback, const QString &cmd, QCborMap args,
                   const QString &replyEvent, Done done, QObject *parent)
        : QObject(parent), m_fallback(fallback), m_replyEvent(replyEvent), m_done(std::move(done))
    {
        connect(&m_socket, &QLocalSocket::readyRead, this, &ServiceRequest::drain);
        connect(&m_socket, &QLocalSocket::connected, this, [this] {
            m_connected = true;
            m_socket.write(m_frame);
        });
        connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
            if (m_connected) // a connection that never came up is the error below
                finish({}, QCoreApplication::translate("SessionClient", "The DeskFlower service closed the connection"));
        });
        // Queued: on Unix the error comes while the socket still says it is connecting.
        connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
            if (m_socket.state() != QLocalSocket::UnconnectedState)
                return;
            if (!m_connected && !m_fallback.isEmpty()) {
                send(std::exchange(m_fallback, {}));
                return;
            }
            finish({}, QCoreApplication::translate("SessionClient", "The DeskFlower service is not running"));
        }, Qt::QueuedConnection);
        args.insert(QStringLiteral("cmd"), cmd);
        const QByteArray body = args.toCborValue().toCbor();
        m_frame = QByteArray(4, Qt::Uninitialized);
        qToBigEndian<quint32>(quint32(body.size()), m_frame.data());
        m_frame += body;
        send(serverName);
    }

    bool finished() const { return m_finished; }

private:
    void send(const QString &serverName)
    {
        if (m_finished)
            return;
        m_socket.abort();
        m_socket.connectToServer(serverName); // the request goes once connected
    }

    void drain()
    {
        m_buffer += m_socket.readAll();
        while (!m_finished && m_buffer.size() >= 4) {
            const quint32 size = qFromBigEndian<quint32>(m_buffer.constData());
            if (quint32(m_buffer.size() - 4) < size)
                break;
            const QCborMap m = QCborValue::fromCbor(m_buffer.mid(4, int(size))).toMap();
            m_buffer.remove(0, int(size) + 4);
            if (m.value(QStringLiteral("ev")).toString() != m_replyEvent)
                continue; // state, thumbnails...: what every new connection gets
            finish(m, m.value(QStringLiteral("ok")).toBool() ? QString() : m.value(QStringLiteral("error")).toString());
        }
    }

    void finish(const QCborMap &reply, const QString &error)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_socket.disconnect(this);
        m_socket.abort();
        if (m_done)
            m_done(reply, error);
        deleteLater();
    }

    QLocalSocket m_socket;
    QString m_fallback;
    QString m_replyEvent;
    Done m_done;
    QByteArray m_frame;
    QByteArray m_buffer;
    bool m_connected = false;
    bool m_finished = false;
};

SessionClient::SessionClient(const QString &instance, const QByteArray &token, InputBackend *input,
                             QObject *parent)
    : QObject(parent),
      m_serverName(df::ipc::serverName(instance)),
      m_renderServerName(df::ipc::renderServerName(instance)),
      m_token(token),
      m_input(input),
      m_conn(&m_socket)
{
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
        m_lastCapture = -1;
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

    // Why keyboard and mouse sharing (or the screen) does not work here, shown in the agents.
    connect(m_input, &InputBackend::problemChanged, this, &SessionClient::sendInputStatus);

    m_watchdog.setInterval(500);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_input->isCapturing() && m_sinceLastPing.elapsed() > kWatchdogMs) {
            qCWarning(lcSession) << "No heartbeat from the service: releasing the keyboard and mouse";
            m_input->setCapture(false, primaryCenter());
        }
    });
    m_watchdog.start();
    m_sinceLastPing.start();

    connect(m_input, &InputBackend::edgeHit, this, [this](int side, QPoint at, bool captured) {
        QCborMap a;
        a.insert(QStringLiteral("s"), side);
        a.insert(QStringLiteral("x"), at.x());
        a.insert(QStringLiteral("y"), at.y());
        a.insert(QStringLiteral("lb"), m_input->leftButtonHeld()); // a drag may be crossing
        a.insert(QStringLiteral("c"), captured); // already capturing (edge map): confirm or decline
        send(QStringLiteral("edge"), a);
    });
    connect(m_input, &InputBackend::captureLost, this, [this](QPoint at) {
        QCborMap a;
        a.insert(QStringLiteral("x"), at.x());
        a.insert(QStringLiteral("y"), at.y());
        send(QStringLiteral("captureLost"), a);
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
    connect(m_input, &InputBackend::diagnostic, this, [this](const QString &line) {
        QCborMap a;
        a.insert(QStringLiteral("l"), line);
        send(QStringLiteral("diag"), a);
    });
    // Paste evidence for file data: one localPress per qualifying local press, without saying
    // which key it was. Not throttled: the backends count no autorepeat, so this is human rate.
    connect(m_input, &InputBackend::localPasteGesture, this, [this] { send(QStringLiteral("localPress")); });
    connect(m_input, &InputBackend::keyEvent, this, [this](quint32 sc, bool down) {
        QCborMap a;
        a.insert(QStringLiteral("s"), qint64(sc));
        a.insert(QStringLiteral("d"), down);
        send(QStringLiteral("key"), a);
    });

    connectToService();
}

SessionClient::~SessionClient()
{
    // Destroying the socket below would report a disconnection to a half-destroyed client (and
    // end the application): not any more.
    disconnect(&m_socket, nullptr, this, nullptr);
    delete m_paste.request;
    stopCaptures();
}

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
    sendInputStatus();
    sendScreens();
    m_lastCapture = -1;
    captureThumbnails();
}

void SessionClient::onDisconnected()
{
    qCInfo(lcSession) << "Disconnected from the service";
    // Never keep the user's keyboard and mouse without a service to forward them to.
    if (m_input->isCapturing())
        m_input->setCapture(false, primaryCenter());
    m_input->setReportEdges(false);
    m_input->setEdgeMap({});
    m_input->releaseAll();
    stopCaptures();
    m_viewers.clear();
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

void SessionClient::flushMove()
{
    m_moveFlushQueued = false;
    if (!m_pendingMove)
        return;
    const QPoint at = *std::exchange(m_pendingMove, std::nullopt);
    m_input->setCursorHidden(false);
    m_input->moveTo(at);
}

void SessionClient::onMessage(const QCborMap &msg)
{
    const QString ev = msg.value(QStringLiteral("ev")).toString();
    if (ev == QLatin1String("mabs")) {
        m_pendingMove = QPoint(i(msg, "x"), i(msg, "y"));
        if (!m_moveFlushQueued) {
            m_moveFlushQueued = true;
            QMetaObject::invokeMethod(this, &SessionClient::flushMove, Qt::QueuedConnection);
        }
        return;
    }
    flushMove(); // what came before goes first
    if (ev == QLatin1String("dragWatch")) {
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
        const int guardSide = msg.contains(QStringLiteral("ge")) ? i(msg, "ge") : -1;
        m_input->setCapture(msg.value(QStringLiteral("on")).toBool(), QPoint(i(msg, "x"), i(msg, "y")), guardSide);
    } else if (ev == QLatin1String("config")) {
        const bool master = msg.value(QStringLiteral("master")).toBool();
        m_input->setReportEdges(master);
        if (!master)
            m_input->setEdgeMap({});
    } else if (ev == QLatin1String("edges")) {
        // Where a push past an edge is known to cross (master only): the backend captures there
        // without waiting for the service.
        QList<InputBackend::EdgeSpan> spans;
        for (const QCborValue &v : msg.value(QStringLiteral("e")).toArray()) {
            const QCborMap e = v.toMap();
            spans.append({QRect(i(e, "x"), i(e, "y"), i(e, "w"), i(e, "h")), i(e, "s"), i(e, "a"), i(e, "b")});
        }
        m_input->setEdgeMap(spans);
    } else if (ev == QLatin1String("keyDiag")) { // ignored without keyboard diagnostics built
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
        const bool on = msg.value(QStringLiteral("on")).toBool();
        if (on) {
            QCborMap a;
            a.insert(QStringLiteral("l"), QStringLiteral("recording (helper %1, capturing: %2)")
                                              .arg(df::build::version(), m_input->isCapturing() ? QStringLiteral("yes")
                                                                                                : QStringLiteral("no")));
            send(QStringLiteral("diag"), a);
        }
        m_input->setKeyDiagnostics(on);
#endif
    } else if (ev == QLatin1String("releaseAll")) {
        m_input->releaseAll();
    } else if (ev == QLatin1String("captureThumbs")) {
        captureThumbnails();
    } else if (ev == QLatin1String("viewCapture")) {
        if (msg.value(QStringLiteral("on")).toBool())
            setCaptures(msg.value(QStringLiteral("screens")).toArray());
        else
            stopCaptures();
    } else if (ev == QLatin1String("viewNext")) {
        if (const auto it = m_captures.constFind(i(msg, "i")); it != m_captures.constEnd())
            it->streamer->next();
    } else if (ev == QLatin1String("viewOpen")) {
        m_viewDevice = msg.value(QStringLiteral("device")).toString();
        m_viewStatus.clear();
        m_viewFailed = false;
        m_viewers.clear();
        showViewers(msg.value(QStringLiteral("screens")).toArray());
    } else if (ev == QLatin1String("viewShow")) {
        showViewers(msg.value(QStringLiteral("screens")).toArray());
    } else if (ev == QLatin1String("viewStatus")) {
        if (msg.value(QStringLiteral("device")).toString() == m_viewDevice) {
            m_viewStatus = msg.value(QStringLiteral("text")).toString();
            m_viewFailed = msg.value(QStringLiteral("state")).toString() == QLatin1String("failed");
            for (const auto &viewer : m_viewers)
                viewer->setStatus(m_viewStatus, m_viewFailed);
        }
    } else if (ev == QLatin1String("viewFrame")) {
        if (ViewerWindow *viewer = viewerFor(i(msg, "i")))
            viewer->applyFrame(msg);
        QCborMap ack;
        ack.insert(QStringLiteral("s"), msg.value(QStringLiteral("s")));
        ack.insert(QStringLiteral("i"), msg.value(QStringLiteral("i")));
        send(QStringLiteral("vack"), ack); // even without a window: never stall the source
    } else if (ev == QLatin1String("viewCursor")) {
        // Drawn on the window showing that screen only.
        const int index = i(msg, "i");
        for (const auto &viewer : m_viewers)
            viewer->setRemoteCursor(viewer->index() == index ? QPoint(i(msg, "x"), i(msg, "y")) : QPoint(-1, -1));
    } else if (ev == QLatin1String("viewClose")) {
        closeViewers();
    } else if (ev == QLatin1String("clipConfig")) {
        m_clipboard.setEnabled(msg.value(QStringLiteral("on")).toBool());
    } else if (ev == QLatin1String("clipClear")) {
        m_clipboard.clearOwned(); // the service holds nothing for our lazy clipboard any more
    } else if (ev == QLatin1String("clipSet")) {
        const quint64 generation = ++m_clipGeneration; // a paste fetch of the previous one is dropped
        QStringList files;
        for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
            files.append(v.toString());
        if (msg.contains(QStringLiteral("sealed"))) {
            const QByteArray kid = msg.value(QStringLiteral("kid")).toByteArray();
            QList<ClipboardBridge::SealedFormat> formats;
            for (const QCborValue &v : msg.value(QStringLiteral("sealed")).toArray()) {
                const QCborArray item = v.toArray();
                formats.append(ClipboardBridge::SealedFormat{item.at(0).toString(), item.at(1).toInteger()});
            }
            const int pending = int(msg.value(QStringLiteral("pendingFiles")).toInteger());
            auto unseal = [this, kid](const QString &mime, QString *error) {
                return unsealFromService(kid, mime, error);
            };
#ifdef Q_OS_MACOS
            // One pasteboard item per top-level file or folder: by name, from the full pendingList
            // only (an incomplete or missing list, e.g. for a guest or a long copy, gets the
            // unlisted fallback of the bridge); files already local by their own names.
            QList<ClipboardBridge::ListedFile> listed;
            if (!files.isEmpty()) {
                for (const QString &f : std::as_const(files))
                    listed.append(ClipboardBridge::ListedFile{QFileInfo(f).fileName(), 0, QFileInfo(f).isDir()});
            } else {
                for (const QCborValue &v : msg.value(QStringLiteral("pendingList")).toArray())
                    listed.append(ClipboardBridge::listedFromCbor(v.toArray()));
                if (listed.size() != pending)
                    listed.clear();
            }
#else
            // Copied files are transferred when an application reads the uri-list (the paste), so
            // the master can grant it then; files already local are offered as they are.
            const QList<ClipboardBridge::ListedFile> listed;
#endif
            auto fetch = [this, files, generation, kid, pending](QString *error) {
                return files.isEmpty() ? pastedFiles(generation, kid, pending, error) : files;
            };
            m_clipboard.applySealed(formats, listed, files.isEmpty() ? pending : int(files.size()), unseal, fetch,
                                    msg.value(QStringLiteral("gated")).toBool());
        } else {
            m_clipboard.apply(msg.value(QStringLiteral("f")).toMap(), files);
        }
    }
}

QCborMap SessionClient::requestFromService(const QString &cmd, QCborMap args, const QString &replyEvent,
                                           int timeoutMs, QString *error) const
{
    // Called from inside an application's clipboard request (QMimeData::retrieveData, on this
    // thread, from the selection handling of the event loop): the answer is awaited in a nested
    // event loop rather than a blocking read, so that the service's injections, pings and edge
    // reports keep flowing while a paste is served; only this process's own user input waits. The
    // clipboard object being rendered is kept alive meanwhile (ClipboardBridge defers a
    // replacement until the render returns).
    QCborMap reply;
    QString failure;
    bool answered = false;
    QEventLoop loop;
    // Pastes go to the render server; a service without one answers on the main server.
    auto *request = new ServiceRequest(m_renderServerName, m_serverName, cmd, args, replyEvent,
                                       [&](const QCborMap &r, const QString &e) {
                                           reply = r;
                                           failure = e;
                                           answered = true;
                                           loop.quit();
                                       },
                                       nullptr);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!answered) {
        delete request; // its answer would come to a gone loop
        *error = tr("No answer from the DeskFlower service");
        return {};
    }
    *error = failure;
    return reply;
}

QByteArray SessionClient::unsealFromService(const QByteArray &kid, const QString &mime, QString *error) const
{
    QCborMap args;
    args.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
    args.insert(QStringLiteral("mime"), mime);
    const QCborMap m = requestFromService(QStringLiteral("clipUnseal"), args, QStringLiteral("clipUnsealed"),
                                          df::ipc::kClipboardRenderTimeoutMs, error);
    return m.value(QStringLiteral("ok")).toBool() ? m.value(QStringLiteral("data")).toByteArray() : QByteArray();
}

QStringList SessionClient::pastedFiles(quint64 generation, const QByteArray &kid, int count, QString *error)
{
    if (m_paste.generation != generation) {
        delete m_paste.request; // a fetch for an older clipboard: its answer is not wanted
        m_paste = PasteFetch{};
        m_paste.generation = generation;
    }
    if (m_paste.done) {
        if (!m_paste.files.isEmpty())
            return m_paste.files;
        // Failed or refused: said once; the next paste asks again.
        *error = m_paste.error;
        m_paste = PasteFetch{};
        m_paste.generation = generation;
        return {};
    }
    if (!m_paste.request) {
        m_paste.startedAt = df::nowMs();
        QCborMap args;
        if (!kid.isEmpty()) // the service refuses a clipboard that is not its current one
            args.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
        m_paste.request = new ServiceRequest(
            m_renderServerName, m_serverName, QStringLiteral("clipFetch"), args, QStringLiteral("clipFetched"),
            [this, generation, count](const QCborMap &reply, const QString &failure) {
                if (m_paste.generation != generation)
                    return;
                m_paste.request = nullptr;
                m_paste.done = true;
                if (reply.value(QStringLiteral("ok")).toBool()) {
                    for (const QCborValue &v : reply.value(QStringLiteral("files")).toArray())
                        m_paste.files.append(v.toString());
                }
                m_paste.error = failure;
                if (reply.value(QStringLiteral("code")).toString() == QLatin1String("noPaste")) {
                    // A reader that is no paste (a file manager looking at the clipboard): nothing
                    // was transferred and nobody is told; the paste that follows asks again.
                    m_paste.error = QString::fromLatin1(ClipboardBridge::kNotAPaste);
                    if (m_paste.toldToWait) {
                        m_paste = PasteFetch{};
                        m_paste.generation = generation;
                    }
                    return;
                }
                if (reply.value(QStringLiteral("code")).toString() == QLatin1String("cancelled")
                    && m_paste.toldToWait) {
                    // The user cancelled it (DeskFlower's progress window): nothing went wrong and
                    // nothing to tell; the next paste asks again.
                    m_paste = PasteFetch{};
                    m_paste.generation = generation;
                    return;
                }
                if (m_paste.files.isEmpty() && m_paste.error.isEmpty())
                    m_paste.error = tr("Nothing was received");
                if (!m_paste.toldToWait)
                    return; // the paste waiting got them (or its refusal), nothing to add
                if (!m_paste.files.isEmpty()) {
                    notice(tr("%n pasted item(s) arrived: paste again to put them here", nullptr, count), false);
                } else {
                    notice(tr("The pasted items could not be received: %1").arg(m_paste.error), true);
                    m_paste = PasteFetch{}; // told: the next paste asks again
                    m_paste.generation = generation;
                }
            },
            this);
    }
    // Held a moment: small transfers complete within the paste itself.
    const int left = int(kPasteWaitMs - (df::nowMs() - m_paste.startedAt));
    if (left > 0) {
        QEventLoop loop;
        QTimer poll;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (m_paste.done || m_paste.generation != generation)
                loop.quit();
        });
        poll.start(20);
        QTimer::singleShot(left, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    if (m_paste.generation == generation && m_paste.done)
        return pastedFiles(generation, kid, count, error);
    *error = QStringLiteral("Still being transferred");
    if (m_paste.generation == generation && !m_paste.toldToWait) {
        m_paste.toldToWait = true;
        notice(tr("Receiving %n pasted item(s)… paste again once they arrived", nullptr, count), false);
    }
    return {};
}

void SessionClient::notice(const QString &text, bool problem)
{
    QCborMap a;
    a.insert(QStringLiteral("text"), text);
    a.insert(QStringLiteral("problem"), problem);
    send(QStringLiteral("notice"), a);
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

void SessionClient::sendInputStatus()
{
    QCborMap a;
    a.insert(QStringLiteral("problem"), m_input->problem());
    a.insert(QStringLiteral("link"), m_input->problemLink());
    send(QStringLiteral("inputStatus"), a);
    // Sent at connect and whenever the backend's state changes (macOS: the event tap appears once
    // Accessibility allows it), so the service knows whether local presses can be seen at all.
    QCborMap support;
    support.insert(QStringLiteral("on"), m_input->reportsLocalPresses());
    send(QStringLiteral("localPressSupport"), support);
}

void SessionClient::captureThumbnails()
{
    const qint64 now = df::nowMs();
    if (m_socket.state() != QLocalSocket::ConnectedState
        || (m_lastCapture >= 0 && now - m_lastCapture < kThumbnailIntervalMs))
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

void SessionClient::setCaptures(const QCborArray &screens)
{
    QHash<int, Capture> next;
    for (const QCborValue &v : screens) {
        if (next.size() >= df::kMaxViewScreens)
            break;
        const QCborMap e = v.toMap();
        const int index = i(e, "i");
        const QRect screen(i(e, "sx"), i(e, "sy"), i(e, "sw"), i(e, "sh"));
        const QSize maxSize(i(e, "w"), i(e, "h"));
        const quint32 generation = quint32(e.value(QStringLiteral("g")).toInteger());
        if (index < 0 || next.contains(index))
            continue;
        Capture c = m_captures.take(index);
        if (!c.streamer) {
            c.streamer = new ScreenStreamer(index, this);
            connect(c.streamer, &ScreenStreamer::frameReady, this,
                    [this](const QCborMap &frame) { send(QStringLiteral("vframe"), frame); });
            connect(c.streamer, &ScreenStreamer::failed, this, [this, index](const QString &reason) {
                QCborMap a;
                a.insert(QStringLiteral("i"), index);
                a.insert(QStringLiteral("r"), reason);
                send(QStringLiteral("vfail"), a);
            });
        }
        // A kept screen goes on as it is; a new or changed one, or one the service gave a new
        // generation (a new viewer), starts afresh: the service expects a full frame of it, tagged
        // with that generation, and ignores the acknowledgement of the previous one.
        if (!c.streamer->isRunning() || c.screen != screen || c.maxSize != maxSize || c.generation != generation)
            c.streamer->start(maxSize, screen, generation);
        c.screen = screen;
        c.maxSize = maxSize;
        c.generation = generation;
        next.insert(index, c);
    }
    stopCaptures(); // what is left: screens no longer shown
    m_captures = next;
}

void SessionClient::stopCaptures()
{
    // Deleted once their threads end: this thread also moves the master's cursor and must not
    // wait for a capture in progress.
    for (const Capture &c : std::as_const(m_captures))
        c.streamer->shutdown();
    m_captures.clear();
}

void SessionClient::showViewers(const QCborArray &screens)
{
    // A screen that moves to another monitor takes its image along; a monitor that changes
    // screen starts blank (or with that image) until the next full frame.
    QHash<int, df::view::FrameCanvas> canvases;
    for (const auto &viewer : m_viewers)
        canvases.insert(viewer->index(), viewer->canvas());
    std::vector<std::unique_ptr<ViewerWindow>> next;
    for (const QCborValue &v : screens) {
        if (int(next.size()) >= df::kMaxViewScreens)
            break;
        const QCborMap e = v.toMap();
        const int index = i(e, "i");
        const QRect monitor(i(e, "x"), i(e, "y"), i(e, "w"), i(e, "h"));
        const QSize remote(i(e, "rw"), i(e, "rh"));
        std::unique_ptr<ViewerWindow> viewer;
        for (auto &existing : m_viewers) {
            if (existing && existing->nativeMonitor() == monitor)
                viewer = std::move(existing);
        }
        if (!viewer) {
            viewer = std::make_unique<ViewerWindow>(monitor, index, remote);
            if (!m_viewStatus.isEmpty())
                viewer->setStatus(m_viewStatus, m_viewFailed); // opened mid-view: the status so far
        }
        viewer->setRemote(index, remote, canvases.value(index)); // its own canvas stays if unchanged
        next.push_back(std::move(viewer));
    }
    m_viewers = std::move(next); // monitors no longer listed close
}

void SessionClient::closeViewers()
{
    if (!m_viewers.empty() && m_viewers.front()->hasFailed()) {
        // Keep the reason readable a moment on the first window; it is transparent for input, so
        // it blocks nothing meanwhile.
        QTimer::singleShot(3000, m_viewers.front().release(), &QObject::deleteLater);
    }
    m_viewers.clear();
}

ViewerWindow *SessionClient::viewerFor(int index) const
{
    for (const auto &viewer : m_viewers) {
        if (viewer->index() == index)
            return viewer.get();
    }
    return nullptr;
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
