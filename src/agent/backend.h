#pragma once

#include "clipboardbridge.h"
#include "filedragagent.h"
#include "ipc.h"

#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class ThumbnailProvider;

// Agent-side view of the service, exposed to QML.
class Backend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")

    Q_PROPERTY(bool serviceConnected READ serviceConnected NOTIFY serviceConnectedChanged)
    Q_PROPERTY(QString serviceStartError READ serviceStartError NOTIFY serviceStartErrorChanged)
    Q_PROPERTY(QString selfId READ selfId NOTIFY stateChanged)
    Q_PROPERTY(QString selfName READ selfName NOTIFY stateChanged)
    Q_PROPERTY(QString fingerprint READ fingerprint NOTIFY stateChanged)
    Q_PROPERTY(int port READ port NOTIFY stateChanged)
    // data, discovery, phone (configured) and dataInUse, tcpInUse, discoveryInUse, phoneInUse.
    Q_PROPERTY(QVariantMap ports READ ports NOTIFY stateChanged)
    Q_PROPERTY(bool exploring READ exploring NOTIFY stateChanged)
    Q_PROPERTY(bool clipboardSharing READ clipboardSharing NOTIFY stateChanged)
    // Start the agent at every sign-in (the service manages it; not supported everywhere).
    Q_PROPERTY(bool autostartSupported READ autostartSupported NOTIFY stateChanged)
    Q_PROPERTY(bool autostart READ autostart NOTIFY stateChanged)
    Q_PROPERTY(QVariantList trustedPeers READ trustedPeers NOTIFY stateChanged)
    Q_PROPERTY(QVariantList nearbyPeers READ nearbyPeers NOTIFY stateChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY stateChanged)
    Q_PROPERTY(QString masterId READ masterId NOTIFY stateChanged)
    Q_PROPERTY(QString masterName READ masterName NOTIFY stateChanged)
    Q_PROPERTY(bool sharing READ sharing NOTIFY stateChanged)
    Q_PROPERTY(QString activeId READ activeId NOTIFY stateChanged)
    Q_PROPERTY(bool sessionRunning READ sessionRunning NOTIFY stateChanged)
    Q_PROPERTY(int thumbRevision READ thumbRevision NOTIFY thumbnailsChanged)
    Q_PROPERTY(QString sharedFolder READ sharedFolder NOTIFY stateChanged)
    Q_PROPERTY(QUrl sharedFolderUrl READ sharedFolderUrl NOTIFY stateChanged)
    Q_PROPERTY(bool phoneAccess READ phoneAccess NOTIFY stateChanged)
    Q_PROPERTY(bool phoneRunning READ phoneRunning NOTIFY stateChanged)
    Q_PROPERTY(int phoneSessions READ phoneSessions NOTIFY stateChanged)
    Q_PROPERTY(QString phoneError READ phoneError NOTIFY stateChanged)
    Q_PROPERTY(QStringList phoneUrls READ phoneUrls NOTIFY stateChanged)
    // Paired device this screen is being shown on right now (empty: none).
    Q_PROPERTY(QString screenViewerName READ screenViewerName NOTIFY stateChanged)
    Q_PROPERTY(QString selfVersion READ selfVersion NOTIFY stateChanged)
    // Updates from paired devices: whether this build can verify them (signing key compiled in) and
    // install them (platform), and the one in progress ("", downloading, verifying, installing).
    Q_PROPERTY(bool updatesConfigured READ updatesConfigured NOTIFY stateChanged)
    Q_PROPERTY(bool updatesSupported READ updatesSupported NOTIFY stateChanged)
    Q_PROPERTY(QString updateState READ updateState NOTIFY stateChanged)
    Q_PROPERTY(QString updatePeerId READ updatePeerId NOTIFY stateChanged)
    Q_PROPERTY(double updateProgress READ updateProgress NOTIFY stateChanged)

