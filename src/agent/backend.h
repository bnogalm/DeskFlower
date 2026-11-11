#pragma once

#include "clipboardbridge.h"
#include "filedragagent.h"
#include "ipc.h"
#include "modeltypes.h"

#include <QElapsedTimer>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class ThumbnailProvider;

// Agent-side view of the service, exposed to QML.
class Backend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")

    Q_PROPERTY(bool serviceConnected READ serviceConnected NOTIFY serviceConnectedChanged FINAL)
    Q_PROPERTY(QString serviceStartError READ serviceStartError NOTIFY serviceStartErrorChanged FINAL)
    Q_PROPERTY(QString selfId READ selfId NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString selfName READ selfName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString fingerprint READ fingerprint NOTIFY stateChanged FINAL)
    Q_PROPERTY(int port READ port NOTIFY stateChanged FINAL)
    // data, discovery, phone (configured) and dataInUse, tcpInUse, discoveryInUse, phoneInUse.
    Q_PROPERTY(PortsInfo ports READ ports NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool exploring READ exploring NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool clipboardSharing READ clipboardSharing NOTIFY stateChanged FINAL)
    // Start the agent at every sign-in (the service manages it; not supported everywhere).
    Q_PROPERTY(bool autostartSupported READ autostartSupported NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool autostart READ autostart NOTIFY stateChanged FINAL)
    // Remote view at all on this device: off, nobody views this screen and it views nobody's.
    Q_PROPERTY(bool remoteViewEnabled READ remoteViewEnabled NOTIFY stateChanged FINAL)
    // Windows service: let devices on other private networks through the firewall on public networks.
    Q_PROPERTY(bool otherSubnetsSupported READ otherSubnetsSupported NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool otherSubnets READ otherSubnets NOTIFY stateChanged FINAL)
    // Large pastes of copied files moved out of their transfer folder instead of copied (the
    // Windows agent's move paste, see ClipboardBridge::FileStreaming::move). Kept by the service.
    Q_PROPERTY(bool pasteMoveSupported READ pasteMoveSupported CONSTANT FINAL)
    Q_PROPERTY(bool pasteMove READ pasteMove NOTIFY stateChanged FINAL)
    // What keeps this computer's keyboard, mouse or screen from being shared (a missing macOS
    // permission, a refused Wayland portal...), and a link to the system setting that fixes it
    // (if there is one). Empty when fine.
    Q_PROPERTY(QString inputProblem READ inputProblem NOTIFY stateChanged FINAL)
    Q_PROPERTY(QUrl inputProblemLink READ inputProblemLink NOTIFY stateChanged FINAL)
    Q_PROPERTY(QList<PeerInfo> trustedPeers READ trustedPeers NOTIFY stateChanged FINAL)
    // Connected paired devices of another protocol (PeerInfo::protocolMismatch): a banner each.
    Q_PROPERTY(QList<PeerInfo> protocolAlerts READ protocolAlerts NOTIFY stateChanged FINAL)
    Q_PROPERTY(QList<PeerInfo> nearbyPeers READ nearbyPeers NOTIFY stateChanged FINAL)
    // Devices forgotten here, which group members cannot introduce again (only id and name).
    Q_PROPERTY(QList<PeerInfo> forgottenPeers READ forgottenPeers NOTIFY stateChanged FINAL)
    // This device's id, then those of the connected paired devices (empty ones left out): one
    // flower each in the bouquet.
    Q_PROPERTY(QStringList connectedIds READ connectedIds NOTIFY stateChanged FINAL)
    Q_PROPERTY(QList<DeviceInfo> devices READ devices NOTIFY stateChanged FINAL)
    // This device's entry of `devices` (an empty one until its screens are known).
    Q_PROPERTY(DeviceInfo selfDevice READ selfDevice NOTIFY stateChanged FINAL)
    // Where each device of `devices` is in the shared plane (same order): the left, top, right
    // and bottom of the rectangle around its screens moved by its pack offset, four numbers per
    // device in one list (a list of numbers compiles to C++ in QML, one of rectangles does not).
    Q_PROPERTY(QList<double> packEdges READ packEdges NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString masterId READ masterId NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString masterName READ masterName NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool sharing READ sharing NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString activeId READ activeId NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool sessionRunning READ sessionRunning NOTIFY stateChanged FINAL)
    Q_PROPERTY(int thumbRevision READ thumbRevision NOTIFY thumbnailsChanged FINAL)
    Q_PROPERTY(QString sharedFolder READ sharedFolder NOTIFY stateChanged FINAL)
    Q_PROPERTY(QUrl sharedFolderUrl READ sharedFolderUrl NOTIFY stateChanged FINAL)
    // Web page with the latest installers (to update by hand).
    Q_PROPERTY(QUrl downloadsUrl READ downloadsUrl CONSTANT FINAL)
    Q_PROPERTY(bool phoneAccess READ phoneAccess NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool phoneRunning READ phoneRunning NOTIFY stateChanged FINAL)
    Q_PROPERTY(int phoneSessions READ phoneSessions NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString phoneError READ phoneError NOTIFY stateChanged FINAL)
    Q_PROPERTY(QStringList phoneUrls READ phoneUrls NOTIFY stateChanged FINAL)
    // The same addresses without their access token ("/?t=..."), to tell them apart in a list.
    Q_PROPERTY(QStringList phoneUrlLabels READ phoneUrlLabels NOTIFY stateChanged FINAL)
    // Paired device this screen is being shown on right now (empty: none).
    Q_PROPERTY(QString screenViewerName READ screenViewerName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString selfVersion READ selfVersion NOTIFY stateChanged FINAL)
    // Updates from paired devices: whether this build can verify them (signing key compiled in) and
    // install them (platform), and the one in progress ("", downloading, verifying, installing).
    Q_PROPERTY(bool updatesConfigured READ updatesConfigured NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool updatesSupported READ updatesSupported NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString updateState READ updateState NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString updatePeerId READ updatePeerId NOTIFY stateChanged FINAL)
    Q_PROPERTY(double updateProgress READ updateProgress NOTIFY stateChanged FINAL)
    // Transfers in progress that the user sees (pasted or dragged files from another device):
    // oldest first. A finished one leaves the list (its outcome comes as a notice).
    Q_PROPERTY(QList<TransferInfo> transfers READ transfers NOTIFY transfersChanged FINAL)
    // What DeskFlower's progress window shows: the pasted files' transfers in progress that no file
    // manager shows itself (TransferInfo::shellDriven), then those that ended in the last few
    // seconds (kEndedShownMs).
    Q_PROPERTY(QList<TransferInfo> windowTransfers READ windowTransfers NOTIFY transfersChanged FINAL)
    // Files are being received or pasted: a transfer is in progress, or a file manager read a
    // streaming paste's files in the last few seconds (it may lag well behind the transfer, see
    // PasteSession::activeWithin). The window then stops its continuous animations: this thread
    // answers the file manager's requests for the pasted files, one per file, and every frame
    // drawn meanwhile delays one.
    Q_PROPERTY(bool transferBusy READ transferBusy NOTIFY transferBusyChanged FINAL)
    // A paste of the files copied on another device was refused for want of a paste gesture (the
    // service's clipWaiting): waitingCount files and folders copied on waitingFrom, which Receive
    // (receiveClipboard) takes anyway. 0: nothing waits. receiving: Receive was asked and not
    // answered yet; waitingError: why the last Receive failed.
    Q_PROPERTY(int waitingCount READ waitingCount NOTIFY waitingChanged FINAL)
    Q_PROPERTY(QString waitingFrom READ waitingFrom NOTIFY waitingChanged FINAL)
    Q_PROPERTY(bool receiving READ receiving NOTIFY waitingChanged FINAL)
    Q_PROPERTY(QString waitingError READ waitingError NOTIFY waitingChanged FINAL)
    // Keyboard diagnostics (df::KeyDiagnostics): whether it records, when it ends (ms since the
    // epoch, local clock), who started it (empty: this device) and the lines so far, as one text.
    // Whether this build has them (DESKFLOWER_KEY_DIAGNOSTICS); without them the UI is hidden and
    // the rest stays empty.
    Q_PROPERTY(bool keyDiagnosticsAvailable READ keyDiagnosticsAvailable CONSTANT FINAL)
    Q_PROPERTY(bool keyDiagActive READ keyDiagActive NOTIFY keyDiagChanged FINAL)
    Q_PROPERTY(double keyDiagEndsAt READ keyDiagEndsAt NOTIFY keyDiagChanged FINAL)
    Q_PROPERTY(QString keyDiagBy READ keyDiagBy NOTIFY keyDiagChanged FINAL)
    Q_PROPERTY(QString keyDiagText READ keyDiagText NOTIFY keyDiagTextChanged FINAL)

