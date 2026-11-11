#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QCborMap;

// The service's "state" event as typed values for QML (Backend's lists), so that the bindings
// reading them compile to C++. Fields keep the service's names; a field an older service leaves
// out reads as empty, false or 0, which the QML tests as it would test a missing one. Equality
// compares every field, so that a QML view of a list (a Repeater) is rebuilt only when its data
// changes.

// A paired or nearby device (state "peers"; only id and name for "forgotten").
class PeerInfo
{
    Q_GADGET
    QML_VALUE_TYPE(peerInfo)

    Q_PROPERTY(QString id MEMBER id FINAL)
    Q_PROPERTY(QString name MEMBER name FINAL)
    Q_PROPERTY(QString os MEMBER os FINAL)
    Q_PROPERTY(QString fingerprint MEMBER fingerprint FINAL)
    // The endpoint in use (through NAT, the router's), empty when not known.
    Q_PROPERTY(QString address MEMBER address FINAL)
    Q_PROPERTY(int port MEMBER port FINAL)
    Q_PROPERTY(bool viaNat MEMBER viaNat FINAL)
    Q_PROPERTY(bool trusted MEMBER trusted FINAL)
    Q_PROPERTY(bool connected MEMBER connected FINAL)
    Q_PROPERTY(bool exploring MEMBER exploring FINAL)
    Q_PROPERTY(bool allowView MEMBER allowView FINAL)
    // Trust levels ("guest", "device", "group"): given here, the stricter of both sides, and what
    // it declared for this device ("" before its first hello).
    Q_PROPERTY(QString trust MEMBER trust FINAL)
    Q_PROPERTY(QString effectiveTrust MEMBER effectiveTrust FINAL)
    Q_PROPERTY(QString declaredTrust MEMBER declaredTrust FINAL)
    Q_PROPERTY(QString version MEMBER version FINAL)
    // A newer version it can send us (empty: none).
    Q_PROPERTY(QString update MEMBER update FINAL)
    Q_PROPERTY(bool swapCtrlCmd MEMBER swapCtrlCmd FINAL)
    // Its transfer protocol (0: not known yet); "older" or "newer" when it differs from this
    // device's (a connected paired device: file transfers with it are off until one is updated),
    // and whether this device's update package would update it.
    Q_PROPERTY(int protocol MEMBER protocol FINAL)
    Q_PROPERTY(QString protocolMismatch MEMBER protocolMismatch FINAL)
    Q_PROPERTY(bool canServeUpdate MEMBER canServeUpdate FINAL)
    // The start of id, shown for a device without a name.
    Q_PROPERTY(QString shortId READ shortId FINAL)

public:
    static PeerInfo fromCbor(const QCborMap &map);
    friend bool operator==(const PeerInfo &, const PeerInfo &) = default;

    QString shortId() const { return id.left(8); }

    QString id;
    QString name;
    QString os;
    QString fingerprint;
    QString address;
    int port = 0;
    bool viaNat = false;
    bool trusted = false;
    bool connected = false;
    bool exploring = false;
    bool allowView = false;
    QString trust;
    QString effectiveTrust;
    QString declaredTrust;
    QString version;
    QString update;
    bool swapCtrlCmd = false;
    int protocol = 0;
    QString protocolMismatch;
    bool canServeUpdate = false;
};

// The paired devices of another protocol (PeerInfo::protocolMismatch) the user has not been told
// of yet: each device and protocol once per run of the agent (a tray notification), however
// often the state repeats it.
class ProtocolAlerts
{
public:
    // The devices of `peers` whose mismatch is new.
    QList<PeerInfo> fresh(const QList<PeerInfo> &peers);

private:
    QSet<QString> m_told; // "<id>/<protocol>"
};

// One monitor of a device (ScreenInfo::toCbor's short names), with the key of its thumbnail.
class DeviceScreen
{
    Q_GADGET
    QML_VALUE_TYPE(deviceScreen)

