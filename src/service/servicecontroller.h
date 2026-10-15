#pragma once

#include "ipc.h"
#include "clipboardsync.h"
#include "filedrag.h"
#include "filetransfer.h"
#include "inputcontroller.h"
#include "node.h"
#include "phoneserver.h"
#include "remoteview.h"
#include "settings.h"
#include "updater.h"
#include "workspace.h"

#include <QLocalServer>
#include <QObject>
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
    bool listen(QString *error);

private:
    void onNewConnection();
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
    // A request to the agents (the first answer wins); `done` gets an empty map without one.
    void askAgent(const QString &event, QCborMap args, int timeoutMs, std::function<void(const QCborMap &)> done);

    Node *m_node;
    Workspace *m_workspace;
    InputController *m_input;
    ClipboardSync *m_clipboard;
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
    QString m_instance;
    bool m_serviceMode;
    QLocalServer m_server;
    QList<ipc::Connection *> m_clients;
    QTimer m_stateTimer;
    // Pairing prompts are replayed to agents that connect while a pairing is in progress.
    QCborMap m_pendingPin;
    QCborMap m_pendingPrompt;
    QCborMap m_pendingViewPrompt; // someone asks to see this screen
    std::unique_ptr<FileDrag> m_fileDrag;
    qint64 m_agentRequestSeq = 0;
    QHash<qint64, std::function<void(const QCborMap &)>> m_agentRequests;
};

} // namespace df
