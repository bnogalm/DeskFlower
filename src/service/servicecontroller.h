#pragma once

#include "ipc.h"
#include "agentrequests.h"
#include "agentwatch.h"
#include "clipboardsync.h"
#include "filedrag.h"
#include "filetransfer.h"
#include "inputcontroller.h"
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
#include "keydiagnostics.h"
#endif
#include "node.h"
#include "phoneserver.h"
#include "remoteview.h"
#include "settings.h"
#include "updater.h"
#include "workspace.h"

#include <QLocalServer>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>

namespace df {

// Bridges the Node to the agents connected over local IPC.
class ServiceController : public QObject
{
    Q_OBJECT
public:
    // sessionToken: secret of the session helper started by this service (empty: none expected).
    ServiceController(Node *node, Workspace *workspace, InputController *input, ClipboardSync *clipboard,
                      FileTransfer *transfer, Settings *settings, web::PhoneServer *phone, RemoteView *remoteView, Updater *updater,
                      std::function<QString()> sharedFolder, std::function<QString()> receivedFolder,
                      const QString &instance, bool serviceMode, const QByteArray &sessionToken,
                      QObject *parent = nullptr);
    ~ServiceController() override;
    bool listen(QString *error);

    // Re-checks every IPC client (the session helper included) against the active console session
    // and drops those that no longer belong to it; when the console user changed, also forgets the
    // clipboard, thumbnails and pending prompts and revokes phone sessions (a fast user switch must
    // not leak them). Also what SessionLauncher asks for when it sees the session change.
    void onConsoleSessionChanged();

signals:
    // Linux: the agent's graphical session environment (DISPLAY, WAYLAND_DISPLAY...), for the
    // session helper (SessionLauncher::setSessionEnvironment). Never on Windows or macOS.
    void sessionEnvironment(const QHash<QString, QString> &env);

private:
    void onNewConnection();
    // The render server (ipc::renderServerName): pastes only, nothing pushed to the client.
    void onNewRenderConnection();
    // Whether `conn` (just read from) runs in the active console session; aborts it otherwise.
    bool admitFromConsole(ipc::Connection *conn);
    // clipUnseal / clipFetch / clipList, from either server; false for any other command.
    bool handleClipboardRequest(ipc::Connection *conn, const QString &cmd, const QCborMap &msg);
    // The connection went away: the files requests it made are dropped (ClipboardSync::
    // dropFilesWaiter: a transfer only a streaming paste of it read is cancelled, and the folders
    // it read are no longer pinned).
    void dropClipboardWaiters(ipc::Connection *conn);
    // Feeds the agent watch (whether a user and an agent are there).
    void updateAgentWatch();
    void handleCommand(ipc::Connection *conn, const QCborMap &msg);
    void handleSessionCommand(const QString &cmd, const QCborMap &msg);
    void sendToSession(const QCborMap &event);
    void updateUserPresence();
    void sendClipboardConfig();
    // The clipboard is shared through the session helper or the agents (platform::clipboardInAgent).
    QCborMap clipboardConfig(bool forAgent) const;
    void sendToClipboardOwner(const QCborMap &event);
    void handleClipboardChange(const QCborMap &msg);
    void setPhoneAccess(bool on);
    QCborMap filesState() const;
    QCborMap updatesState() const;
    void broadcast(const QCborMap &event);
    QCborMap stateEvent() const;
    QCborMap workspaceState() const;
    QCborMap thumbEvent(const QByteArray &deviceId, const QString &screenId, const QByteArray &jpeg) const;
    void scheduleState();
    // A request to the agents (the first answer wins); `done` gets an empty map without one: no
    // agent, no answer within timeoutMs, or every agent asked disconnected first. `to`: only these
    // agents (default: all).
    void askAgent(const QString &event, QCborMap args, int timeoutMs, std::function<void(const QCborMap &)> done,
                  const QList<ipc::Connection *> &to = {});
    // A notice event for the agents (problem: something went wrong).
    void sendNotice(const QString &text, bool problem);
    // Notices about trust levels (the other side's choices, group introductions).
    void connectTrustNotices();
    QString peerName(const QByteArray &id) const;
    // Devices whose Control and Command keys are swapped (settings: swapCtrlCmd, hex ids).
    QSet<QByteArray> swapCtrlCmdDevices() const;