    Q_PROPERTY(QString i MEMBER i FINAL)       // id
    Q_PROPERTY(QString n MEMBER n FINAL)       // name
    Q_PROPERTY(int x MEMBER x FINAL)           // geometry in the device's own desktop
    Q_PROPERTY(int y MEMBER y FINAL)
    Q_PROPERTY(int w MEMBER w FINAL)
    Q_PROPERTY(int h MEMBER h FINAL)
    Q_PROPERTY(double s MEMBER s FINAL)        // scale
    Q_PROPERTY(bool p MEMBER p FINAL)          // primary
    // "image://thumbs/<thumbKey>" (Backend::thumbKey).
    Q_PROPERTY(QString thumbKey MEMBER thumbKey FINAL)

public:
    static DeviceScreen fromCbor(const QCborMap &map);
    friend bool operator==(const DeviceScreen &, const DeviceScreen &) = default;

    QString i;
    QString n;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    double s = 0;
    bool p = false;
    QString thumbKey;
};

// A device of the workspace (state "workspace" "devices"): its pack in the shared plane.
class DeviceInfo
{
    Q_GADGET
    QML_VALUE_TYPE(deviceInfo)

    Q_PROPERTY(QString id MEMBER id FINAL)
    Q_PROPERTY(QString name MEMBER name FINAL)
    Q_PROPERTY(bool self MEMBER self FINAL)
    Q_PROPERTY(bool connected MEMBER connected FINAL)
    Q_PROPERTY(bool remoteView MEMBER remoteView FINAL)
    // Guests either way: its level here ("" for this device), and whether it treats us as one.
    Q_PROPERTY(QString trust MEMBER trust FINAL)
    Q_PROPERTY(bool guestThere MEMBER guestThere FINAL)
    // Remote screen id -> local screen id or "-" (not shown); empty: every screen where the edge
    // is crossed. Read one entry with placement().
    Q_PROPERTY(QVariantMap viewPlan MEMBER viewPlan FINAL)
    Q_PROPERTY(bool thumbsDenied MEMBER thumbsDenied FINAL)
    // Pack offset in the shared plane.
    Q_PROPERTY(int x MEMBER x FINAL)
    Q_PROPERTY(int y MEMBER y FINAL)
    Q_PROPERTY(QList<DeviceScreen> screens MEMBER screens FINAL)
    // The rectangle around its screens, in its own desktop (infinite extents without screens).
    Q_PROPERTY(QRectF bounds READ bounds FINAL)

public:
    static DeviceInfo fromCbor(const QCborMap &map);
    friend bool operator==(const DeviceInfo &, const DeviceInfo &) = default;

    QRectF bounds() const;

    // Lookups in `screens` for QML, which cannot loop over them in compiled code. A screen
    // that is not there reads as an empty one.
    Q_INVOKABLE QStringList screenIds() const;
    Q_INVOKABLE DeviceScreen screen(const QString &screenId) const;
    // The view plan's entry for one remote screen: a local screen id, "-" or "" (none).
    Q_INVOKABLE QString placement(const QString &screenId) const { return viewPlan.value(screenId).toString(); }
    // The view plan per screen, in order, as the plan editor starts from it: a local screen that
    // is not among localScreenIds (gone) counts as "", as the service treats it.
    Q_INVOKABLE QStringList editablePlan(const QStringList &localScreenIds) const;

    QString id;
    QString name;
    bool self = false;
    bool connected = false;
    bool remoteView = false;
    QString trust;
    bool guestThere = false;
    QVariantMap viewPlan;
    bool thumbsDenied = false;
    int x = 0;
    int y = 0;
    QList<DeviceScreen> screens;
};

// A transfer that the user sees (the service's "transfer" events): in progress, or just ended
// (the progress window keeps it a few seconds).
class TransferInfo
{
    Q_GADGET
    QML_VALUE_TYPE(transferInfo)