public:
    Backend(const QString &instance, ThumbnailProvider *thumbnails, QObject *parent = nullptr);

    bool serviceConnected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    QString serviceStartError() const { return m_serviceStartError; }
    QString selfId() const { return m_selfId; }
    QString selfName() const { return m_selfName; }
    QString fingerprint() const { return m_fingerprint; }
    int port() const { return m_port; }
    PortsInfo ports() const { return m_ports; }
    bool exploring() const { return m_exploring; }
    bool clipboardSharing() const { return m_clipboardSharing; }
    bool autostartSupported() const { return m_autostartSupported; }
    bool autostart() const { return m_autostart; }
    bool remoteViewEnabled() const { return m_remoteViewEnabled; }
    bool otherSubnetsSupported() const { return m_otherSubnetsSupported; }
    bool otherSubnets() const { return m_otherSubnets; }
    static bool pasteMoveSupported();
    bool pasteMove() const { return m_pasteMove; }
    QString inputProblem() const { return m_inputProblem; }
    QUrl inputProblemLink() const { return m_inputProblemLink; }
    QList<PeerInfo> trustedPeers() const { return m_trusted; }
    QList<PeerInfo> protocolAlerts() const;
    // The alert for a device of another protocol, as the banner and the notification say it.
    static QString protocolAlertText(const PeerInfo &peer);
    QList<PeerInfo> nearbyPeers() const { return m_nearby; }
    QList<PeerInfo> forgottenPeers() const { return m_forgotten; }
    QStringList connectedIds() const;
    QList<DeviceInfo> devices() const { return m_devices; }
    DeviceInfo selfDevice() const;
    QList<double> packEdges() const;
    QString masterId() const { return m_masterId; }
    QString masterName() const;
    bool sharing() const { return !m_masterId.isEmpty() && m_masterId == m_selfId; }
    int thumbRevision() const { return m_thumbRevision; }
    QString activeId() const { return m_activeId; }
    bool sessionRunning() const { return m_sessionRunning; }
    QString sharedFolder() const { return m_files.value(QStringLiteral("folder")).toString(); }
    QUrl sharedFolderUrl() const { return QUrl::fromLocalFile(sharedFolder()); }
    static QUrl downloadsUrl() { return QUrl(QStringLiteral("https://bnogalm.github.io/DeskFlower/#download")); }
    bool phoneAccess() const { return m_files.value(QStringLiteral("phoneAccess")).toBool(); }
    bool phoneRunning() const { return m_files.value(QStringLiteral("phoneRunning")).toBool(); }
    int phoneSessions() const { return m_files.value(QStringLiteral("phoneSessions")).toInt(); }
    QString phoneError() const { return m_files.value(QStringLiteral("phoneError")).toString(); }
    QStringList phoneUrls() const { return m_files.value(QStringLiteral("phoneUrls")).toStringList(); }
    QStringList phoneUrlLabels() const;
    QString screenViewerName() const { return m_screenViewerName; }
    QString selfVersion() const { return m_selfVersion; }
    bool updatesConfigured() const { return m_updates.value(QStringLiteral("configured")).toBool(); }
    bool updatesSupported() const { return m_updates.value(QStringLiteral("supported")).toBool(); }
    QString updateState() const { return m_updates.value(QStringLiteral("state")).toString(); }
    QString updatePeerId() const { return m_updates.value(QStringLiteral("peer")).toString(); }
    double updateProgress() const;
    QList<TransferInfo> transfers() const { return m_transfers; }
    QList<TransferInfo> windowTransfers() const;
    bool transferBusy() const { return m_transferBusy; }
    int waitingCount() const { return m_waitingCount; }
    QString waitingFrom() const { return m_waitingFrom; }
    bool receiving() const { return m_receiving; }
    QString waitingError() const { return m_waitingError; }
    bool keyDiagnosticsAvailable() const
    {
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
        return true;
#else
        return false;
#endif
    }
    bool keyDiagActive() const { return m_keyDiagActive; }
    double keyDiagEndsAt() const { return m_keyDiagEndsAt; }
    QString keyDiagBy() const { return m_keyDiagBy; }
    QString keyDiagText() const { return m_keyDiagText; }
    // One line for the tray tooltip about the transfers in progress (empty: none).
    QString transfersSummary() const;
    // One line for the tray tooltip about copied files waiting for Receive (empty: none).
    QString waitingSummary() const;
    // One line for the tray tooltip about the connected guests, either way (empty: none).
    QString guestsSummary() const;

    // Quitting from the tray stops the service too (and with it every connection), until the
    // agent starts it again: at its next launch, or with startService (the banner's button).
    Q_INVOKABLE void stopServiceAndQuit();
    Q_INVOKABLE void startService();
    // Stops the service and starts it again (new ports take effect).
    Q_INVOKABLE void restartService();
    // Advanced settings: saved by the service, used from its next start.
    Q_INVOKABLE void setPorts(int data, int discovery, int phone);
    Q_INVOKABLE void setExploring(bool on);
    // trust: what the device becomes here once paired: "guest" (cannot drive this keyboard and
    // mouse or view this screen; this clipboard reaches it sealed and opens only when its user
    // pastes it with the keyboard), "device" (trusted by this device only: it pastes freely) or
    // "group" (trusted by all the user's devices, which introduce each other). The level also goes
    // to the other device as a suggestion for its PIN dialog.
    Q_INVOKABLE void pair(const QString &peerId, const QString &trust);
    Q_INVOKABLE void setTrust(const QString &peerId, const QString &trust);
    // While this device shows a pairing PIN: what that pairing makes the peer.
    Q_INVOKABLE void setPairTrust(const QString &peerId, const QString &trust);
    Q_INVOKABLE void submitPin(const QString &peerId, const QString &pin);
    Q_INVOKABLE void cancelPair(const QString &peerId);
    Q_INVOKABLE void unpair(const QString &peerId);
    // A forgotten device may be introduced by group members again (forgottenPeers).
    Q_INVOKABLE void unforget(const QString &peerId);
    Q_INVOKABLE void rename(const QString &name);
    Q_INVOKABLE void addPeer(const QString &host, int port);
    Q_INVOKABLE void setSharing(bool on);
    Q_INVOKABLE void setRemoteView(const QString &deviceId, bool on);
    // The entry of `devices` with that id (an empty one when there is none).
    Q_INVOKABLE DeviceInfo device(const QString &deviceId) const;
    // Where this device shows the screens of a viewed device: for each remote screen id, the
    // local screen id, "-" for a screen not shown, or "" for the monitor where the edge was
    // crossed (also what a screen left out gets; the devices model carries the current plan as
    // `viewPlan`).
    Q_INVOKABLE void setViewPlan(const QString &deviceId, const QStringList &screenIds, const QStringList &placements);
    Q_INVOKABLE void setClipboardSharing(bool on);
    Q_INVOKABLE void setAutostart(bool on);
    Q_INVOKABLE void setRemoteViewEnabled(bool on);
    Q_INVOKABLE void setOtherSubnets(bool on);
    Q_INVOKABLE void setPasteMove(bool on);
    // Moves one pack in the shared plane to (x, y), rounded as JavaScript's Math.round does;
    // every other pack stays where `devices` has it.
    Q_INVOKABLE void moveDevice(const QString &deviceId, double x, double y);
    // Asks this device's session helper and every paired device for fresh captures.
    Q_INVOKABLE void refreshThumbnails();
    // Shared folder (phone uploads; what phones see).
    Q_INVOKABLE void setSharedFolder(const QUrl &folder);
    Q_INVOKABLE void openSharedFolder();
    Q_INVOKABLE void setPhoneAccess(bool on);
    Q_INVOKABLE void newPhoneLink();
    Q_INVOKABLE void revokePhones();
    // Remote view of this screen. decision: "once", "always" or "deny".
    Q_INVOKABLE void answerView(const QString &peerId, const QString &decision);
    Q_INVOKABLE void stopScreenView();
    Q_INVOKABLE void setViewAllowed(const QString &peerId, bool on);
    // Typing on that device (a Mac driven from a PC keyboard): Control and Command swapped.
    Q_INVOKABLE void setSwapCtrlCmd(const QString &peerId, bool on);
    // Installs the newer signed package a paired device offers (its "update" field).
    Q_INVOKABLE void updateFrom(const QString &peerId);
    // Stops the update being downloaded (updateState "downloading").
    Q_INVOKABLE void cancelUpdate();
    // Stops a transfer of `transfers` (its id); what was received so far is removed.
    Q_INVOKABLE void cancelTransfer(qint64 id);
    // Opens the folder a transfer of `windowTransfers` (its id) wrote into, once it is done.
    Q_INVOKABLE void openTransferFolder(qint64 id);
    // Takes the copied files waiting (waitingCount) with no paste (the service's clipReceive); once
    // here they are put on the clipboard again, as local files.
    Q_INVOKABLE void receiveClipboard();
    // Hides the Receive offer (the files stay where they were copied, for a paste).
    Q_INVOKABLE void dismissWaiting();
    // Keyboard diagnostics for two minutes, here and on the connected devices (or stop it).
    Q_INVOKABLE void setKeyDiagnostics(bool on);
    // License documents built into the agent (About window): file names, and the text of one.
    Q_INVOKABLE QStringList licenseDocuments() const;
    Q_INVOKABLE QString licenseDocument(const QString &name) const;