    Node *m_node;
    Workspace *m_workspace;
    InputController *m_input;
    ClipboardSync *m_clipboard;
    FileTransfer *m_transfer;
    Settings *m_settings;
    web::PhoneServer *m_phone;
    RemoteView *m_remoteView;
    Updater *m_updater;
    std::function<QString()> m_sharedFolder;
    std::function<QString()> m_receivedFolder; // copied and dragged files (platform::receivedFolder)
    QString m_phoneError;
    QStringList m_phoneUrls;
    QByteArray m_sessionToken;
    ipc::Connection *m_session = nullptr; // the authenticated session helper
    // What keeps the helper from using the keyboard and mouse (or the screen), as it reported it
    // (inputStatus): a missing macOS permission, a refused Wayland portal... Empty when fine or
    // without a helper. The link opens the setting that fixes it (macOS only).
    QString m_inputProblem;
    QString m_inputProblemLink;
    // The Receive offer the agents were last told about (clipWaiting, ClipboardSync::filesWaiting),
    // for an agent that connects later; empty once over.
    QCborMap m_clipWaiting;
    QString m_instance;
    bool m_serviceMode;
    QLocalServer m_server;
    QList<ipc::Connection *> m_clients;
    QHash<ipc::Connection *, quint32> m_clientSession; // console session of each client (helper too)
    QSet<ipc::Connection *> m_sessionUndecided;        // decided again from the token at the first message
    // Agents that declared a version rendering the sealed clipboard ("agent" {v}): the only ones
    // sent a sealed clipSet (an older agent left running across an update would clear the
    // clipboard with it).
    QSet<ipc::Connection *> m_sealingClients;
    // Agents that run native drags of files from other devices ("agent" {dnd}).
    QSet<ipc::Connection *> m_dragClients;
    // How often a drag shown by the agent hears from the service ("dragAlive").
    static constexpr int kDragAliveMs = 2000;
    QLocalServer m_renderServer;
    QSet<ipc::Connection *> m_renderClients;
    // The files requests of each connection (clipFetch) still waiting, and those of its streaming
    // pastes until it closes (they pin the folder the paste reads): ClipboardSync waiter ids.
    QHash<ipc::Connection *, QList<quint64>> m_filesWaiters;
    // clipListed carries at most this many items per message (the IPC frame is capped at 16 MB; a
    // path of 259 UTF-16 units is at most 777 bytes of UTF-8).
    static constexpr qsizetype kListChunkItems = 5000;
    // Windows service mode: keeps an agent (the clipboard's owner) running in the console session.
    AgentWatch m_agentWatch;
    QTimer m_agentWatchTimer;
    // Linux: the agent whose graphical session the helper is given (sessionEnvironment): the first
    // one that reports it, until it disconnects, so that a second agent (another session of the
    // user, one under ssh -X) does not move the helper back and forth.
    ipc::Connection *m_envClient = nullptr;
    QTimer m_sessionWatch;                            // re-checks clients (session notifications can be missed)
    QString m_consoleUser;                             // SID of the user at the console, to notice a change
    QTimer m_stateTimer;
    // Pairing prompts are replayed to agents that connect while a pairing is in progress.
    QCborMap m_pendingPin;
    QCborMap m_pendingPrompt;
    QCborMap m_pendingViewPrompt; // someone asks to see this screen
    std::unique_ptr<FileDrag> m_fileDrag;
    AgentRequests<ipc::Connection> m_agentRequests;
    QHash<QByteArray, qint64> m_incomingBlockedNoticed; // Node::incomingBlocked notices, by peer
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
    // Keyboard diagnostics ("keyDiag {on}" from an agent, lines to the agents as "keyDiagLine").
    // Only built with DESKFLOWER_KEY_DIAGNOSTICS; without it those commands are ignored.
    std::unique_ptr<KeyDiagnostics> m_keyDiag;
#endif
};

} // namespace df