public:
    Backend(const QString &instance, ThumbnailProvider *thumbnails, QObject *parent = nullptr);

    bool serviceConnected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    QString serviceStartError() const { return m_serviceStartError; }
    QString selfId() const { return m_selfId; }
    QString selfName() const { return m_selfName; }
    QString fingerprint() const { return m_fingerprint; }
    int port() const { return m_port; }
    QVariantMap ports() const { return m_ports; }
    bool exploring() const { return m_exploring; }
    bool clipboardSharing() const { return m_clipboardSharing; }
    bool autostartSupported() const { return m_autostartSupported; }
    bool autostart() const { return m_autostart; }
    QVariantList trustedPeers() const { return m_trusted; }
    QVariantList nearbyPeers() const { return m_nearby; }
    QVariantList devices() const { return m_devices; }
    QString masterId() const { return m_masterId; }
    QString masterName() const;
    bool sharing() const { return !m_masterId.isEmpty() && m_masterId == m_selfId; }
    int thumbRevision() const { return m_thumbRevision; }
    QString activeId() const { return m_activeId; }
    bool sessionRunning() const { return m_sessionRunning; }
    QString sharedFolder() const { return m_files.value(QStringLiteral("folder")).toString(); }
    QUrl sharedFolderUrl() const { return QUrl::fromLocalFile(sharedFolder()); }
    bool phoneAccess() const { return m_files.value(QStringLiteral("phoneAccess")).toBool(); }
    bool phoneRunning() const { return m_files.value(QStringLiteral("phoneRunning")).toBool(); }
    int phoneSessions() const { return m_files.value(QStringLiteral("phoneSessions")).toInt(); }
    QString phoneError() const { return m_files.value(QStringLiteral("phoneError")).toString(); }
    QStringList phoneUrls() const { return m_files.value(QStringLiteral("phoneUrls")).toStringList(); }
    QString screenViewerName() const { return m_screenViewerName; }
    QString selfVersion() const { return m_selfVersion; }
    bool updatesConfigured() const { return m_updates.value(QStringLiteral("configured")).toBool(); }
    bool updatesSupported() const { return m_updates.value(QStringLiteral("supported")).toBool(); }
    QString updateState() const { return m_updates.value(QStringLiteral("state")).toString(); }
    QString updatePeerId() const { return m_updates.value(QStringLiteral("peer")).toString(); }
    double updateProgress() const;

    // Quitting from the tray stops the service too (and with it every connection), until the
    // agent starts it again: at its next launch, or with startService (the banner's button).
    Q_INVOKABLE void stopServiceAndQuit();
    Q_INVOKABLE void startService();
    // Stops the service and starts it again (new ports take effect).
    Q_INVOKABLE void restartService();
    // Advanced settings: saved by the service, used from its next start.
    Q_INVOKABLE void setPorts(int data, int discovery, int phone);
    Q_INVOKABLE void setExploring(bool on);
    Q_INVOKABLE void pair(const QString &peerId);
    Q_INVOKABLE void submitPin(const QString &peerId, const QString &pin);
    Q_INVOKABLE void cancelPair(const QString &peerId);
    Q_INVOKABLE void unpair(const QString &peerId);
    Q_INVOKABLE void rename(const QString &name);
    Q_INVOKABLE void addPeer(const QString &host, int port);
    Q_INVOKABLE void setSharing(bool on);
    Q_INVOKABLE void setRemoteView(const QString &deviceId, bool on);
    Q_INVOKABLE void setClipboardSharing(bool on);
    Q_INVOKABLE void setAutostart(bool on);
    // packs: [{id, x, y}] pack offsets in the shared plane.
    Q_INVOKABLE void setLayout(const QVariantList &packs);
    // Asks this device's session helper and every paired device for fresh captures.
    Q_INVOKABLE void refreshThumbnails();
    // Shared folder (phone uploads, files received through the clipboard).
    Q_INVOKABLE void setSharedFolder(const QUrl &folder);
    Q_INVOKABLE void openSharedFolder();
    Q_INVOKABLE void setPhoneAccess(bool on);
    Q_INVOKABLE void newPhoneLink();
    Q_INVOKABLE void revokePhones();
    // Remote view of this screen. decision: "once", "always" or "deny".
    Q_INVOKABLE void answerView(const QString &peerId, const QString &decision);
    Q_INVOKABLE void stopScreenView();
    Q_INVOKABLE void setViewAllowed(const QString &peerId, bool on);
    // Installs the newer signed package a paired device offers (its "update" field).
    Q_INVOKABLE void updateFrom(const QString &peerId);
    // License documents built into the agent (About window): file names, and the text of one.
    Q_INVOKABLE QStringList licenseDocuments() const;
    Q_INVOKABLE QString licenseDocument(const QString &name) const;

signals:
    void serviceConnectedChanged();
    void serviceStartErrorChanged();
    void stateChanged();
    void pinShown(const QString &peerId, const QString &peerName, const QString &pin);
    void pinRequested(const QString &peerId, const QString &peerName);
    void pairingFinished(const QString &peerId, bool ok, const QString &error);
    void viewPromptRequested(const QString &peerId, const QString &peerName, int timeoutMs);
    void viewPromptClosed(const QString &peerId);
    void thumbnailsChanged();
    void phoneUploaded(const QString &fileName);
    void serviceError(const QString &message);
    // Something the user may want to know (e.g. copied files received); problem: it went wrong.
    void notice(const QString &message, bool problem);
    void updateFinished(bool ok, const QString &message);

private:
    void connectToService();
    void onMessage(const QCborMap &msg);
    void command(const QString &cmd, QCborMap args = {});
    static QString thumbKey(const QString &deviceId, const QString &screenId);

    void setServiceStartError(const QString &error);

    QString m_instance;
    QString m_serverName;
    QString m_serviceStartError;
    bool m_triedStart = false;
    QLocalSocket m_socket;
    df::ipc::Connection m_conn;
    QTimer m_reconnect;
    ThumbnailProvider *m_thumbnails;

    QString m_selfId;
    QString m_selfName;
    QString m_fingerprint;
    int m_port = 0;
    QVariantMap m_ports;
    int m_restartAttempts = 0; // restartService: starts left to try once the service is gone
    bool m_exploring = false;
    bool m_clipboardSharing = true;
    // Windows service mode: this agent (running as the user) owns the user's clipboard for sharing.
    ClipboardBridge m_clipboard;
    // Dragging files between devices: the service asks, this agent talks to Explorer.
    FileDragAgent m_fileDrag;
    void replyToService(qint64 requestId, QCborMap reply);
    // Files copied on another device, transferred while an application waits for them (paste).
    static constexpr int kPasteTimeoutMs = 30 * 60 * 1000;
    QStringList fetchClipboardFiles(QString *error) const;
    // Asks the service on a connection of its own and waits (no event loop, any thread) for
    // `replyEvent` {ok, files, error}: files transferred for a paste or a drop.
    QStringList fetchFromService(const QString &cmd, QCborMap args, const QString &replyEvent, QString *error) const;
    bool m_autostartSupported = false;
    bool m_autostart = false;
    QVariantList m_trusted;
    QVariantList m_nearby;
    QVariantList m_devices;
    QString m_masterId;
    QString m_activeId;
    bool m_sessionRunning = false;
    QVariantMap m_files;
    QString m_screenViewerName;
    QString m_selfVersion;
    QVariantMap m_updates;
    int m_thumbRevision = 0;
};