signals:
    void serviceConnectedChanged();
    void serviceStartErrorChanged();
    void stateChanged();
    // trust: the level to offer by default (what the pairing applies unless the user picks
    // another; for pinRequested, the one chosen in pair()). suggested (pinShown): a higher level
    // the requester suggested that applies only if picked here (Group), or empty.
    void pinShown(const QString &peerId, const QString &peerName, const QString &pin, const QString &trust,
                  const QString &suggested);
    void pinRequested(const QString &peerId, const QString &peerName, const QString &trust);
    void pairingFinished(const QString &peerId, bool ok, const QString &error);
    void viewPromptRequested(const QString &peerId, const QString &peerName, int timeoutMs);
    void viewPromptClosed(const QString &peerId);
    void thumbnailsChanged();
    void phoneUploaded(const QString &fileName);
    void serviceError(const QString &message);
    // The folder chosen in setSharedFolder cannot be shared (a system folder, AppData...).
    void sharedFolderRejected(const QString &path, const QString &reason);
    // Something the user may want to know (e.g. copied files received); problem: it went wrong.
    void notice(const QString &message, bool problem);
    void updateFinished(bool ok, const QString &message);
    // A paired device of another protocol connected (once per device and protocol per run): for
    // a tray notification, whether the window is shown or not.
    void protocolAlert(const QString &text);
    void transfersChanged();
    void transferBusyChanged();
    void waitingChanged();
    void keyDiagChanged();
    void keyDiagTextChanged();

