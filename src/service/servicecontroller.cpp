#include "servicecontroller.h"

#include "buildinfo.h"
#include "crypto.h"
#include "platform/platform.h"

#include <QCborArray>
#include <QCoreApplication>
#include <QDir>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <utility>

Q_LOGGING_CATEGORY(lcService, "deskflower.service")

namespace df {

namespace {
QString hexId(const QByteArray &id) { return QString::fromLatin1(id.toHex()); }
QByteArray fromHex(const QCborValue &v) { return QByteArray::fromHex(v.toString().toLatin1()); }

QCborMap makeEvent(const QString &name)
{
    QCborMap m;
    m.insert(QStringLiteral("ev"), name);
    return m;
}

QString viewStateName(RemoteView::ViewState state)
{
    switch (state) {
    case RemoteView::ViewState::Connecting: return QStringLiteral("connecting");
    case RemoteView::ViewState::AwaitingApproval: return QStringLiteral("waiting");
    case RemoteView::ViewState::Starting: return QStringLiteral("starting");
    case RemoteView::ViewState::Streaming: return QStringLiteral("streaming");
    case RemoteView::ViewState::Failed: break;
    }
    return QStringLiteral("failed");
}

QString updateStateName(Updater::State state)
{
    switch (state) {
    case Updater::State::Downloading: return QStringLiteral("downloading");
    case Updater::State::Verifying: return QStringLiteral("verifying");
    case Updater::State::Installing: return QStringLiteral("installing");
    case Updater::State::Idle: break;
    }
    return QString();
}
} // namespace

ServiceController::ServiceController(Node *node, Workspace *workspace, InputController *input,
                                     ClipboardSync *clipboard, FileTransfer *transfer, Settings *settings,
                                     web::PhoneServer *phone,
                                     RemoteView *remoteView, Updater *updater, std::function<QString()> sharedFolder,
                                     std::function<QString()> receivedFolder, const QString &instance,
                                     bool serviceMode, const QByteArray &sessionToken, QObject *parent)
    : QObject(parent),
      m_node(node),
      m_workspace(workspace),
      m_input(input),
      m_clipboard(clipboard),
      m_settings(settings),
      m_phone(phone),
      m_remoteView(remoteView),
      m_updater(updater),
      m_sharedFolder(std::move(sharedFolder)),
      m_receivedFolder(std::move(receivedFolder)),
      m_sessionToken(sessionToken),
      m_instance(instance),
      m_serviceMode(serviceMode)
{
    // Dragging files between devices: the agent (running as the user) talks to Explorer.
    FileDrag::Agent agent;
    agent.catchFiles = [this](QPoint at, std::function<void(const QStringList &)> done) {
        QCborMap a;
        a.insert(QStringLiteral("x"), at.x());
        a.insert(QStringLiteral("y"), at.y());
        askAgent(QStringLiteral("dragCatch"), a, 1200, [done](const QCborMap &r) {
            QStringList paths;
            for (const QCborValue &v : r.value(QStringLiteral("paths")).toArray())
                paths.append(v.toString());
            done(paths);
        });
    };
    agent.hover = [this](const FileDrag::Hover &hover, std::function<void(bool, bool)> done) {
        QCborMap a;
        a.insert(QStringLiteral("o"), qint64(hover.offer));
        a.insert(QStringLiteral("own"), hover.own);
        a.insert(QStringLiteral("paths"), QCborArray::fromStringList(hover.paths));
        QCborArray files;
        for (const FileDrag::ListedFile &f : hover.files)
            files.append(QCborArray{f.path, f.size});
        a.insert(QStringLiteral("files"), files);
        a.insert(QStringLiteral("n"), hover.count);
        // Answered when the drag there ends (dropped or cancelled).
        askAgent(QStringLiteral("dragHover"), a, 60 * 60 * 1000, [done](const QCborMap &r) {
            done(r.value(QStringLiteral("dropped")).toBool(), r.value(QStringLiteral("transferred")).toBool());
        });
    };
    agent.endHover = [this](bool drop) {
        QCborMap ev = makeEvent(QStringLiteral("dragHoverEnd"));
        ev.insert(QStringLiteral("drop"), drop);
        broadcast(ev);
    };
    agent.folderAt = [this](QPoint at, std::function<void(const QString &)> done) {
        QCborMap a;
        a.insert(QStringLiteral("x"), at.x());
        a.insert(QStringLiteral("y"), at.y());
        askAgent(QStringLiteral("dropFolder"), a, 3000,
                 [done](const QCborMap &r) { done(r.value(QStringLiteral("folder")).toString()); });
    };
    agent.recycle = [this](const QStringList &paths, std::function<void(bool, const QString &)> done) {
        QCborMap a;
        a.insert(QStringLiteral("paths"), QCborArray::fromStringList(paths));
        askAgent(QStringLiteral("recycle"), a, 60000, [done](const QCborMap &r) {
            if (r.isEmpty())
                done(false, tr("DeskFlower is not running for the signed-in user"));
            else
                done(r.value(QStringLiteral("ok")).toBool(), r.value(QStringLiteral("error")).toString());
        });
    };
    m_fileDrag = std::make_unique<FileDrag>(m_node, transfer, m_workspace, m_input, agent, m_receivedFolder);
    connect(m_fileDrag.get(), &FileDrag::notice, this, [this](const QString &text, bool problem) {
        QCborMap ev = makeEvent(QStringLiteral("notice"));
        ev.insert(QStringLiteral("message"), text);
        ev.insert(QStringLiteral("problem"), problem);
        broadcast(ev);
    });

    connect(m_input, &InputController::inputRefused, this, [this](const QByteArray &device) {
        QString name = QString::fromLatin1(device.toHex().left(8));
        for (const PeerView &p : m_node->peers()) {
            if (p.id == device)
                name = p.name;
        }
        QCborMap ev = makeEvent(QStringLiteral("notice"));
        ev.insert(QStringLiteral("message"),
                  tr("%1 cannot take the keyboard and mouse right now (is DeskFlower running there?)").arg(name));
        ev.insert(QStringLiteral("problem"), true);
        broadcast(ev);
    });

    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(30);
    connect(&m_stateTimer, &QTimer::timeout, this, [this] { broadcast(stateEvent()); });

    connect(m_node, &Node::peersChanged, this, &ServiceController::scheduleState);
    connect(m_node, &Node::explorationChanged, this, &ServiceController::scheduleState);
    connect(m_workspace, &Workspace::changed, this, &ServiceController::scheduleState);
    connect(m_workspace, &Workspace::thumbnailChanged, this, [this](const QByteArray &device, const QString &screen) {
        broadcast(thumbEvent(device, screen, m_workspace->thumbnail(device, screen)));
    });
    connect(m_workspace, &Workspace::localThumbnailsRequested, this,
            [this] { sendToSession(makeEvent(QStringLiteral("captureThumbs"))); });
    connect(m_input, &InputController::toSession, this, &ServiceController::sendToSession);
    connect(m_input, &InputController::activeDeviceChanged, this, &ServiceController::scheduleState);
    connect(m_clipboard, &ClipboardSync::applyLocally, this,
            [this](const ClipboardData &data, const QStringList &files, int pendingFiles) {
                QCborMap formats;
                for (auto it = data.constBegin(); it != data.constEnd(); ++it)
                    formats.insert(it.key(), it.value());
                auto send = [this, formats](const QStringList &paths, int pending) {
                    QCborMap ev = makeEvent(QStringLiteral("clipSet"));
                    ev.insert(QStringLiteral("f"), formats);
                    ev.insert(QStringLiteral("files"), QCborArray::fromStringList(paths));
                    ev.insert(QStringLiteral("pendingFiles"), pending);
                    if (pending > 0) {
                        QCborArray list;
                        for (const ClipboardSync::ListedFile &f : m_clipboard->pendingFileList())
                            list.append(QCborArray{f.path, f.size});
                        ev.insert(QStringLiteral("pendingList"), list);
                    }
                    sendToClipboardOwner(ev);
                };
                if (pendingFiles > 0 && !platform::clipboardInAgent(m_serviceMode)) {
                    // The session helper has no deferred paste: transfer the files first.
                    m_clipboard->fetchFiles([send](bool ok, const QStringList &fetched, const QString &) {
                        if (ok)
                            send(fetched, 0);
                    });
                    if (!formats.isEmpty())
                        send({}, 0);
                    return;
                }
                send(files, pendingFiles);
            });
    // Remote view: viewer side (we are the master) and source side (our screen is shown elsewhere).
    connect(m_input, &InputController::viewRequested, this,
            [this](const QByteArray &device, QSize monitor, const QRect &screen) {
                m_remoteView->startViewing(device, monitor, screen);
            });
    connect(m_input, &InputController::viewScreenChanged, this,
            [this](const QByteArray &, const QRect &screen) { m_remoteView->switchScreen(screen); });
    connect(m_input, &InputController::viewEnded, m_remoteView, &RemoteView::stopViewing);
    connect(m_remoteView, &RemoteView::viewStateChanged, this,
            [this](const QByteArray &device, RemoteView::ViewState state, const QString &text) {
                QCborMap ev = makeEvent(QStringLiteral("viewStatus"));
                ev.insert(QStringLiteral("device"), hexId(device));
                ev.insert(QStringLiteral("state"), viewStateName(state));
                ev.insert(QStringLiteral("text"), text);
                sendToSession(ev);
                // Input reaches the source only once it accepted (no blind clicks on its prompt).
                if (state == RemoteView::ViewState::Starting || state == RemoteView::ViewState::Streaming)
                    m_input->setViewAccepted(device);
            });
    connect(m_remoteView, &RemoteView::viewingFailed, this, [this](const QString &reason) {
        m_input->cancelView(); // the cursor goes back home; the viewer window keeps the reason a moment
        QCborMap ev = makeEvent(QStringLiteral("error"));
        ev.insert(QStringLiteral("message"), tr("Remote view: %1").arg(reason));
        broadcast(ev);
    });
    connect(m_remoteView, &RemoteView::frameReceived, this, [this](const QCborMap &frame) {
        QCborMap ev = frame;
        ev.insert(QStringLiteral("ev"), QStringLiteral("viewFrame"));
        sendToSession(ev);
    });
    auto insertScreen = [](QCborMap *ev, const QRect &screen) {
        ev->insert(QStringLiteral("sx"), screen.x());
        ev->insert(QStringLiteral("sy"), screen.y());
        ev->insert(QStringLiteral("sw"), screen.width());
        ev->insert(QStringLiteral("sh"), screen.height());
    };
    connect(m_remoteView, &RemoteView::captureRequested, this, [this, insertScreen](QSize maxSize, const QRect &screen) {
        QCborMap ev = makeEvent(QStringLiteral("viewCapture"));
        ev.insert(QStringLiteral("on"), true);
        ev.insert(QStringLiteral("w"), maxSize.width());
        ev.insert(QStringLiteral("h"), maxSize.height());
        insertScreen(&ev, screen);
        sendToSession(ev);
    });
    connect(m_remoteView, &RemoteView::captureScreenChanged, this, [this, insertScreen](const QRect &screen) {
        QCborMap ev = makeEvent(QStringLiteral("viewCaptureScreen"));
        insertScreen(&ev, screen);
        sendToSession(ev);
    });
    connect(m_remoteView, &RemoteView::captureNext, this,
            [this] { sendToSession(makeEvent(QStringLiteral("viewNext"))); });
    connect(m_remoteView, &RemoteView::captureStopped, this, [this] {
        QCborMap ev = makeEvent(QStringLiteral("viewCapture"));
        ev.insert(QStringLiteral("on"), false);
        sendToSession(ev);
    });
    // Source side: the local user decides who may see this screen (replayed like pairing prompts).
    connect(m_remoteView, &RemoteView::authorizationRequested, this,
            [this](const QByteArray &peer, const QString &name, int timeoutMs) {
                m_pendingViewPrompt = makeEvent(QStringLiteral("viewPrompt"));
                m_pendingViewPrompt.insert(QStringLiteral("id"), hexId(peer));
                m_pendingViewPrompt.insert(QStringLiteral("name"), name);
                m_pendingViewPrompt.insert(QStringLiteral("timeout"), timeoutMs);
                broadcast(m_pendingViewPrompt);
                m_input->setRemoteInputBlocked(peer, true); // the requester cannot answer for us
            });
    connect(m_remoteView, &RemoteView::authorizationClosed, this, [this](const QByteArray &peer) {
        m_pendingViewPrompt.clear();
        QCborMap ev = makeEvent(QStringLiteral("viewPromptDone"));
        ev.insert(QStringLiteral("id"), hexId(peer));
        broadcast(ev);
        m_input->setRemoteInputBlocked(peer, false);
    });
    connect(m_remoteView, &RemoteView::streamingChanged, this, &ServiceController::scheduleState);

    // Updates from paired devices (started from an agent).
    connect(m_updater, &Updater::stateChanged, this, &ServiceController::scheduleState);
    // The installer replaces the agent's files, and from session 0 it cannot close the agent itself.
    connect(m_updater, &Updater::aboutToInstall, this, [this] { broadcast(makeEvent(QStringLiteral("quit"))); });
    connect(m_updater, &Updater::finished, this, [this](const QByteArray &, bool ok, const QString &error) {
        QCborMap ev = makeEvent(QStringLiteral("updateResult"));
        ev.insert(QStringLiteral("ok"), ok);
        ev.insert(QStringLiteral("message"), ok ? tr("Installing the update. DeskFlower restarts when it is done.")
                                                : tr("Update failed: %1").arg(error));
        broadcast(ev);
        scheduleState();
    });

    // Phone access.
    connect(m_phone, &web::PhoneServer::sessionsChanged, this, &ServiceController::scheduleState);
    connect(m_phone, &web::PhoneServer::accessTokenChanged, this, [this] {
        m_phoneUrls = m_phone->accessUrls(m_phone->accessToken());
        scheduleState();
    });
    connect(m_phone, &web::PhoneServer::fileUploaded, this, [this](const QString &path) {
        QCborMap ev = makeEvent(QStringLiteral("phoneUpload"));
        ev.insert(QStringLiteral("path"), path);
        broadcast(ev);
    });
    connect(m_phone, &web::PhoneServer::textReceived, this, [this](const QString &text) {
        // Onto this computer's clipboard, and the group's when clipboard sharing is on.
        const ClipboardData data{{QStringLiteral("text/plain"), text.toUtf8()}};
        m_clipboard->localChanged(data);
        QCborMap formats;
        formats.insert(QStringLiteral("text/plain"), text.toUtf8());
        QCborMap ev = makeEvent(QStringLiteral("clipSet"));
        ev.insert(QStringLiteral("f"), formats);
        sendToClipboardOwner(ev);
    });
    if (m_settings->value(QStringLiteral("phoneAccess")).toBool())
        QTimer::singleShot(0, this, [this] { setPhoneAccess(true); });

    connect(m_clipboard, &ClipboardSync::filesNotice, this, [this](const QString &text, bool problem) {
        QCborMap ev = makeEvent(QStringLiteral("notice"));
        ev.insert(QStringLiteral("message"), text);
        ev.insert(QStringLiteral("problem"), problem);
        broadcast(ev);
    });
    connect(m_clipboard, &ClipboardSync::enabledChanged, this, [this] {
        sendClipboardConfig();
        scheduleState();
    });

    connect(m_node, &Node::pairingPinGenerated, this,
            [this](const QByteArray &id, const QString &name, const QString &pin) {
                m_pendingPin = makeEvent(QStringLiteral("pairPin"));
                m_pendingPin.insert(QStringLiteral("id"), hexId(id));
                m_pendingPin.insert(QStringLiteral("name"), name);
                m_pendingPin.insert(QStringLiteral("pin"), pin);
                broadcast(m_pendingPin);
            });
    connect(m_node, &Node::pairingPinRequired, this, [this](const QByteArray &id, const QString &name) {
        m_pendingPrompt = makeEvent(QStringLiteral("pairPrompt"));
        m_pendingPrompt.insert(QStringLiteral("id"), hexId(id));
        m_pendingPrompt.insert(QStringLiteral("name"), name);
        broadcast(m_pendingPrompt);
    });
    connect(m_node, &Node::pairingFinished, this, [this](const QByteArray &id, bool ok, const QString &error) {
        m_pendingPin.clear();
        m_pendingPrompt.clear();
        QCborMap ev = makeEvent(QStringLiteral("pairResult"));
        ev.insert(QStringLiteral("id"), hexId(id));
        ev.insert(QStringLiteral("ok"), ok);
        ev.insert(QStringLiteral("error"), error);
        broadcast(ev);
    });
}

bool ServiceController::listen(QString *error)
{
    const QString name = ipc::serverName(m_instance);
    QLocalServer::removeServer(name);
    // As a Windows service the agent runs under another account, so the pipe must be reachable by
    // local users; isClientAllowed() then restricts it to the interactive console session.
    m_server.setSocketOptions(m_serviceMode ? QLocalServer::WorldAccessOption : QLocalServer::UserAccessOption);
    if (!m_server.listen(name)) {
        *error = QStringLiteral("Cannot listen on %1: %2").arg(name, m_server.errorString());
        return false;
    }
    connect(&m_server, &QLocalServer::newConnection, this, &ServiceController::onNewConnection);
    return true;
}

void ServiceController::onNewConnection()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        if (!platform::isClientAllowed(socket, m_serviceMode)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto *conn = new ipc::Connection(socket, this);
        m_clients.append(conn);
        connect(conn, &ipc::Connection::messageReceived, this,
                [this, conn](const QCborMap &msg) { handleCommand(conn, msg); });
        connect(socket, &QLocalSocket::disconnected, this, [this, conn, socket] {
            m_clients.removeAll(conn);
            if (conn == m_session) {
                m_session = nullptr;
                m_input->sessionDisconnected();
            }
            updateUserPresence();
            conn->deleteLater();
            socket->deleteLater();
        });
        conn->send(stateEvent());
        if (platform::clipboardInAgent(m_serviceMode))
            conn->send(clipboardConfig(true));
        if (!m_pendingPin.isEmpty())
            conn->send(m_pendingPin);
        if (!m_pendingPrompt.isEmpty())
            conn->send(m_pendingPrompt);
        if (!m_pendingViewPrompt.isEmpty())
            conn->send(m_pendingViewPrompt);
        for (const QByteArray &device : m_workspace->devices()) {
            const auto thumbs = m_workspace->thumbnails(device);
            for (auto it = thumbs.constBegin(); it != thumbs.constEnd(); ++it)
                conn->send(thumbEvent(device, it.key(), it.value()));
        }
        updateUserPresence();
    }
}

