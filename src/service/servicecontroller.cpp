#include "servicecontroller.h"

#include "buildinfo.h"
#include "crypto.h"
#include "platform/platform.h"

#include <QCborArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <QVersionNumber>
#include <utility>

Q_LOGGING_CATEGORY(lcService, "deskflower.service")

namespace df {

namespace {
QString hexId(const QByteArray &id) { return QString::fromLatin1(id.toHex()); }

// The level in a pair/setTrust/setPairTrust command: "trust" ("guest"/"device"/"group").
TrustLevel trustArgument(const QCborMap &msg)
{
    return trustFromName(msg.value(QStringLiteral("trust")).toString());
}

// A level as the agent names it (TrustLevels.qml), for notices.
QString trustText(TrustLevel level)
{
    switch (level) {
    case TrustLevel::Guest: return ServiceController::tr("Guest");
    case TrustLevel::Group: return ServiceController::tr("All my devices");
    case TrustLevel::Device: break;
    }
    return ServiceController::tr("This device only");
}
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
      m_transfer(transfer),
      m_settings(settings),
      m_phone(phone),
      m_remoteView(remoteView),
      m_updater(updater),
      m_sharedFolder(std::move(sharedFolder)),
      m_receivedFolder(std::move(receivedFolder)),
      m_sessionToken(sessionToken),
      m_instance(instance),
      m_serviceMode(serviceMode),
      m_agentWatch([] { platform::relaunchAgent(); })
{
    m_input->setSwapCtrlCmd(swapCtrlCmdDevices());

#ifdef DESKFLOWER_KEY_DIAGNOSTICS
    // Keyboard diagnostics: the helper records while it runs, the agents show the lines.
    m_keyDiag = std::make_unique<KeyDiagnostics>(m_node);
    m_input->setKeyTrace([this](const QByteArray &peer, quint32 scancode, bool down, bool sent) {
        m_keyDiag->noteKey(peer, scancode, down, sent);
    });
    m_input->setIgnoredKeyTrace([this](const QByteArray &peer, quint32 scancode, bool down, const QString &why) {
        m_keyDiag->noteIgnoredKey(peer, scancode, down, why);
    });
    connect(m_keyDiag.get(), &KeyDiagnostics::activeChanged, this, [this](bool on) {
        QCborMap ev = makeEvent(QStringLiteral("keyDiag"));
        ev.insert(QStringLiteral("on"), on);
        sendToSession(ev);
        if (on) { // a new run: the agents drop the previous run's lines before its first ones
            QCborMap lines = makeEvent(QStringLiteral("keyDiagLines"));
            lines.insert(QStringLiteral("lines"), QCborArray::fromStringList(m_keyDiag->lines()));
            lines.insert(QStringLiteral("h"), m_keyDiag->header().size()); // the first h: the header
            broadcast(lines);
        }
        scheduleState();
    });
    connect(m_keyDiag.get(), &KeyDiagnostics::lineAdded, this, [this](const QString &line, bool header) {
        QCborMap ev = makeEvent(QStringLiteral("keyDiagLine"));
        ev.insert(QStringLiteral("l"), line);
        ev.insert(QStringLiteral("h"), header); // never dropped by the agent
        broadcast(ev);
    });
#endif

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
    agent.hover = [this](const FileDrag::Hover &hover, std::function<void(const FileDrag::HoverEnd &)> done) {
        QCborMap a;
        a.insert(QStringLiteral("o"), qint64(hover.offer));
        a.insert(QStringLiteral("own"), hover.own);
        a.insert(QStringLiteral("paths"), QCborArray::fromStringList(hover.paths));
        a.insert(QStringLiteral("files"), listedToCbor(hover.files)); // a folder: [path, -1]
        a.insert(QStringLiteral("n"), hover.count);
        a.insert(QStringLiteral("local"), hover.local);
        if (m_dragClients.isEmpty()) {
            // No agent that runs drags: none can be shown here.
            QTimer::singleShot(0, this, [done] {
                FileDrag::HoverEnd end;
                end.unavailable = true;
                done(end);
            });
            return;
        }
        // While the drag runs the agent hears from the service regularly: a drag that stops
        // hearing (the service stuck or gone) ends there instead of staying on screen.
        auto *alive = new QTimer(this);
        alive->setInterval(kDragAliveMs);
        connect(alive, &QTimer::timeout, this, [this, offer = hover.offer] {
            QCborMap ev = makeEvent(QStringLiteral("dragAlive"));
            ev.insert(QStringLiteral("o"), qint64(offer));
            broadcast(ev);
        });
        alive->start();
        // The agent may rely on it (and give up a drag that stops hearing it).
        a.insert(QStringLiteral("alive"), true);
        // Answered when the drag there ends (dropped, into a folder, or cancelled). No answer
        // (the agent went away, or the timeout): the drag there is over and took nothing.
        askAgent(QStringLiteral("dragHover"), a, 60 * 60 * 1000, [done, alive](const QCborMap &r) {
            alive->stop();
            alive->deleteLater();
            FileDrag::HoverEnd end;
            end.lost = r.isEmpty();
            end.unavailable = r.value(QStringLiteral("unavailable")).toBool();
            end.dropped = r.value(QStringLiteral("dropped")).toBool();
            end.confirmed = r.value(QStringLiteral("confirmed")).toBool();
            end.folder = r.value(QStringLiteral("folder")).toString();
            done(end);
        }, m_dragClients.values());
    };
    agent.badge = [this](const QString &text) {
        QCborMap ev = makeEvent(QStringLiteral("dragBadge"));
        ev.insert(QStringLiteral("text"), text);
        broadcast(ev);
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
    m_fileDrag->setNativeDrag(false); // until an agent that runs drags connects (see "agent")
    connect(m_fileDrag.get(), &FileDrag::notice, this, &ServiceController::sendNotice);

    // Transfers the user sees (pasted files, dragged files): progress for the agents, which show it
    // and may cancel (cancelTransfer). FileTransfer already limits the rate of progress reports.
    // target: the folder the files go into (an agent recognises the transfer of a paste Explorer
    // shows itself, and opens it once done); cancellable: cancelTransfer would stop it now.
    auto transferEvent = [this](quint64 handle, const QByteArray &peer, const QString &kind, const QString &state,
                                const QString &target) {
        QCborMap ev = makeEvent(QStringLiteral("transfer"));
        ev.insert(QStringLiteral("id"), qint64(handle));
        ev.insert(QStringLiteral("peer"), hexId(peer));
        ev.insert(QStringLiteral("peerName"), peerName(peer));
        ev.insert(QStringLiteral("kind"), kind);
        ev.insert(QStringLiteral("state"), state);
        ev.insert(QStringLiteral("target"), QDir::toNativeSeparators(target));
        ev.insert(QStringLiteral("cancellable"),
                  state == QLatin1String("running") && m_transfer->isVisibleFetch(handle));
        return ev;
    };
    connect(m_transfer, &FileTransfer::transferProgress, this,
            [this, transferEvent](quint64 handle, const QByteArray &peer, const QString &kind, qint64 done,
                                  qint64 total, int files, int fileCount, const QString &target) {
                QCborMap ev = transferEvent(handle, peer, kind, QStringLiteral("running"), target);
                ev.insert(QStringLiteral("done"), done);
                ev.insert(QStringLiteral("total"), total);
                ev.insert(QStringLiteral("files"), files);
                ev.insert(QStringLiteral("fileCount"), fileCount);
                broadcast(ev);
            });
    connect(m_transfer, &FileTransfer::transferFinished, this,
            [this, transferEvent](quint64 handle, const QByteArray &peer, const QString &kind, const QString &state,
                                  const QString &error, const QString &target) {
                QCborMap ev = transferEvent(handle, peer, kind, state, target);
                ev.insert(QStringLiteral("error"), error);
                broadcast(ev);
            });
    // A paste of copied files refused for want of a gesture (once per clipboard): the agents offer
    // to take them anyway (Receive: clipReceive). The offer ends with the clipboard, or once the
    // files are here (count 0).
    connect(m_clipboard, &ClipboardSync::filesWaiting, this,
            [this](const QByteArray &kid, int count, const QString &from) {
                QCborMap ev = makeEvent(QStringLiteral("clipWaiting"));
                ev.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
                ev.insert(QStringLiteral("count"), count);
                ev.insert(QStringLiteral("from"), from);
                m_clipWaiting = count > 0 ? ev : QCborMap();
                broadcast(ev);
            });

    connectTrustNotices();

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
    // A paste of copied files waiting for its paste gesture (the helper's localPress, a key or a
    // button from the master) is checked again as soon as one comes.
    connect(m_input, &InputController::pasteGesture, m_clipboard, &ClipboardSync::pasteGestureNoted);
    connect(m_clipboard, &ClipboardSync::applyLocally, this,
            [this](const QByteArray &kid, const QVariantMap &sealed, const QStringList &files, int pendingFiles,
                   bool gated) {
                // No plaintext leaves the service ahead of a paste: the owner gets the id and the
                // list of formats (with their plaintext sizes) and renders each one lazily through
                // clipUnseal. Copied files are not transferred here either: the owner fetches them
                // (clipFetch) when an application reads them, i.e. at the paste. gated: a read
                // needs a paste grant (a guest's clipboard), so the owner refuses reads that do not
                // come from a paste without asking; otherwise it serves (and caches) every read.
                QCborArray sealedList;
                for (auto it = sealed.constBegin(); it != sealed.constEnd(); ++it)
                    sealedList.append(QCborArray{it.key(), it.value().toLongLong()});
                QCborMap ev = makeEvent(QStringLiteral("clipSet"));
                ev.insert(QStringLiteral("kid"), kid);
                ev.insert(QStringLiteral("sealed"), sealedList);
                ev.insert(QStringLiteral("files"), QCborArray::fromStringList(files));
                ev.insert(QStringLiteral("pendingFiles"), pendingFiles);
                ev.insert(QStringLiteral("gated"), gated);
                // pending counts files and folders; the list has a folder as [path, -1].
                if (pendingFiles > 0)
                    ev.insert(QStringLiteral("pendingList"), listedToCbor(m_clipboard->pendingFileList()));
                // stream (0.1.30): the files may be pasted while they arrive (a streaming clipFetch,
                // listed with clipList when pendingList does not hold them). An older owner ignores it.
                if (pendingFiles > 0 && m_clipboard->filesStreamable())
                    ev.insert(QStringLiteral("stream"), true);
                // move (0.1.31): the owner may paste these files by moving them out of the folder
                // they are transferred into (a streaming clipFetch with fresh, then clipConsumed),
                // when the user left that on (setting "pasteMove", off by default since 0.1.32). pendingBytes:
                // for the owner's choice of it (large pastes only).
                if (pendingFiles > 0) {
                    ev.insert(QStringLiteral("pendingBytes"), m_clipboard->pendingBytes());
                    if (m_clipboard->filesMovable() && m_settings->value(QStringLiteral("pasteMove"), false).toBool())
                        ev.insert(QStringLiteral("move"), true);
                }
                sendToClipboardOwner(ev);
            });
    // Remote view: viewer side (we are the master) and source side (our screen is shown elsewhere).
    connect(m_input, &InputController::viewRequested, this,
            [this](const QByteArray &device, const QList<ViewScreen> &shown) {
                m_remoteView->startViewing(device, shown);
            });
    connect(m_input, &InputController::viewScreensChanged, this,
            [this](const QByteArray &, const QList<ViewScreen> &shown) { m_remoteView->setScreens(shown); });
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
    connect(m_remoteView, &RemoteView::captureRequested, this, [this](const QList<CaptureSpec> &screens) {
        QCborMap ev = makeEvent(QStringLiteral("viewCapture"));
        ev.insert(QStringLiteral("on"), true);
        QCborArray list;
        for (const CaptureSpec &s : screens) {
            QCborMap m;
            m.insert(QStringLiteral("i"), s.index);
            m.insert(QStringLiteral("g"), qint64(s.generation));
            m.insert(QStringLiteral("sx"), s.screen.x());
            m.insert(QStringLiteral("sy"), s.screen.y());
            m.insert(QStringLiteral("sw"), s.screen.width());
            m.insert(QStringLiteral("sh"), s.screen.height());
            m.insert(QStringLiteral("w"), s.maxSize.width());
            m.insert(QStringLiteral("h"), s.maxSize.height());
            list.append(m);
        }
        ev.insert(QStringLiteral("screens"), list);
        sendToSession(ev);
    });
    connect(m_remoteView, &RemoteView::captureNext, this, [this](int index) {
        QCborMap ev = makeEvent(QStringLiteral("viewNext"));
        ev.insert(QStringLiteral("i"), index);
        sendToSession(ev);
    });
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
    connect(m_updater, &Updater::aboutToInstall, this, [this] {
        // The agents quit so that the installer can replace their files: none is started again
        // meanwhile (the new service starts the new agent).
        m_agentWatchTimer.stop();
        broadcast(makeEvent(QStringLiteral("quit")));
    });
    connect(m_updater, &Updater::finished, this, [this](const QByteArray &, bool ok, const QString &error) {
        QCborMap ev = makeEvent(QStringLiteral("updateResult"));
        ev.insert(QStringLiteral("ok"), ok);
        ev.insert(QStringLiteral("message"), ok ? tr("Installing the update. DeskFlower restarts when it is done.")
                                                : tr("Update failed: %1").arg(error));
        broadcast(ev);
        scheduleState();
        if (!ok && platform::clipboardInAgent(m_serviceMode))
            m_agentWatchTimer.start(); // no installer runs (any more): watch the agent again
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
    m_node->setRemoteViewEnabled(m_settings->value(QStringLiteral("remoteView"), true).toBool());
    // --install (an upgrade too) writes the rule for the local subnet only: the user's choice is
    // applied again.
    if (platform::firewallScopeSupported(m_serviceMode) && m_settings->value(QStringLiteral("otherSubnets")).toBool())
        platform::setFirewallOtherSubnets(true);
    connect(m_node, &Node::incomingBlocked, this, [this](const QByteArray &peer, const QHostAddress &, quint16 port) {
        // A paired device on another network could not connect here (it fell back to a slower
        // path): say what to change, now and then.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_incomingBlockedNoticed.value(peer, 0) < 10 * 60 * 1000)
            return;
        m_incomingBlockedNoticed.insert(peer, now);
        const bool firewallHere = platform::firewallScopeSupported(m_serviceMode)
                                  && !m_settings->value(QStringLiteral("otherSubnets")).toBool();
        sendNotice(firewallHere
                       ? tr("%1, on another network, cannot connect to this computer: Windows Firewall only lets in "
                            "the local subnet on public networks. Files still arrive, more slowly. To let it "
                            "connect, turn on \"Allow devices on other networks\" in Advanced settings.")
                             .arg(peerName(peer))
                       : tr("%1, on another network, cannot connect to this computer on TCP port %2 (a firewall "
                            "here may refuse it). Files still arrive, more slowly.")
                             .arg(peerName(peer))
                             .arg(port),
                   true);
    });

    connect(m_clipboard, &ClipboardSync::filesNotice, this, &ServiceController::sendNotice);
    connect(m_clipboard, &ClipboardSync::notice, this, &ServiceController::sendNotice);
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
                // The dialog offers what the pairing will apply unless the user picks another
                // level (Node::setPairTrust): the requester's suggestion, never less than the
                // level the device has already, a guest staying a guest, and Group only once
                // picked here. A Group suggestion not taken up yet comes along so that the dialog
                // can point at it.
                m_pendingPin.insert(QStringLiteral("trust"), trustName(m_node->pairingTrust(id)));
                if (const auto suggested = m_node->pairingSuggestion(id))
                    m_pendingPin.insert(QStringLiteral("suggested"), trustName(*suggested));
                broadcast(m_pendingPin);
            });
    connect(m_node, &Node::pairingPinRequired, this, [this](const QByteArray &id, const QString &name) {
        m_pendingPrompt = makeEvent(QStringLiteral("pairPrompt"));
        m_pendingPrompt.insert(QStringLiteral("id"), hexId(id));
        m_pendingPrompt.insert(QStringLiteral("name"), name);
        // The level chosen with the request, so that an agent started while the prompt is
        // pending states the right one.
        m_pendingPrompt.insert(QStringLiteral("trust"), trustName(m_node->pairingTrust(id)));
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

    // IPC clients are checked at connect; a fast user switch would otherwise leave the previous
    // user's agent receiving the new user's clipboard, PINs and thumbnails (and able to send
    // commands). Re-check on every session change and on a 2 s watch (notifications can be missed).
    if (m_serviceMode) {
        m_consoleUser = platform::interactiveUserId(m_serviceMode);
        connect(platform::SessionEvents::instance(), &platform::SessionEvents::sessionChanged, this,
                [this] { onConsoleSessionChanged(); });
        m_sessionWatch.setInterval(2000);
        connect(&m_sessionWatch, &QTimer::timeout, this, &ServiceController::onConsoleSessionChanged);
        m_sessionWatch.start();
    }
    // Without the agent, the clipboard of a Windows service is neither shared nor pasted (the agent
    // owns it): one that went away is started again.
    if (platform::clipboardInAgent(m_serviceMode)) {
        m_agentWatch.setAutostart(m_settings->value(QStringLiteral("agentAutostart"), true).toBool());
        m_agentWatchTimer.setInterval(AgentWatch::kCheckIntervalMs);
        connect(&m_agentWatchTimer, &QTimer::timeout, this, [this] { m_agentWatch.check(); });
        m_agentWatchTimer.start();
    }
}

void ServiceController::updateAgentWatch()
{
    // The session helper also runs on the sign-in screen: only a signed-in user gets an agent.
    m_agentWatch.setUserPresent(m_session != nullptr && !m_consoleUser.isEmpty());
    m_agentWatch.setAgentConnected(!m_sealingClients.isEmpty());
}

void ServiceController::onConsoleSessionChanged()
{
    if (!m_serviceMode)
        return;
    const quint32 console = platform::activeConsoleSession();
    // Drop every client that no longer belongs to the active console session. Their disconnected
    // handler removes them from m_clients/m_clientSession and updates presence.
    for (ipc::Connection *conn : QList<ipc::Connection *>(m_clients)) {
        const quint32 session = m_clientSession.value(conn, platform::kNoSession);
        if (session != platform::kNoSession && console != platform::kNoSession && session != console) {
            qCInfo(lcService) << "Dropping an IPC client from session" << session
                              << "(active console session is" << console << ")";
            conn->socket()->abort();
        }
    }
    // The session helper belongs to the session it was started in. Once that is no longer the
    // console, its pipe is closed: it puts the system cursors back, releases injected keys and
    // exits, and SessionLauncher starts one in the new session (terminating it only if it stays).
    if (m_session) {
        const quint32 session = m_clientSession.value(m_session, platform::kNoSession);
        if (session != platform::kNoSession && console != platform::kNoSession && session != console) {
            qCInfo(lcService) << "Closing the session helper of session" << session;
            m_session->socket()->disconnectFromServer();
        }
    }

    // When the console user actually changed, nothing of the previous user's may stay: clear the
    // clipboard (it often holds passwords), the cached screen thumbnails and the pending prompts,
    // and revoke phone sessions bound to the previous user.
    const QString user = platform::interactiveUserId(m_serviceMode);
    for (ipc::Connection *conn : QList<ipc::Connection *>(m_renderClients.values())) {
        const quint32 session = platform::clientSessionId(conn->socket(), m_serviceMode);
        if (session != platform::kNoSession && console != platform::kNoSession && session != console)
            conn->socket()->abort();
    }
    if (user != m_consoleUser) {
        m_consoleUser = user;
        m_agentWatch.userChanged();
        updateAgentWatch();
        m_clipboard->reset();
        m_workspace->forgetThumbnails();
        m_phone->revokeSessions();
        m_pendingPin.clear();
        m_pendingPrompt.clear();
        m_pendingViewPrompt.clear();
        scheduleState();
    }
}

ServiceController::~ServiceController()
{
    // The clients' sockets belong to the servers (the render server's too), which go after the
    // members their disconnected handlers use: those handlers must not run while this is destroyed.
    for (QLocalSocket *socket : m_server.findChildren<QLocalSocket *>())
        socket->disconnect(this);
    for (QLocalSocket *socket : m_renderServer.findChildren<QLocalSocket *>())
        socket->disconnect(this);
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
    m_input->setKeyTrace({}); // the input controller outlives m_keyDiag
    m_input->setIgnoredKeyTrace({});
#endif
    // The clipboard outlives this: the requests of the connections going with it go too.
    const QHash<ipc::Connection *, QList<quint64>> waiters = std::exchange(m_filesWaiters, {});
    for (auto it = waiters.cbegin(); it != waiters.cend(); ++it) {
        for (const quint64 id : it.value())
            m_clipboard->dropFilesWaiter(id);
    }
}

void ServiceController::dropClipboardWaiters(ipc::Connection *conn)
{
    for (const quint64 id : m_filesWaiters.take(conn))
        m_clipboard->dropFilesWaiter(id);
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
    // Pastes have a server of their own (see ipc::renderServerName). Without it they still work,
    // over the main one.
    const QString renderName = ipc::renderServerName(m_instance);
    QLocalServer::removeServer(renderName);
    m_renderServer.setSocketOptions(m_server.socketOptions());
    if (m_renderServer.listen(renderName))
        connect(&m_renderServer, &QLocalServer::newConnection, this, &ServiceController::onNewRenderConnection);
    else
        qCWarning(lcService) << "Cannot listen on" << renderName << ":" << m_renderServer.errorString();
    return true;
}

void ServiceController::onNewRenderConnection()
{
    while (QLocalSocket *socket = m_renderServer.nextPendingConnection()) {
        if (!platform::isClientAllowed(socket, m_serviceMode)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto *conn = new ipc::Connection(socket, this);
        m_renderClients.insert(conn);
        connect(conn, &ipc::Connection::messageReceived, this, [this, conn](const QCborMap &msg) {
            if (!admitFromConsole(conn))
                return;
            if (!handleClipboardRequest(conn, msg.value(QStringLiteral("cmd")).toString(), msg))
                conn->socket()->abort(); // nothing else is served here
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, conn, socket] {
            m_renderClients.remove(conn);
            dropClipboardWaiters(conn);
            conn->deleteLater();
            socket->deleteLater();
        });
    }
}

bool ServiceController::admitFromConsole(ipc::Connection *conn)
{
    // Something has been read from the pipe now: the client's own token says which session it is
    // in (robust against process id reuse), and a client outside the console goes.
    const quint32 session = platform::clientSessionId(conn->socket(), m_serviceMode);
    const quint32 console = platform::activeConsoleSession();
    if (session != platform::kNoSession && console != platform::kNoSession && session != console) {
        qCWarning(lcService) << "Rejected an IPC client from session" << session << "(active console session is"
                             << console << ")";
        conn->socket()->abort();
        return false;
    }
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
        // The console session this client connected from (its process id for now: the pipe lets
        // the client be impersonated only once something was read from it, so the client's own
        // token decides at its first message), re-checked on every session change.
        m_clientSession.insert(conn, platform::clientSessionId(socket, m_serviceMode));
        m_sessionUndecided.insert(conn);
        connect(conn, &ipc::Connection::messageReceived, this,
                [this, conn](const QCborMap &msg) { handleCommand(conn, msg); });
        connect(socket, &QLocalSocket::disconnected, this, [this, conn, socket] {
            m_clients.removeAll(conn);
            m_clientSession.remove(conn);
            m_sessionUndecided.remove(conn);
            m_sealingClients.remove(conn);
            if (conn == m_envClient)
                m_envClient = nullptr;
            if (m_dragClients.remove(conn))
                m_fileDrag->setNativeDrag(!m_dragClients.isEmpty());
            // Requests that only it could still answer are answered empty now rather than at
            // their timeout (a drag shown by an agent that crashed ends at once).
            m_agentRequests.disconnected(conn);
            dropClipboardWaiters(conn);
            if (conn == m_session) {
                m_session = nullptr;
                m_input->sessionDisconnected();
                if (!m_inputProblem.isEmpty()) { // the next helper reports its own
                    m_inputProblem.clear();
                    m_inputProblemLink.clear();
                    scheduleState();
                }
            }
            updateUserPresence();
            updateAgentWatch();
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

bool ServiceController::handleClipboardRequest(ipc::Connection *conn, const QString &cmd, const QCborMap &msg)
{
    if (cmd == QLatin1String("clipUnseal")) {
        // The owner renders a format of the sealed clipboard (a paste): decrypt it now and answer.
        // The key never leaves the service; the owner gets the plaintext of that one format only
        // (a guest's, only after the master granted the paste).
        const QPointer<ipc::Connection> requester(conn);
        const QByteArray kid = fromHex(msg.value(QStringLiteral("kid")));
        const QString mime = msg.value(QStringLiteral("mime")).toString();
        m_clipboard->unseal(kid, mime, [requester, kid, mime](bool ok, const QByteArray &plain, const QString &error) {
            if (!requester)
                return;
            QCborMap ev = makeEvent(QStringLiteral("clipUnsealed"));
            ev.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
            ev.insert(QStringLiteral("mime"), mime);
            ev.insert(QStringLiteral("ok"), ok);
            ev.insert(QStringLiteral("data"), plain);
            ev.insert(QStringLiteral("error"), error);
            requester->send(ev);
        });
        return true;
    }
    if (cmd == QLatin1String("clipList")) {
        // A file manager reads what a paste will bring (Explorer's descriptor): names and sizes
        // only, from the announcement or a list-only fetch made once per clipboard. No paste is
        // needed, no contents move (ClipboardSync::listFiles). Answered in clipListed messages of
        // at most kListChunkItems items, `more` on all but the last.
        const QPointer<ipc::Connection> requester(conn);
        const QByteArray kid = fromHex(msg.value(QStringLiteral("kid")));
        m_clipboard->listFiles(kid, [requester, kid](bool ok, const QList<ListedFile> &items, const QString &error) {
            if (!requester)
                return;
            qsizetype at = 0;
            do {
                QCborMap ev = makeEvent(QStringLiteral("clipListed"));
                ev.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
                ev.insert(QStringLiteral("ok"), ok);
                if (!ok)
                    ev.insert(QStringLiteral("error"), error);
                ev.insert(QStringLiteral("items"), listedToCbor(items.mid(at, kListChunkItems)));
                at += kListChunkItems;
                ev.insert(QStringLiteral("more"), at < items.size());
                requester->send(ev);
            } while (at < items.size());
        });
        return true;
    }
    if (cmd == QLatin1String("clipFetch")) {
        // The user pastes files copied on another device: transfer them now (a transfer the user
        // sees, with its progress), then answer. kid (0.1.30 owners): the clipboard object asking,
        // refused when it is not the current one. Only for a paste (ClipboardSync::fetchFiles): a
        // refusal for want of one says so (code "noPaste"), and the owner gives the reader nothing
        // without telling anyone.
        //
        // stream (0.1.30, a paste that copies the files while they arrive): clipStarted {root}
        // as soon as the transfer's folder is known (the files appear there, each growing to its
        // size), then clipFetched as usual. root may not exist yet when clipStarted comes: the
        // transfer creates it shortly after; the owner waits for it and never creates it itself
        // (a transfer that fails first answers with clipFetched {ok:false} instead). Such a request
        // belongs to its connection: closing it drops the request (the transfer is cancelled when
        // nobody else waits for it), and until then the folder stays pinned, also after the
        // transfer ended (never pruned while read). listedAgo (ms, a streaming owner): how long ago
        // its reader read the list of the files, whose paste may read the first contents long
        // after (see ClipboardSync::fetchFiles).
        //
        // fresh (0.1.31, with stream: the owner's move paste, told it may with clipSet {move}):
        // the paste moves the files out of the folder it is given, so files transferred here before
        // are transferred again into a new folder (ClipboardSync::fetchFiles). Once the paste moved
        // them, the owner sends clipConsumed on the same connection before closing it.
        const QPointer<ipc::Connection> requester(conn);
        const QPointer<ServiceController> self(this);
        const QByteArray kid = fromHex(msg.value(QStringLiteral("kid")));
        const bool streaming = msg.value(QStringLiteral("stream")).toBool();
        const qint64 listedAgo = streaming ? msg.value(QStringLiteral("listedAgo")).toInteger(-1) : -1;
        const bool fresh = streaming && msg.value(QStringLiteral("fresh")).toBool();
        ClipboardSync::StartedCallback started;
        if (streaming) {
            started = [requester](const QString &root) {
                if (!requester)
                    return;
                QCborMap ev = makeEvent(QStringLiteral("clipStarted"));
                ev.insert(QStringLiteral("root"), QDir::toNativeSeparators(root));
                requester->send(ev);
            };
        }
        auto waiter = std::make_shared<quint64>(0);
        // A large transfer takes minutes: the waiting agent is told it is still going, and gives
        // up only when nothing comes for a while (Backend::fetchFromService).
        auto *keepAlive = new QTimer(conn);
        connect(keepAlive, &QTimer::timeout, conn, [requester] {
            if (requester)
                requester->send(makeEvent(QStringLiteral("clipFetching")));
        });
        keepAlive->start(10000);
        const QPointer<QTimer> keepAliveGuard(keepAlive);
        *waiter = m_clipboard->fetchFiles(
            [requester, self, keepAliveGuard, waiter, streaming](bool ok, const QStringList &files,
                                                                 const QString &error, ClipboardSync::FetchError code) {
                if (keepAliveGuard)
                    keepAliveGuard->deleteLater();
                if (!requester)
                    return;
                // A plain request is over; a streaming one stays until its connection closes.
                if (self && !streaming && *waiter) {
                    auto it = self->m_filesWaiters.find(requester.data());
                    if (it != self->m_filesWaiters.end()) {
                        it->removeAll(*waiter);
                        if (it->isEmpty())
                            self->m_filesWaiters.erase(it);
                    }
                }
                QStringList native;
                for (const QString &f : files)
                    native << QDir::toNativeSeparators(f);
                QCborMap ev = makeEvent(QStringLiteral("clipFetched"));
                ev.insert(QStringLiteral("ok"), ok);
                ev.insert(QStringLiteral("files"), QCborArray::fromStringList(native));
                ev.insert(QStringLiteral("error"), error);
                if (const QString name = ClipboardSync::fetchErrorName(code); !name.isEmpty())
                    ev.insert(QStringLiteral("code"), name);
                requester->send(ev);
            },
            kid, std::move(started), streaming, listedAgo, fresh);
        if (*waiter)
            m_filesWaiters[conn].append(*waiter);
        return true;
    }
    if (cmd == QLatin1String("clipConsumed")) {
        // A move paste took the files of this connection's streaming request out of their folder
        // (ClipboardSync::filesConsumed): what is left there goes, and the next paste transfers
        // again. Only this connection's own requests: the owner names no folder.
        for (const quint64 id : m_filesWaiters.take(conn))
            m_clipboard->filesConsumed(id);
        return true;
    }
    return false;
}

void ServiceController::handleCommand(ipc::Connection *conn, const QCborMap &msg)
{
    const QString cmd = msg.value(QStringLiteral("cmd")).toString();
    const QByteArray id = fromHex(msg.value(QStringLiteral("id")));

    if (m_sessionUndecided.remove(conn)) {
        // Something has been read from the pipe now: the client's own token says which session
        // it is in (robust against process id reuse), and a client outside the console goes.
        const quint32 session = platform::clientSessionId(conn->socket(), m_serviceMode);
        m_clientSession.insert(conn, session);
        const quint32 console = platform::activeConsoleSession();
        if (session != platform::kNoSession && console != platform::kNoSession && session != console) {
            qCWarning(lcService) << "Rejected an IPC client from session" << session
                                 << "(active console session is" << console << ")";
            conn->socket()->abort();
            return;
        }
    }
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
            m_clients.removeAll(conn); // its session stays in m_clientSession (onConsoleSessionChanged)
            m_session = conn;
            m_input->sessionConnected();
            sendClipboardConfig();
            // A new helper may belong to another user session: a live view does not carry over.
            if (m_remoteView->isStreaming())
                m_remoteView->setLocalUserPresent(false);
            updateUserPresence();
            updateAgentWatch();
            // Where the helper owns the clipboard: what came from another device is put back on a
            // restarted helper (see the agent's case below).
            if (!platform::clipboardInAgent(m_serviceMode) && !m_clipboard->reapply())
                sendToSession(makeEvent(QStringLiteral("clipClear")));
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
            if (m_keyDiag->isActive()) { // a helper restarted during a diagnostic records too
                QCborMap ev = makeEvent(QStringLiteral("keyDiag"));
                ev.insert(QStringLiteral("on"), true);
                sendToSession(ev);
            }
#endif
        } else {
            conn->socket()->abort();
        }
        return;
    }

    if (cmd == QLatin1String("agent")) {
        // The agent's version: a sealed clipboard goes only to agents that render it (an older
        // agent still running across an update would put nothing on the clipboard instead).
        const QVersionNumber v = QVersionNumber::fromString(msg.value(QStringLiteral("v")).toString());
        if (!v.isNull() && v >= QVersionNumber::fromString(QLatin1String(ClipboardSync::kSealingVersion)))
            m_sealingClients.insert(conn);
        // Whether it runs native drags of files from other devices: peers are told (FileDrag),
        // and show nothing here without one (dropped files then go into Received).
        if (msg.value(QStringLiteral("dnd")).toBool()) {
            m_dragClients.insert(conn);
            m_fileDrag->setNativeDrag(true);
        }
        updateAgentWatch();
        if (!m_clipWaiting.isEmpty()) // a Receive offer made before this agent came
            conn->send(m_clipWaiting);
        if (m_sealingClients.contains(conn) && platform::clipboardInAgent(m_serviceMode)) {
            // A clipboard that came from another device while no agent was there (it was
            // restarted, or connects again) is put back; otherwise the agent drops a clipboard
            // object it may still hold from before a service restart, which would only fail.
            if (!m_clipboard->reapply())
                conn->send(makeEvent(QStringLiteral("clipClear")));
        }
#ifdef Q_OS_LINUX
        // The graphical session the agent runs in, for the helper started by this (per-user)
        // service: systemd user services do not always get it (outside GNOME and KDE). Only the
        // variables naming the display and the session bus are taken, from one agent at a time
        // (m_envClient). Linux only: on Windows the service is LocalSystem and agents of any user
        // connect; on macOS launchd gives the helper its session.
        const QCborMap envMap = msg.value(QStringLiteral("env")).toMap();
        if (!envMap.isEmpty() && (!m_envClient || m_envClient == conn)) {
            static const QStringList kAllowed = {
                QStringLiteral("DISPLAY"), QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("XAUTHORITY"),
                QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("XDG_CURRENT_DESKTOP"),
                QStringLiteral("XDG_SESSION_DESKTOP"), QStringLiteral("DESKTOP_SESSION"),
                QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("DBUS_SESSION_BUS_ADDRESS")};
            QHash<QString, QString> env;
            for (const QString &name : kAllowed) {
                const QString value = envMap.value(name).toString();
                if (!value.isEmpty() && value.size() < 1024)
                    env.insert(name, value);
            }
            if (!env.isEmpty()) {
                m_envClient = conn;
                emit sessionEnvironment(env);
            }
        }
#endif
        return;
    }
    if (handleClipboardRequest(conn, cmd, msg))
        return;
    if (cmd == QLatin1String("clipReceive")) {
        // The user takes the copied files a paste of which was refused for want of a gesture
        // (clipWaiting: Receive). Only for the current clipboard and files this device was let
        // into (never a guest's); no paste needed. Once here the clipboard is applied again with
        // the local files (ClipboardSync::receiveFiles). Answered with clipReceived.
        const QPointer<ipc::Connection> requester(conn);
        const QByteArray kid = fromHex(msg.value(QStringLiteral("kid")));
        m_clipboard->receiveFiles(kid, [requester, kid](bool ok, const QStringList &, const QString &error,
                                                        ClipboardSync::FetchError code) {
            if (!requester)
                return;
            QCborMap ev = makeEvent(QStringLiteral("clipReceived"));
            ev.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
            ev.insert(QStringLiteral("ok"), ok);
            ev.insert(QStringLiteral("error"), error);
            if (const QString name = ClipboardSync::fetchErrorName(code); !name.isEmpty())
                ev.insert(QStringLiteral("code"), name);
            requester->send(ev);
        });
        return;
    }
    if (cmd == QLatin1String("clipWaitingDismiss")) {
        // The user hid the Receive offer: not made again to an agent that connects later, and
        // hidden by the other agents too (count 0). The files stay on the clipboard for a paste.
        const QString kid = msg.value(QStringLiteral("kid")).toString();
        if (!m_clipWaiting.isEmpty() && !kid.isEmpty() && m_clipWaiting.value(QStringLiteral("kid")).toString() == kid) {
            QCborMap ev = makeEvent(QStringLiteral("clipWaiting"));
            ev.insert(QStringLiteral("kid"), kid);
            ev.insert(QStringLiteral("count"), 0);
            ev.insert(QStringLiteral("from"), m_clipWaiting.value(QStringLiteral("from")));
            m_clipWaiting = QCborMap();
            broadcast(ev);
        }
        return;
    }
    if (cmd == QLatin1String("clip")) {
        if (platform::clipboardInAgent(m_serviceMode))
            handleClipboardChange(msg);
        return;
    }
    if (cmd == QLatin1String("agentReply")) {
        m_agentRequests.reply(msg.value(QStringLiteral("rid")).toInteger(), conn, msg); // only from one asked
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
    if (cmd == QLatin1String("cancelTransfer")) {
        // Only a transfer the user is shown (the agent got its id from a transfer event).
        const quint64 handle = quint64(msg.value(QStringLiteral("id")).toInteger());
        if (m_transfer->isVisibleFetch(handle))
            m_transfer->cancel(handle);
        return;
    }
    if (cmd == QLatin1String("keyDiag")) {
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
        if (msg.value(QStringLiteral("on")).toBool())
            m_keyDiag->start();
        else
            m_keyDiag->stop();
#endif
        return; // without keyboard diagnostics built: ignored
    }
    if (cmd == QLatin1String("getState")) {
        conn->send(stateEvent());
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
        if (!m_keyDiag->lines().isEmpty()) { // the diagnostic's lines so far, for a new agent
            QCborMap ev = makeEvent(QStringLiteral("keyDiagLines"));
            ev.insert(QStringLiteral("lines"), QCborArray::fromStringList(m_keyDiag->lines()));
            ev.insert(QStringLiteral("h"), m_keyDiag->header().size());
            conn->send(ev);
        }
#endif
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
        if (!m_node->requestPairing(id, trustArgument(msg))) {
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
    } else if (cmd == QLatin1String("unforget")) {
        m_node->unforget(id);
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
            m_agentWatch.setAutostart(on);
            scheduleState();
        }
    } else if (cmd == QLatin1String("setClipboardSharing")) {
        m_clipboard->setEnabled(msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("setRemoteView")) {
        m_workspace->setRemoteView(id, msg.value(QStringLiteral("on")).toBool());
    } else if (cmd == QLatin1String("setViewPlan")) {
        // Where this device shows each screen of `id` (local, never gossiped): remote screen id ->
        // local screen id, or Workspace::kHiddenScreen. Bounded like the screen ids themselves.
        const QCborMap map = msg.value(QStringLiteral("plan")).toMap();
        if (map.size() <= kMaxViewPlanEntries) {
            Workspace::ViewPlan plan;
            for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                const QString remote = it.key().toString().left(128);
                if (!remote.isEmpty())
                    plan.insert(remote, it.value().toString().left(128));
            }
            m_workspace->setViewPlan(id, plan);
        }
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
    } else if (cmd == QLatin1String("cancelUpdate")) {
        m_updater->cancel();
    } else if (cmd == QLatin1String("setTrust")) {
        const TrustLevel level = trustArgument(msg);
        m_node->setTrust(id, level);
        if (level == TrustLevel::Guest)
            m_remoteView->revoke(id); // a guest views nothing here
    } else if (cmd == QLatin1String("setPairTrust")) {
        // The answering side of a pairing (PIN shown here) chooses what the peer becomes; only
        // while that pairing is under way (a late choice must not stick to a later one).
        if (!m_node->setPairTrust(id, trustArgument(msg)))
            qInfo() << "Trust level for a pairing that is not under way: ignored";
    } else if (cmd == QLatin1String("setViewAllowed")) {
        // Granted in advance too: a paired master already has this keyboard and mouse. While a
        // prompt is open its input is blocked, so only the local user can grant it from here.
        if (msg.value(QStringLiteral("on")).toBool()) {
            m_node->setViewAllowed(id, true);
            m_remoteView->answer(id, RemoteView::Decision::Always); // its open prompt, if any
        } else {
            m_node->setViewAllowed(id, false);
            m_remoteView->revoke(id);
        }
    } else if (cmd == QLatin1String("setOtherSubnets")) {
        if (platform::firewallScopeSupported(m_serviceMode)) {
            const bool on = msg.value(QStringLiteral("on")).toBool();
            m_settings->setValue(QStringLiteral("otherSubnets"), on);
            platform::setFirewallOtherSubnets(on);
            scheduleState();
        }
    } else if (cmd == QLatin1String("setSwapCtrlCmd")) {
        if (id.isEmpty())
            return;
        QSet<QByteArray> devices = swapCtrlCmdDevices();
        if (msg.value(QStringLiteral("on")).toBool())
            devices.insert(id);
        else
            devices.remove(id);
        QJsonArray list;
        for (const QByteArray &d : std::as_const(devices))
            list.append(QString::fromLatin1(d.toHex()));
        m_settings->setValue(QStringLiteral("swapCtrlCmd"), list);
        m_input->setSwapCtrlCmd(devices);
        scheduleState();
    } else if (cmd == QLatin1String("setPasteMove")) {
        // Large pastes of copied files moved out of their transfer folder (the Windows agent's
        // move paste) or copied as virtual files: from the next clipboard on.
        m_settings->setValue(QStringLiteral("pasteMove"), msg.value(QStringLiteral("on")).toBool());
        scheduleState();
    } else if (cmd == QLatin1String("setRemoteViewEnabled")) {
        const bool on = msg.value(QStringLiteral("on")).toBool();
        m_settings->setValue(QStringLiteral("remoteView"), on);
        m_node->setRemoteViewEnabled(on); // ends a view of this screen
        if (!on && m_input->isViewing())
            m_input->cancelView(); // and of another one
        scheduleState();
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
        m_remoteView->frameShown(quint32(msg.value(QStringLiteral("s")).toInteger()),
                                 int(msg.value(QStringLiteral("i")).toInteger()));
    } else if (cmd == QLatin1String("vfail")) {
        // Of one screen, but the whole view ends: the viewer cannot show it half.
        m_remoteView->captureFailed(msg.value(QStringLiteral("r")).toString().left(200));
    } else if (cmd == QLatin1String("clip")) {
        if (!platform::clipboardInAgent(m_serviceMode))
            handleClipboardChange(msg);
    } else if (cmd == QLatin1String("inputStatus")) {
        // What keeps the helper from working (a missing permission...), shown by the agents until
        // it is fixed. Bounded: it comes from the helper, but it is only text for a banner.
        const QString problem = msg.value(QStringLiteral("problem")).toString().left(500);
        QString link = msg.value(QStringLiteral("link")).toString().left(300);
        // Only the system's own settings pages: the agent opens this link when clicked.
        if (!link.startsWith(QLatin1String("x-apple.systempreferences:")))
            link.clear();
        if (problem != m_inputProblem || link != m_inputProblemLink) {
            m_inputProblem = problem;
            m_inputProblemLink = link;
            scheduleState();
        }
    } else if (cmd == QLatin1String("notice")) {
        // A notice from the helper for the user (macOS: copied files ready to be pasted again).
        const QString text = msg.value(QStringLiteral("text")).toString().left(500);
        if (!text.isEmpty())
            sendNotice(text, msg.value(QStringLiteral("problem")).toBool());
    } else if (cmd == QLatin1String("diag")) {
        // A keyboard diagnostics line from the helper (only taken while one runs; bounded there).
        // Without keyboard diagnostics built: ignored.
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
        m_keyDiag->addLine(QStringLiteral("helper: ") + msg.value(QStringLiteral("l")).toString());
#endif
    } else if (cmd == QLatin1String("localPress")) {
        // A paste gesture on this device's own keyboard or mouse (the helper sends one per
        // qualifying press, without saying which key): paste evidence for file data. Only the
        // helper's connection gets here, and it is taken whether or not the input is away.
        m_input->noteLocalPasteGesture();
    } else if (cmd == QLatin1String("localPressSupport")) {
        m_input->setLocalPressSupported(msg.value(QStringLiteral("on")).toBool());
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
                                 std::function<void(const QCborMap &)> done, const QList<ipc::Connection *> &to)
{
    const QList<ipc::Connection *> agents = to.isEmpty() ? m_clients : to;
    if (agents.isEmpty()) {
        QTimer::singleShot(0, this, [done] { done({}); });
        return;
    }
    const qint64 id = m_agentRequests.add(std::move(done), agents);
    QCborMap ev = makeEvent(event);
    for (auto it = args.constBegin(); it != args.constEnd(); ++it)
        ev.insert(it.key(), it.value());
    ev.insert(QStringLiteral("rid"), id);
    for (ipc::Connection *c : agents)
        c->send(ev);
    QTimer::singleShot(timeoutMs, this, [this, id] { m_agentRequests.expire(id); });
}

void ServiceController::sendNotice(const QString &text, bool problem)
{
    QCborMap ev = makeEvent(QStringLiteral("notice"));
    ev.insert(QStringLiteral("message"), text);
    ev.insert(QStringLiteral("problem"), problem);
    broadcast(ev);
}

QString ServiceController::peerName(const QByteArray &id) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == id)
            return p.name;
    }
    return QString::fromLatin1(id.toHex().left(8));
}

QSet<QByteArray> ServiceController::swapCtrlCmdDevices() const
{
    QSet<QByteArray> devices;
    for (const QJsonValue &v : m_settings->value(QStringLiteral("swapCtrlCmd")).toArray()) {
        const QByteArray id = QByteArray::fromHex(v.toString().toLatin1());
        if (!id.isEmpty())
            devices.insert(id);
    }
    return devices;
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
    // Sealed content only to agents that render it (see m_sealingClients); plaintext (a phone's
    // text) to every agent.
    const bool sealed = event.contains(QStringLiteral("sealed"));
    for (ipc::Connection *c : std::as_const(m_clients)) {
        if (!sealed || m_sealingClients.contains(c))
            c->send(event);
    }
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

void ServiceController::connectTrustNotices()
{
    // What the other side decided, said where the user notices its effect: the cursor stopping
    // at an edge, a layout edit that others ignore, a group that did not form.
    const auto inputNotice = [this](const QByteArray &device) {
        const bool guestThere = m_node->declaredTrust(device) == TrustLevel::Guest;
        const QString name = peerName(device);
        sendNotice(guestThere ? tr("%1 treats this device as a guest: it does not take this keyboard and mouse. "
                                   "Change this device's trust level on %1 to use it from here.")
                                    .arg(name)
                              : tr("%1 cannot take the keyboard and mouse right now (is DeskFlower running there?)")
                                    .arg(name),
                   true);
    };
    connect(m_input, &InputController::inputRefused, this, inputNotice);
    connect(m_input, &InputController::crossingBlocked, this, inputNotice);
    connect(m_workspace, &Workspace::editRefused, this,
            [this](const QByteArray &peer, const QString &kind, const QString &) {
                const QString name = peerName(peer);
                sendNotice(kind == QLatin1String("master")
                               ? tr("%1 treats this device as a guest: it does not follow this keyboard and mouse.")
                                     .arg(name)
                               : tr("%1 treats this device as a guest: it keeps its own arrangement of the screens, "
                                    "so the change made here does not apply there.")
                                     .arg(name),
                           true);
            });
    connect(m_node, &Node::pairingLevelDiffers, this,
            [this](const QByteArray &peer, TrustLevel chosen, TrustLevel declared) {
                const QString name = peerName(peer);
                QString version;
                for (const PeerView &p : m_node->peers()) {
                    if (p.id == peer)
                        version = p.version;
                }
                // Before 0.1.20 there were no levels: such a device always declares Device.
                const bool knowsLevels = QVersionNumber::fromString(version) >= QVersionNumber(0, 1, 20);
                QString text;
                if (declared == TrustLevel::Guest)
                    text = tr("%1 made this device a guest: it does not take this keyboard and mouse, and gets "
                              "the clipboard only when you paste on it.")
                               .arg(name);
                else if (chosen == TrustLevel::Group && !knowsLevels)
                    text = tr("%1 runs an older DeskFlower without groups: it trusts this device by itself, so it "
                              "does not join your other devices. Update it and choose \"%2\" there.")
                               .arg(name, trustText(TrustLevel::Group));
                else
                    text = tr("%1 chose \"%2\" for this device, not \"%3\": the two share only "
                              "\"%2\". To join the group, choose \"%3\" for this device on %1 too.")
                               .arg(name, trustText(declared), trustText(chosen));
                sendNotice(text, true);
            });
    connect(m_node, &Node::forgottenIntroductionSkipped, this,
            [this](const QByteArray &introducer, const QByteArray &, const QString &name) {
                sendNotice(tr("%1 wanted to add %2 to your devices, but you forgot %2 on this device. Pair it "
                              "with a PIN, or allow it again under Forgotten devices.")
                               .arg(peerName(introducer), name.isEmpty() ? tr("a device") : name),
                           false);
            });
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
        m.insert(QStringLiteral("viaNat"), p.viaNat);
        m.insert(QStringLiteral("trusted"), p.trusted);
        m.insert(QStringLiteral("connected"), p.connected);
        m.insert(QStringLiteral("exploring"), p.exploring);
        m.insert(QStringLiteral("allowView"), p.allowView);
        m.insert(QStringLiteral("trust"), trustName(p.trust));
        m.insert(QStringLiteral("effectiveTrust"), trustName(p.effectiveTrust));
        // What it declared for this device ("" before its first hello).
        m.insert(QStringLiteral("declaredTrust"), p.declaredTrust ? trustName(*p.declaredTrust) : QString());
        m.insert(QStringLiteral("version"), p.version);
        m.insert(QStringLiteral("update"), m_updater->offeredUpdate(p)); // newer version it can send us
        // Its transfer protocol (0: not known yet), and whether it differs from this device's:
        // "older" or "newer" (connected paired devices only: the agent shows an alert), with
        // whether this device's update package would update it.
        m.insert(QStringLiteral("protocol"), p.protocol);
        QString mismatch;
        if (p.trusted && p.connected && p.protocol > 0 && p.protocol != wire::kProtocolVersion)
            mismatch = p.protocol < wire::kProtocolVersion ? QStringLiteral("older") : QStringLiteral("newer");
        m.insert(QStringLiteral("protocolMismatch"), mismatch);
        m.insert(QStringLiteral("canServeUpdate"), m_updater->servesUpdateFor(p));
        m.insert(QStringLiteral("swapCtrlCmd"), m_input->swapsCtrlCmd(p.id));
        peers.append(m);
    }

    QCborMap ev = makeEvent(QStringLiteral("state"));
    ev.insert(QStringLiteral("self"), self);
    ev.insert(QStringLiteral("exploring"), m_node->isExploring());
    ev.insert(QStringLiteral("clipboard"), m_clipboard->isEnabled());
    ev.insert(QStringLiteral("autostartSupported"), platform::agentAutostartSupported(m_serviceMode));
    ev.insert(QStringLiteral("autostart"), m_settings->value(QStringLiteral("agentAutostart"), true).toBool());
    ev.insert(QStringLiteral("remoteViewEnabled"), m_node->isRemoteViewEnabled());
    ev.insert(QStringLiteral("otherSubnetsSupported"), platform::firewallScopeSupported(m_serviceMode));
    ev.insert(QStringLiteral("otherSubnets"), m_settings->value(QStringLiteral("otherSubnets")).toBool());
    ev.insert(QStringLiteral("pasteMove"), m_settings->value(QStringLiteral("pasteMove"), false).toBool());
    ev.insert(QStringLiteral("inputProblem"), m_inputProblem);
    ev.insert(QStringLiteral("inputProblemLink"), m_inputProblemLink);
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
    // Forgotten devices, which group members cannot introduce until allowed again (unforget).
    QCborArray forgotten;
    const QHash<QByteArray, QString> forgottenPeers = m_node->forgottenPeers();
    for (auto it = forgottenPeers.constBegin(); it != forgottenPeers.constEnd(); ++it) {
        if (m_node->isTrusted(it.key()))
            continue; // paired again by another user of this device (tombstones are per device)
        QCborMap f;
        f.insert(QStringLiteral("id"), hexId(it.key()));
        f.insert(QStringLiteral("name"), it.value());
        forgotten.append(f);
    }
    ev.insert(QStringLiteral("forgotten"), forgotten);
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
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
    // Keyboard diagnostics: running, for how much longer, and who started it (empty: this device).
    QCborMap diag;
    diag.insert(QStringLiteral("on"), m_keyDiag->isActive());
    diag.insert(QStringLiteral("ms"), m_keyDiag->remainingMs());
    diag.insert(QStringLiteral("by"), m_keyDiag->requestedBy().isEmpty() ? QString() : peerName(m_keyDiag->requestedBy()));
    ev.insert(QStringLiteral("keyDiag"), diag);
#endif
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
        // Guests either way, for a badge on the pack: a guest here, or a device that treats us so.
        d.insert(QStringLiteral("trust"), self ? QString() : trustName(peers.value(id).trust));
        d.insert(QStringLiteral("guestThere"), !self && peers.value(id).declaredTrust == TrustLevel::Guest);
        QCborMap plan;
        const Workspace::ViewPlan viewPlan = m_workspace->viewPlan(id);
        for (auto it = viewPlan.constBegin(); it != viewPlan.constEnd(); ++it)
            plan.insert(it.key(), it.value());
        d.insert(QStringLiteral("viewPlan"), plan); // empty: every screen where the edge is crossed
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