private:
    void connectToService();
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Gives systemd's user manager (and D-Bus activation) this session's display variables.
    static void importSessionEnvironment();
#endif
    void onMessage(const QCborMap &msg);
    void onTransfer(const QCborMap &msg);
    void onClipWaiting(const QCborMap &msg);
    void onClipReceived(const QCborMap &msg);
    void clearWaiting();
    // Whether `target` is the folder of a paste a file manager reads while it arrives (Windows: a
    // live PasteSession's root).
    static bool isShellDriven(const QString &target);
    // Whether `target` is the folder of a move paste (Windows: a live move PasteSession's root),
    // whose files the paste moves away.
    static bool isMovePaste(const QString &target);
    // Marks the transfers in progress that isShellDriven now recognises (a paste session's folder
    // became known after their last report).
    void recheckShellDriven();
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
    PortsInfo m_ports;
    int m_restartAttempts = 0; // restartService: starts left to try once the service is gone
    bool m_exploring = false;
    bool m_clipboardSharing = true;
    // Windows service mode: this agent (running as the user) owns the user's clipboard for sharing.
    ClipboardBridge m_clipboard;
    // Dragging files between devices: the service asks, this agent talks to Explorer.
    FileDragAgent m_fileDrag;
    void replyToService(qint64 requestId, QCborMap reply);
    // Files copied on another device, transferred while an application waits for them (paste):
    // the wait lasts as long as the service keeps saying the transfer goes on (clipFetching, every
    // 10 s), and ends when it falls silent this long.
    static constexpr int kPasteIdleTimeoutMs = 2 * 60 * 1000;
    // kid: the clipboard object asking (empty with a service from before sealing). A refusal for
    // want of a paste gesture comes back as ClipboardBridge::kNotAPaste.
    QStringList fetchClipboardFiles(const QByteArray &kid, QString *error) const;
    // Connects `socket` for a blocking request: clip* commands to the render server (see
    // ipc::renderServerName), anything else, or a service without one, to the main server.
    bool connectForRequest(QLocalSocket &socket, const QString &cmd, int timeoutMs) const;
    // A clipboard object's files pasted while they arrive (a service that announced `stream`):
    // their list (clipList) and the streaming paste's session (clipFetch {stream}, Windows). move
    // (a service that announced `move`, for a large paste): the move paste's session too
    // (clipFetch {stream, fresh}, shown in DeskFlower's window). Empty elsewhere.
    ClipboardBridge::FileStreaming fileStreaming(const QByteArray &kid, bool stream, bool move) const;
    // The plaintext of one sealed clipboard format (a paste): asks the service, which asks the
    // master for the key. Empty on refusal or timeout.
    QByteArray unsealClipboardFormat(const QByteArray &kid, const QString &mime, QString *error) const;
    // Asks the service on a connection of its own and waits (no event loop, any thread) for
    // `replyEvent`: `files` for a paste or a drop (filesOut), or the `data` bytes of a reply.
    QStringList fetchFromService(const QString &cmd, QCborMap args, const QString &replyEvent, QString *error) const;
    QByteArray requestFromService(const QString &cmd, QCborMap args, const QString &replyEvent, int timeoutMs,
                                  QString *error) const;
    bool m_autostartSupported = false;
    bool m_autostart = false;
    bool m_remoteViewEnabled = true;
    bool m_otherSubnetsSupported = false;
    bool m_otherSubnets = false;
    bool m_pasteMove = false;
    QString m_inputProblem;
    QUrl m_inputProblemLink;
    QList<PeerInfo> m_trusted;
    ProtocolAlerts m_protocolAlerts;
    QList<PeerInfo> m_nearby;
    QList<PeerInfo> m_forgotten;
    QHash<QString, QString> m_pairTrust; // the level chosen in pair(), shown with the PIN prompt
    QList<DeviceInfo> m_devices;
    QString m_masterId;
    QString m_activeId;
    bool m_sessionRunning = false;
    QVariantMap m_files;
    QString m_screenViewerName;
    QString m_selfVersion;
    QVariantMap m_updates;
    QList<TransferInfo> m_transfers;
    // How long an ended transfer stays in the progress window.
    static constexpr int kEndedShownMs = 6000;
    QList<TransferInfo> m_ended; // ended transfers for the progress window, oldest first
    // transferBusy: looked at again at each transfer report, and every second while it is true
    // (a paste's reads end with no report).
    static constexpr int kPasteQuietMs = 3000;
    void updateTransferBusy();
    bool m_transferBusy = false;
    QTimer m_transferBusyTimer;
    // The rate of each transfer in progress, measured from its progress reports.
    struct RateSample {
        qint64 at = -1; // ms (m_clock) of the last sample used
        double done = 0;
        double rate = 0; // bytes per second, moving average (0: not known yet)
    };
    QHash<qint64, RateSample> m_rates;
    QElapsedTimer m_clock;
    QByteArray m_waitingKid;
    int m_waitingCount = 0;
    QString m_waitingFrom;
    QString m_waitingError;
    bool m_receiving = false;
    int m_thumbRevision = 0;
    bool m_keyDiagActive = false;
    double m_keyDiagEndsAt = 0;
    QString m_keyDiagBy;
    // The lines that are never dropped (where each side runs), then the newest ones; the text
    // shown is rebuilt at most every kKeyDiagTextMs while lines come in quickly.
    static constexpr int kKeyDiagMaxLines = 2000;
    static constexpr int kKeyDiagTextMs = 200;
    QStringList m_keyDiagHeader;
    QStringList m_keyDiagLines;
    int m_keyDiagDropped = 0;
    QString m_keyDiagText;
    QTimer m_keyDiagTextTimer;
    void updateKeyDiagText();
};