void ServiceController::handleCommand(ipc::Connection *conn, const QCborMap &msg)
{
    const QString cmd = msg.value(QStringLiteral("cmd")).toString();
    const QByteArray id = fromHex(msg.value(QStringLiteral("id")));

    if (conn == m_session) {
        handleSessionCommand(cmd, msg);
        return;
    }
    if (cmd == QLatin1String("hello")) {
        const QByteArray token = msg.value(QStringLiteral("token")).toString().toLatin1();
        if (msg.value(QStringLiteral("role")).toString() == QLatin1String("session") && !m_sessionToken.isEmpty()
            && crypto::constantTimeEqual(token, m_sessionToken)) {
            if (m_session)
                m_session->socket()->disconnectFromServer(); // a restarted helper replaces the old one
            m_clients.removeAll(conn);
            m_session = conn;
            m_input->sessionConnected();
            sendClipboardConfig();
            // A new helper may belong to another user session: a live view does not carry over.
            if (m_remoteView->isStreaming())
                m_remoteView->setLocalUserPresent(false);
            updateUserPresence();
        } else {
            conn->socket()->abort();
        }
        return;
    }

    if (cmd == QLatin1String("clip")) {
        if (platform::clipboardInAgent(m_serviceMode))
            handleClipboardChange(msg);
        return;
    }
    if (cmd == QLatin1String("agentReply")) {
        if (auto done = m_agentRequests.take(msg.value(QStringLiteral("rid")).toInteger()))
            done(msg);
        return;
    }
    if (cmd == QLatin1String("dragFetch")) {
        // The drop target reads the dragged files: transfer them now, then answer.
        const QPointer<ipc::Connection> requester(conn);
        m_fileDrag->fetchDragged(quint64(msg.value(QStringLiteral("o")).toInteger()),
                                 [this, requester](bool ok, const QStringList &items, const QString &error) {
                                     if (!requester)
                                         return;
                                     QStringList native;
                                     for (const QString &f : items)
                                         native << QDir::toNativeSeparators(f);
                                     QCborMap ev = makeEvent(QStringLiteral("dragFetched"));
                                     ev.insert(QStringLiteral("ok"), ok);
                                     ev.insert(QStringLiteral("files"), QCborArray::fromStringList(native));
                                     ev.insert(QStringLiteral("error"), error);
                                     requester->send(ev);
                                 });
        return;
    }
    if (cmd == QLatin1String("clipFetch")) {
        // The user pastes files copied on another device: transfer them now, then answer.
        const QPointer<ipc::Connection> requester(conn);
        m_clipboard->fetchFiles([this, requester](bool ok, const QStringList &files, const QString &error) {
            if (!requester)
                return;
            QStringList native;
            for (const QString &f : files)
                native << QDir::toNativeSeparators(f);
            QCborMap ev = makeEvent(QStringLiteral("clipFetched"));
            ev.insert(QStringLiteral("ok"), ok);
            ev.insert(QStringLiteral("files"), QCborArray::fromStringList(native));
            ev.insert(QStringLiteral("error"), error);
            requester->send(ev);
        });
        return;
    }
    if (cmd == QLatin1String("getState")) {
        conn->send(stateEvent());
    } else if (cmd == QLatin1String("setPorts")) {
        // Saved for the next start of the service (the agent offers to restart it).
        const int data = int(msg.value(QStringLiteral("data")).toInteger());
        const int discovery = int(msg.value(QStringLiteral("discovery")).toInteger());
        const int phone = int(msg.value(QStringLiteral("phone")).toInteger());
        auto valid = [](int p) { return p >= 1024 && p <= 65535; };
        QString problem;
        if (!valid(data) || !valid(discovery) || !valid(phone))
            problem = tr("Ports must be between 1024 and 65535");
        else if (data == discovery || data == phone || discovery == phone)
            problem = tr("The three ports must be different");
        if (problem.isEmpty()) {
            m_settings->setValue(QStringLiteral("dataPort"), data);
            m_settings->setValue(QStringLiteral("discoveryPort"), discovery);
            m_settings->setValue(QStringLiteral("phonePort"), phone);
        } else {
            QCborMap ev = makeEvent(QStringLiteral("error"));
            ev.insert(QStringLiteral("message"), problem);
            conn->send(ev);
        }
        scheduleState();
    } else if (cmd == QLatin1String("stopService")) {
        // The user quit DeskFlower: everything stops (network, phone access, session helper)
        // until the agent starts the service again, or the next boot does.
        qInfo() << "Stopped by the user";
        QTimer::singleShot(100, QCoreApplication::instance(), &QCoreApplication::quit);
    } else if (cmd == QLatin1String("setExploring")) {
        m_node->setExploring(msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("pair")) {
        if (!m_node->requestPairing(id)) {
            QCborMap ev = makeEvent(QStringLiteral("pairResult"));
            ev.insert(QStringLiteral("id"), hexId(id));
            ev.insert(QStringLiteral("ok"), false);
            ev.insert(QStringLiteral("error"), tr("Cannot pair with that device"));
            conn->send(ev);
        }
    } else if (cmd == QLatin1String("pin")) {
        m_pendingPrompt.clear();
        m_node->submitPin(id, msg.value(QStringLiteral("pin")).toString());
    } else if (cmd == QLatin1String("cancelPair")) {
        m_node->cancelPairing(id);
    } else if (cmd == QLatin1String("unpair")) {
        m_node->unpair(id);
    } else if (cmd == QLatin1String("rename")) {
        m_node->setName(msg.value(QStringLiteral("name")).toString());
        scheduleState();
    } else if (cmd == QLatin1String("requestThumbs")) {
        m_workspace->requestRemoteThumbnails();
        sendToSession(makeEvent(QStringLiteral("captureThumbs")));
    } else if (cmd == QLatin1String("setLayout")) {
        QMap<QByteArray, QPoint> offsets;
        for (const QCborValue &v : msg.value(QStringLiteral("packs")).toArray()) {
            const QCborMap p = v.toMap();
            offsets.insert(fromHex(p.value(QStringLiteral("id"))),
                           QPoint(int(p.value(QStringLiteral("x")).toInteger()),
                                  int(p.value(QStringLiteral("y")).toInteger())));
        }
        m_workspace->setLayout(offsets);
    } else if (cmd == QLatin1String("setSharedFolder")) {
        // The service writes and serves files there: only a folder of the signed-in user, created
        // with that user's rights.
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(msg.value(QStringLiteral("path")).toString()));
        QString why;
        bool ok = platform::isAllowedSharedFolder(path, m_serviceMode, &why);
        if (ok) {
            const auto asUser = platform::actAsInteractiveUser(m_serviceMode);
            ok = asUser && QDir().mkpath(path);
        }
        if (ok) {
            m_settings->setValue(QStringLiteral("sharedFolder"), path);
        } else {
            QCborMap ev = makeEvent(QStringLiteral("sharedFolderRejected"));
            ev.insert(QStringLiteral("path"), QDir::toNativeSeparators(path));
            ev.insert(QStringLiteral("reason"), why.isEmpty() ? tr("it cannot be created") : why);
            conn->send(ev);
        }
        scheduleState();
    } else if (cmd == QLatin1String("setPhoneAccess")) {
        setPhoneAccess(msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("newPhoneLink")) {
        if (m_phone->isRunning())
            m_phoneUrls = m_phone->accessUrls(m_phone->newAccessToken());
        scheduleState();
    } else if (cmd == QLatin1String("revokePhones")) {
        m_phone->revokeSessions();
    } else if (cmd == QLatin1String("setAgentAutostart")) {
        if (platform::agentAutostartSupported(m_serviceMode)) {
            const bool on = msg.value(QStringLiteral("on")).toBool();
            m_settings->setValue(QStringLiteral("agentAutostart"), on);
            platform::setAgentAutostart(on);
            scheduleState();
        }
    } else if (cmd == QLatin1String("setClipboardSharing")) {
        m_clipboard->setEnabled(msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("setRemoteView")) {
        m_workspace->setRemoteView(id, msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("setSharing")) {
        m_workspace->setSharing(msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("addPeer")) {
        const QHostAddress addr(msg.value(QStringLiteral("host")).toString());
        if (!addr.isNull())
            m_node->addManualPeer(addr, quint16(msg.value(QStringLiteral("port")).toInteger()));
    } else if (cmd == QLatin1String("viewAnswer")) {
        // Only agents get here (the session helper is routed above), so the helper never answers.
        const QString decision = msg.value(QStringLiteral("decision")).toString();
        m_remoteView->answer(id, decision == QLatin1String("always") ? RemoteView::Decision::Always
                                 : decision == QLatin1String("once") ? RemoteView::Decision::Once
                                                                     : RemoteView::Decision::Deny);
    } else if (cmd == QLatin1String("stopScreenView")) {
        m_remoteView->stopStreaming();
    } else if (cmd == QLatin1String("update")) {
        // Only on the local user's request; the package is verified before it is installed.
        QString error;
        if (!m_updater->update(id, &error)) {
            QCborMap ev = makeEvent(QStringLiteral("updateResult"));
            ev.insert(QStringLiteral("ok"), false);
            ev.insert(QStringLiteral("message"), tr("Update failed: %1").arg(error));
            conn->send(ev);
        }
    } else if (cmd == QLatin1String("setViewAllowed")) {
        // Revoke only. The grant comes from answering the prompt, while the requester's input is
        // blocked; here the master could flip the switch itself through the shared keyboard and mouse.
        if (!msg.value(QStringLiteral("on")).toBool()) {
            m_node->setViewAllowed(id, false);
            m_remoteView->revoke(id);
        }
    }
}

void ServiceController::handleSessionCommand(const QString &cmd, const QCborMap &msg)
{
    if (cmd == QLatin1String("localScreens")) {
        QList<ScreenInfo> screens;
        for (const QCborValue &v : msg.value(QStringLiteral("screens")).toArray())
            screens.append(ScreenInfo::fromCbor(v.toMap()));
        m_workspace->setLocalScreens(screens);
    } else if (cmd == QLatin1String("localThumb")) {
        m_workspace->setLocalThumbnail(msg.value(QStringLiteral("screen")).toString(),
                                       msg.value(QStringLiteral("jpeg")).toByteArray());
    } else if (cmd == QLatin1String("vframe")) {
        m_remoteView->frameCaptured(msg);
    } else if (cmd == QLatin1String("vack")) {
        m_remoteView->frameShown(quint32(msg.value(QStringLiteral("s")).toInteger()));
    } else if (cmd == QLatin1String("vfail")) {
        m_remoteView->captureFailed(msg.value(QStringLiteral("r")).toString().left(200));
    } else if (cmd == QLatin1String("clip")) {
        if (!platform::clipboardInAgent(m_serviceMode))
            handleClipboardChange(msg);
    } else {
        m_input->handleSessionMessage(msg); // edge, mm, mb, mw, key
    }
}

void ServiceController::sendToSession(const QCborMap &event)
{
    if (m_session) {
        m_session->send(event);
        return;
    }
    const QString ev = event.value(QStringLiteral("ev")).toString();
    if (ev.startsWith(QLatin1String("view")))
        qCWarning(lcService).noquote() << "No session helper: dropped" << ev;
}

void ServiceController::updateUserPresence()
{
    // A prompt needs the helper (a signed-in session) and an agent to show it. Clients are limited
    // to the console session (isClientAllowed), and the helper leaves m_clients on hello.
    m_remoteView->setLocalUserPresent(m_session != nullptr && !m_clients.isEmpty());
}

void ServiceController::setPhoneAccess(bool on)
{
    m_settings->setValue(QStringLiteral("phoneAccess"), on);
    m_phoneError.clear();
    m_phoneUrls.clear();
    if (on) {
        if (m_phone->start(&m_phoneError))
            m_phoneUrls = m_phone->accessUrls(m_phone->newAccessToken());
        else
            qWarning().noquote() << "Phone access:" << m_phoneError;
    } else {
        m_phone->stop();
    }
    scheduleState();
}

QCborMap ServiceController::filesState() const
{
    QCborMap files;
    files.insert(QStringLiteral("folder"), QDir::toNativeSeparators(m_sharedFolder()));
    files.insert(QStringLiteral("phoneAccess"), m_settings->value(QStringLiteral("phoneAccess")).toBool());
    files.insert(QStringLiteral("phoneRunning"), m_phone->isRunning());
    files.insert(QStringLiteral("phonePort"), m_phone->port());
    files.insert(QStringLiteral("phoneSessions"), m_phone->sessionCount());
    files.insert(QStringLiteral("phoneError"), m_phoneError);
    files.insert(QStringLiteral("phoneUrls"), QCborArray::fromStringList(m_phoneUrls));
    return files;
}

QCborMap ServiceController::updatesState() const
{
    QCborMap u;
    u.insert(QStringLiteral("configured"), m_updater->isConfigured());
    u.insert(QStringLiteral("supported"), m_updater->isSupported());
    u.insert(QStringLiteral("state"), updateStateName(m_updater->state()));
    u.insert(QStringLiteral("peer"), hexId(m_updater->peer()));
    u.insert(QStringLiteral("version"), m_updater->targetVersion());
    u.insert(QStringLiteral("received"), m_updater->received());
    u.insert(QStringLiteral("total"), m_updater->total());
    return u;
}

void ServiceController::askAgent(const QString &event, QCborMap args, int timeoutMs,
                                 std::function<void(const QCborMap &)> done)
{
    if (m_clients.isEmpty()) {
        QTimer::singleShot(0, this, [done] { done({}); });
        return;
    }
    const qint64 id = ++m_agentRequestSeq;
    m_agentRequests.insert(id, std::move(done));
    QCborMap ev = makeEvent(event);
    for (auto it = args.constBegin(); it != args.constEnd(); ++it)
        ev.insert(it.key(), it.value());
    ev.insert(QStringLiteral("rid"), id);
    broadcast(ev);
    QTimer::singleShot(timeoutMs, this, [this, id] {
        if (auto late = m_agentRequests.take(id))
            late({});
    });
}

QCborMap ServiceController::clipboardConfig(bool forAgent) const
{
    QCborMap ev = makeEvent(QStringLiteral("clipConfig"));
    ev.insert(QStringLiteral("on"),
              m_clipboard->isEnabled() && forAgent == platform::clipboardInAgent(m_serviceMode));
    return ev;
}

void ServiceController::sendClipboardConfig()
{
    sendToSession(clipboardConfig(false));
    const QCborMap agents = clipboardConfig(true);
    for (ipc::Connection *c : std::as_const(m_clients))
        c->send(agents);
}

void ServiceController::sendToClipboardOwner(const QCborMap &event)
{
    if (!platform::clipboardInAgent(m_serviceMode)) {
        sendToSession(event);
        return;
    }
    for (ipc::Connection *c : std::as_const(m_clients))
        c->send(event);
}

void ServiceController::handleClipboardChange(const QCborMap &msg)
{
    ClipboardData data;
    const QCborMap formats = msg.value(QStringLiteral("f")).toMap();
    for (auto it = formats.constBegin(); it != formats.constEnd(); ++it)
        data.insert(it.key().toString(), it.value().toByteArray());
    QStringList files;
    for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
        files.append(v.toString());
    m_clipboard->localChanged(data, files);
}

void ServiceController::broadcast(const QCborMap &ev)
{
    for (ipc::Connection *c : std::as_const(m_clients))
        c->send(ev);
}

void ServiceController::scheduleState()
{
    if (!m_stateTimer.isActive())
        m_stateTimer.start();
}

QCborMap ServiceController::stateEvent() const
{
    QCborMap self;
    self.insert(QStringLiteral("id"), hexId(m_node->id()));
    self.insert(QStringLiteral("name"), m_node->name());
    self.insert(QStringLiteral("fingerprint"), m_node->fingerprint());
    self.insert(QStringLiteral("port"), m_node->dataPort());
    self.insert(QStringLiteral("version"), build::version());

    QCborArray peers;
    for (const PeerView &p : m_node->peers()) {
        if (p.otherUser)
            continue; // another user's pairing: not theirs to see or use
        QCborMap m;
        m.insert(QStringLiteral("id"), hexId(p.id));
        m.insert(QStringLiteral("name"), p.name);
        m.insert(QStringLiteral("os"), p.os);
        m.insert(QStringLiteral("fingerprint"), p.fingerprint);
        m.insert(QStringLiteral("address"), p.address.isNull() ? QString() : p.address.toString());
        m.insert(QStringLiteral("port"), p.port);
        m.insert(QStringLiteral("trusted"), p.trusted);
        m.insert(QStringLiteral("connected"), p.connected);
        m.insert(QStringLiteral("exploring"), p.exploring);
        m.insert(QStringLiteral("allowView"), p.allowView);
        m.insert(QStringLiteral("version"), p.version);
        m.insert(QStringLiteral("update"), m_updater->offeredUpdate(p)); // newer version it can send us
        peers.append(m);
    }

    QCborMap ev = makeEvent(QStringLiteral("state"));
    ev.insert(QStringLiteral("self"), self);
    ev.insert(QStringLiteral("exploring"), m_node->isExploring());
    ev.insert(QStringLiteral("clipboard"), m_clipboard->isEnabled());
    ev.insert(QStringLiteral("autostartSupported"), platform::agentAutostartSupported(m_serviceMode));
    ev.insert(QStringLiteral("autostart"), m_settings->value(QStringLiteral("agentAutostart"), true).toBool());
    ev.insert(QStringLiteral("files"), filesState());
    ev.insert(QStringLiteral("updates"), updatesState());
    // Configured (used from the next start) and in use now.
    QCborMap ports;
    ports.insert(QStringLiteral("data"), m_settings->value(QStringLiteral("dataPort"), wire::kDefaultDataPort).toInt());
    ports.insert(QStringLiteral("discovery"),
                 m_settings->value(QStringLiteral("discoveryPort"), wire::kDiscoveryPort).toInt());
    ports.insert(QStringLiteral("phone"),
                 m_settings->value(QStringLiteral("phonePort"), web::PhoneServer::kDefaultPort).toInt());
    ports.insert(QStringLiteral("dataInUse"), m_node->dataPort());
    ports.insert(QStringLiteral("tcpInUse"), m_node->tcpPort());
    ports.insert(QStringLiteral("discoveryInUse"), m_node->discoveryPort());
    ports.insert(QStringLiteral("phoneInUse"), m_phone->isRunning() ? m_phone->port() : 0);
    ev.insert(QStringLiteral("ports"), ports);
    ev.insert(QStringLiteral("peers"), peers);
    ev.insert(QStringLiteral("workspace"), workspaceState());
    if (m_remoteView->isStreaming()) {
        const QByteArray viewer = m_remoteView->streamingTo();
        QCborMap v;
        v.insert(QStringLiteral("id"), hexId(viewer));
        for (const PeerView &p : m_node->peers()) {
            if (p.id == viewer)
                v.insert(QStringLiteral("name"), p.name);
        }
        ev.insert(QStringLiteral("screenViewer"), v);
    }
    return ev;
}

QCborMap ServiceController::workspaceState() const
{
    QHash<QByteArray, PeerView> peers;
    for (const PeerView &p : m_node->peers())
        peers.insert(p.id, p);

    const QMap<QByteArray, QPoint> layout = m_workspace->effectiveLayout();
    QCborArray devices;
    for (const QByteArray &id : m_workspace->devices()) {
        const bool self = id == m_node->id();
        QCborArray screens;
        for (const ScreenInfo &s : m_workspace->screens(id))
            screens.append(s.toCbor());
        QCborMap d;
        d.insert(QStringLiteral("id"), hexId(id));
        d.insert(QStringLiteral("name"), self ? m_node->name() : peers.value(id).name);
        d.insert(QStringLiteral("self"), self);
        d.insert(QStringLiteral("connected"), self || peers.value(id).connected);
        d.insert(QStringLiteral("remoteView"), m_workspace->isRemoteView(id));
        d.insert(QStringLiteral("thumbsDenied"), m_workspace->thumbnailsDenied(id));
        d.insert(QStringLiteral("x"), layout.value(id).x());
        d.insert(QStringLiteral("y"), layout.value(id).y());
        d.insert(QStringLiteral("screens"), screens);
        devices.append(d);
    }
    QCborMap ws;
    ws.insert(QStringLiteral("master"), hexId(m_workspace->masterId()));
    ws.insert(QStringLiteral("active"), hexId(m_input->activeDevice()));
    ws.insert(QStringLiteral("session"), m_session != nullptr);
    ws.insert(QStringLiteral("devices"), devices);
    return ws;
}

QCborMap ServiceController::thumbEvent(const QByteArray &deviceId, const QString &screenId,
                                       const QByteArray &jpeg) const
{
    QCborMap ev = makeEvent(QStringLiteral("thumb"));
    ev.insert(QStringLiteral("device"), hexId(deviceId));
    ev.insert(QStringLiteral("screen"), screenId);
    ev.insert(QStringLiteral("jpeg"), jpeg);
    return ev;
}

} // namespace df