    Q_PROPERTY(qint64 id MEMBER id FINAL)
    Q_PROPERTY(QString peer MEMBER peer FINAL)
    Q_PROPERTY(QString peerName MEMBER peerName FINAL)
    Q_PROPERTY(QString kind MEMBER kind FINAL)            // "clipboard", "drag" or "drop"
    Q_PROPERTY(QString state MEMBER state FINAL)          // "running", "done", "failed" or "cancelled"
    Q_PROPERTY(QString error MEMBER error FINAL)          // why it failed
    // Bytes (as double, what QML computes with; exact far beyond any transfer).
    Q_PROPERTY(double done MEMBER done FINAL)
    Q_PROPERTY(double total MEMBER total FINAL)
    Q_PROPERTY(int files MEMBER files FINAL)              // files received so far, of fileCount
    Q_PROPERTY(int fileCount MEMBER fileCount FINAL)
    Q_PROPERTY(QString target MEMBER target FINAL)        // the folder the files go into (native separators)
    Q_PROPERTY(bool cancellable MEMBER cancellable FINAL) // cancelTransfer stops it
    // Measured here: bytes per second (a moving average; 0 until known) and the time left at that
    // rate (-1: not known).
    Q_PROPERTY(double rateBytesPerSec MEMBER rateBytesPerSec FINAL)
    Q_PROPERTY(double secondsLeft MEMBER secondsLeft FINAL)
    // The transfer of a paste a file manager reads while it arrives (Explorer's own progress
    // dialog shows it): DeskFlower's progress window leaves it out.
    Q_PROPERTY(bool shellDriven MEMBER shellDriven FINAL)

public:
    static TransferInfo fromCbor(const QCborMap &map);
    friend bool operator==(const TransferInfo &, const TransferInfo &) = default;

    qint64 id = 0;
    QString peer;
    QString peerName;
    QString kind;
    QString state;
    QString error;
    double done = 0;
    double total = 0;
    int files = 0;
    int fileCount = 0;
    QString target;
    bool cancellable = false;
    double rateBytesPerSec = 0;
    double secondsLeft = -1;
    bool shellDriven = false;
};

// The network ports (state "ports"): configured (used from the service's next start) and in use
// now (0: not known, or not running for the phone).
class PortsInfo
{
    Q_GADGET
    QML_VALUE_TYPE(portsInfo)

    Q_PROPERTY(int data MEMBER data FINAL)
    Q_PROPERTY(int discovery MEMBER discovery FINAL)
    Q_PROPERTY(int phone MEMBER phone FINAL)
    Q_PROPERTY(int dataInUse MEMBER dataInUse FINAL)
    Q_PROPERTY(int tcpInUse MEMBER tcpInUse FINAL)
    Q_PROPERTY(int discoveryInUse MEMBER discoveryInUse FINAL)
    Q_PROPERTY(int phoneInUse MEMBER phoneInUse FINAL)

public:
    static PortsInfo fromCbor(const QCborMap &map);
    friend bool operator==(const PortsInfo &, const PortsInfo &) = default;

    int data = 0;
    int discovery = 0;
    int phone = 0;
    int dataInUse = 0;
    int tcpInUse = 0;
    int discoveryInUse = 0;
    int phoneInUse = 0;
};

// The lists of these types as QML sequences (list<peerInfo>, ...): length, elements and
// iteration for the QML that reads them.
struct PeerInfoList
{
    Q_GADGET
    QML_FOREIGN(QList<PeerInfo>)
    QML_ANONYMOUS
    QML_SEQUENTIAL_CONTAINER(PeerInfo)
};

struct DeviceScreenList
{
    Q_GADGET
    QML_FOREIGN(QList<DeviceScreen>)
    QML_ANONYMOUS
    QML_SEQUENTIAL_CONTAINER(DeviceScreen)
};

struct DeviceInfoList
{
    Q_GADGET
    QML_FOREIGN(QList<DeviceInfo>)
    QML_ANONYMOUS
    QML_SEQUENTIAL_CONTAINER(DeviceInfo)
};

struct TransferInfoList
{
    Q_GADGET
    QML_FOREIGN(QList<TransferInfo>)
    QML_ANONYMOUS
    QML_SEQUENTIAL_CONTAINER(TransferInfo)
};
