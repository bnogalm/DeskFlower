#pragma once

// A Node is one Deskflower endpoint: it owns the identity, the UDP sockets, discovery, pairing and
// the encrypted sessions with every trusted peer. Higher layers (input, clipboard, layout) only use
// sendMessage()/sendDatagram() and the corresponding signals.

#include "pairing.h"
#include "reliable.h"
#include "securestream.h"
#include "session.h"
#include "store.h"

#include <QCborMap>
#include <QHash>
#include <QSet>
#include <QHostAddress>
#include <QObject>
#include <QStringList>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace df {

struct PeerView {
    QByteArray id;
    QString name;
    QString os;
    QString fingerprint;
    QHostAddress address; // where its packets come from now (or were last authenticated)
    quint16 port = 0;
    // Connected through a router that translates addresses (NAT): its packets come from an
    // address that is none of those its hello lists for its own interfaces.
    bool viaNat = false;
    bool trusted = false;
    bool connected = false;
    bool exploring = false;
    bool allowView = false; // trusted peer may view this screen without asking
    TrustLevel trust = TrustLevel::Device; // the level this device gives the peer
    // The stricter of both sides once the peer declared its level (its hello, kept until the
    // pairing ends); `trust` until then, so a device that has not connected since the start is
    // not shown as having chosen less.
    TrustLevel effectiveTrust = TrustLevel::Device;
    // The level the peer declared for this device in its hello (none before it): Guest means it
    // refuses this device's keyboard and mouse and layout edits.
    std::optional<TrustLevel> declaredTrust;
    bool otherUser = false; // paired by another user of this device: unusable now (Node::setLocalUser)
    // From the authenticated hello (connected trusted peers only).
    QString version;        // Deskflower version it runs
    QString platform;       // build::platform()
    QString updateVersion;  // signed update package it can serve (empty: none)
    QString updatePlatform;
    // Optional features it offers (Node::setCapability); capabilitiesKnown is false for versions
    // that send no list (before 0.1.22), whose features follow from version and platform.
    QStringList capabilities;
    bool capabilitiesKnown = false;
    // Its protocol (wire::kProtocolVersion: "pv" in its hello, or implied by its version for
    // versions that advertise none); 0 before its first hello.
    int protocol = 0;
};

// Capability: the device's agent runs a native drag of files from other devices (FileDrag
// hover); without it, files dragged onto the device are saved into its Received folder.
inline constexpr const char *kDragCapability = "dnd";

class Node : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QString configDir;
        quint16 dataPort = wire::kDefaultDataPort; // falls back to an ephemeral port if busy
        quint16 discoveryPort = wire::kDiscoveryPort;
        bool enableDiscovery = true;
        int explorationTimeoutMs = 10 * 60 * 1000;
        int pinTimeoutMs = PairingManager::kPinTimeoutMs; // how long a pairing waits for the PIN
        // Local address the UDP sockets and the TCP server bind to (tests give each node its own
        // 127.0.0.x). Null: every interface, unless DESKFLOWER_BIND_ADDRESS names one (the test
        // suite sets 127.0.0.1). Any other than a wildcard is also the only address the hello
        // reports.
        QHostAddress bindAddress;
        // Tests (simulated networks): when set, every datagram this node sends goes first to
        // `datagramHook`, which may take it (true: dropped, or handed to a simulated router;
        // injectDatagram() is the way back in) or leave it to the socket (false); and a stream
        // connects to an endpoint only when `connectHook` allows it (otherwise the attempt fails
        // at once).
        std::function<bool(const QByteArray &datagram, const QHostAddress &to, quint16 port)> datagramHook;
        std::function<bool(const QHostAddress &to, quint16 port)> connectHook;
    };

    explicit Node(Options options, QObject *parent = nullptr);
    ~Node() override;

    bool start(QString *error);

    QByteArray id() const { return m_identity.id(); }
    QString name() const { return m_identity.name(); }
    QString fingerprint() const { return m_identity.fingerprint(); }
    quint16 dataPort() const { return m_socket.localPort(); }
    quint16 tcpPort() const { return m_tcp.serverPort(); }
    quint16 discoveryPort() const { return m_options.discoveryPort; }
    bool setName(const QString &name);

    bool isExploring() const { return m_exploring; }
    void setExploring(bool on);

    QList<PeerView> peers() const;
    bool isConnected(const QByteArray &peerId) const;
    bool isTrusted(const QByteArray &peerId) const { return m_store.contains(peerId); }

    // Devices with several users (the Windows service): a pairing belongs to the user signed in
    // when it was made, and the peer is only reachable (sessions, messages, streams) while that
    // user is the one signed in here. `user` identifies who is signed in now (empty: nobody).
    // Called by the Windows service (src/service/main.cpp) on every session change; never on
    // platforms that run one service per user.
    void setLocalUser(const QString &user);

    // Local grant letting a trusted peer view this screen without a prompt (persisted). Given by
    // the prompt ("Always allow") or in advance by the local user.
    bool isViewAllowed(const QByteArray &peerId) const;
    void setViewAllowed(const QByteArray &peerId, bool on);
    // Remote view at all ("remoteView" setting, default on). Off: nobody may view this screen
    // (isViewAllowed is false for everyone) and this device views nobody's; grants are kept.
    void setRemoteViewEnabled(bool on);
    bool isRemoteViewEnabled() const { return m_remoteViewEnabled; }

    // Trust levels (TrustLevel): what this device lets a paired peer do here. Each side chooses
    // its own level for the other and declares it in the authenticated hello; what the pair
    // shares is the stricter of the two (effectiveTrust).
    //
    // Guests: paired devices this one does not fully trust. A guest's keyboard and mouse are never
    // obeyed and it never views this screen (no grant, no thumbnails); this device's clipboard
    // and copied files reach it only when the local user pastes on it (ClipboardSync).
    //
    // Group: the user's own devices. When both sides share Group, each tells the other about its
    // group members (public key, name, last address) and tells those members about it, so every
    // member ends up trusting every other as Group without a PIN. Only members this device
    // stores as Group are introduced, and introductions are not forwarded: a device learns only
    // the members of the device it paired with (and of those it is introduced to, since they
    // introduce it in turn). An introduction never changes a record that exists already (a PIN
    // pairing wins, a guest is never promoted) and never brings back a device the user forgot.
    TrustLevel trustLevel(const QByteArray &peerId) const; // Device when not trusted
    void setTrust(const QByteArray &peerId, TrustLevel level);
    // The stricter of this device's level and the one the peer declared (Device before its
    // first hello, as for versions that know no levels).
    TrustLevel effectiveTrust(const QByteArray &peerId) const;
    // The level the peer declared for this device in its hello; none before the first hello of
    // the pairing. A peer that declared Guest refuses this device's keyboard and mouse.
    std::optional<TrustLevel> declaredTrust(const QByteArray &peerId) const;
    bool isGuest(const QByteArray &peerId) const;

    // Announces the signed update package this device serves to paired peers (in the hello).
    void setUpdateOffer(const QString &version, const QString &platform);
    // Optional features this device offers right now (in the hello, "cap"; a change resends it):
    // kDragCapability. Each peer's list is in PeerView::capabilities.
    void setCapability(const QString &name, bool on);
    QStringList capabilities() const { return m_capabilities; }

    // A peer's protocol (PeerView::protocol): 0 while unknown (no hello yet); this device's own
    // for itself.
    int peerProtocol(const QByteArray &peerId) const;
    // Whether bulk transfers (FileTransfer) may be used with `peerId`: itself, a peer whose
    // protocol is still unknown (its hello is on the way: a transfer it cannot read is refused
    // on the wire anyway), or one with this device's protocol.
    bool transfersCompatible(const QByteArray &peerId) const;
    // For tests: changes every hello this device sends (pretending to be an older version).
    void setHelloOverrideForTests(std::function<void(QCborMap &)> tweak) { m_helloOverride = std::move(tweak); }

    // Asks a host directly (when multicast/broadcast discovery does not reach it). The host is
    // asked again every 30 s until it answers; once a paired device answers from there, it is
    // kept as that device's Configured endpoint (persisted, tried while it is not connected).
    void addManualPeer(const QHostAddress &addr, quint16 port);

    // level: what the device becomes here once paired. It goes to the peer as a suggestion
    // (PairingManager, `tl`), so that its PIN dialog offers the same level.
    bool requestPairing(const QByteArray &peerId, TrustLevel level = TrustLevel::Device);
    // Answering side: what the pairing with `peerId` makes it (the PIN dialog's choice). Taken
    // only while a pairing with that device is under way (false otherwise), so that a late or
    // stray choice is never applied silently to some later pairing. Not chosen: the requester's
    // suggestion, except that a device already paired keeps its level when that is Guest (a
    // guest is never promoted by the other side's wish) and never gets less than it had, and
    // that a Group suggestion is not taken up unless the device is a member already: joining
    // the group lets it introduce devices to every member, so it takes the user's own choice
    // (the dialog offers Device and points at the suggestion, pairingSuggestion). Without a
    // suggestion, the level it has already, otherwise Device.
    bool setPairTrust(const QByteArray &peerId, TrustLevel level);
    // Answering side: what the requester suggested for the pairing under way with `peerId`, when
    // that is more than the pairing applies by itself (a Group suggestion awaiting the user).
    std::optional<TrustLevel> pairingSuggestion(const QByteArray &peerId) const;
    // What the pairing under way with `peerId` (either side) will make it: the level chosen
    // for it, else the one it has already (when it is this user's), else Device.
    TrustLevel pairingTrust(const QByteArray &peerId) const;
    bool submitPin(const QByteArray &peerId, const QString &pin);
    void cancelPairing(const QByteArray &peerId);
    // Forget: the device is removed here and told to remove this one, and group members cannot
    // introduce it again until it is paired with a PIN or allowed again (unforget).
    void unpair(const QByteArray &peerId);
    // Devices forgotten here (id -> name when known), which group members cannot introduce.
    QHash<QByteArray, QString> forgottenPeers() const { return m_store.forgotten(); }
    // "Allow again": group members may introduce the device once more. The connected members are
    // sent a hello, which makes them list their members again (sendGroupState).
    void unforget(const QByteArray &peerId);

    // Reliable, ordered, encrypted. Returns false if the peer is not trusted.
    bool sendMessage(const QByteArray &peerId, const QCborMap &message);
    // Unreliable, encrypted, latest-wins traffic (mouse moves). Dropped if not connected.
    bool sendDatagram(const QByteArray &peerId, const QByteArray &data);

    // Encrypted stream to a trusted peer for bulk data. The caller owns the returned stream, which
    // emits `established` once one path reached the peer, or `closed` when none did; nullptr if
    // the peer is unknown or there is no way to try. The paths, raced (see README, NAT):
    //   - TCP to every endpoint known for it (its port from the hello), one more every 250 ms;
    //   - over the session, the peer is asked to dial back (a TCP connection from it to this
    //     device, carrying a one-time nonce), which gets through a NAT the other way round;
    //   - after 5 s without either, a tunnel of the stream's frames through the UDP session.
    SecureStream *openStream(const QByteArray &peerId);

    // Tests: hands a datagram to the node as if its socket had received it (Options::datagramHook).
    void injectDatagram(const QByteArray &datagram, const QHostAddress &from, quint16 port);
    // Unicast IPv4 addresses of this device's interfaces (what the hello reports).
    QList<QHostAddress> localAddresses() const { return m_localAddresses; }

signals:
    void peersChanged();
    void explorationChanged(bool exploring);
    void peerConnected(const QByteArray &peerId);
    void peerDisconnected(const QByteArray &peerId);
    // A peer's protocol (PeerView::protocol) became known or changed (peersChanged follows too).
    void peerProtocolChanged(const QByteArray &peerId);
    void messageReceived(const QByteArray &peerId, const QCborMap &message);
    void datagramReceived(const QByteArray &peerId, const QByteArray &data);
    // A trusted peer opened a stream to us (already authenticated). It deletes itself when closed.
    void streamAccepted(df::SecureStream *stream);
    void pairingPinGenerated(const QByteArray &peerId, const QString &peerName, const QString &pin);
    void pairingPinRequired(const QByteArray &peerId, const QString &peerName);
    void pairingFinished(const QByteArray &peerId, bool ok, const QString &error);
    // A paired device could not open a TCP connection to `address`:`port` (this device's own
    // address) and had to take a slower path: a firewall here likely refuses connections from
    // its network.
    void incomingBlocked(const QByteArray &peerId, const QHostAddress &address, quint16 port);
    // After a pairing, the peer's first hello declared less than this device chose for it
    // (`chosen`): the pair shares `declared` (e.g. Group was chosen here, Device there).
    void pairingLevelDiffers(const QByteArray &peerId, df::TrustLevel chosen, df::TrustLevel declared);
    // Group member `introducer` introduced `id`, which the user forgot here: not taken. Once per
    // device and run, so that every hello of the member does not repeat it.
    void forgottenIntroductionSkipped(const QByteArray &introducer, const QByteArray &id, const QString &name);

private:
    struct Peer;
    struct Endpoint;
    struct OutgoingStream;
    friend class TunnelTransport;

    void onDataReadyRead();
    void onDiscoveryReadyRead();
    void onTick();
    void handlePacket(const QByteArray &packet, const QHostAddress &from, quint16 port, bool viaDataSocket);
    void handleBeacon(const QCborMap &msg, const QHostAddress &from, quint16 port, bool viaDataSocket);
    void handleHandshakeInit(const QByteArray &packet, const QHostAddress &from, quint16 port);
    void handleHandshakeResp(const QByteArray &packet, const QHostAddress &from, quint16 port);
    void handleData(const QByteArray &packet, const QHostAddress &from, quint16 port);
    void handleInner(Peer &peer, const QByteArray &inner);
    void handleControlMessage(Peer &peer, const QCborMap &msg);
    void onPairingSucceeded(const TrustedPeer &trusted, const QHostAddress &addr, quint16 port);

    // Group introductions (see setTrust).
    void sendGroupState(Peer &peer);
    void handleGroupEntries(const Peer &from, const QList<QCborMap> &entries);
    QCborMap groupEntry(const QByteArray &id) const;
    // Removes the records `introducer` introduced that never connected (it was forgotten or demoted).
    void dropIntroducedBy(const QByteArray &introducer);

    void sendBeacons();
    QByteArray beaconPacket(bool query) const;
    void initiateHandshake(Peer &peer);
    void sendInner(Peer &peer, const QByteArray &inner);
    void setConnected(Peer &peer, bool connected);
    void dropSessions(Peer &peer);
    void setPeerAddress(Peer &peer, const QHostAddress &addr, quint16 port);
    void followPeer(Peer &peer, const QHostAddress &addr, quint16 port);

    // Endpoints (see Peer::endpoints).
    Endpoint *findEndpoint(Peer &peer, const QHostAddress &addr, quint16 port);
    Endpoint *addEndpoint(Peer &peer, const QHostAddress &addr, quint16 port, quint16 tcpPort,
                          EndpointSource source);
    void saveEndpoints(const Peer &peer);
    void loadEndpoints(Peer &peer, const TrustedPeer &stored);
    bool hasSteadyEndpoint(const Peer &peer) const;
    bool isSelfAddress(const Peer &peer, const QHostAddress &addr) const;
    bool isViaNat(const Peer &peer) const;
    bool isOnLocalSubnet(const QHostAddress &addr) const;
    bool isThisDevice(const QHostAddress &addr, quint16 port, bool tcp) const;
    void refreshLocalAddresses(bool initial);
    void onNetworkChanged();
    void probeDirectPath(Peer &peer, qint64 now);
    void sendHandshakeTo(Peer &peer, const QHostAddress &addr, quint16 port);
    void abandonHandshake(Peer &peer);
    void matchManualHost(const QByteArray &id, const QHostAddress &from, quint16 port);
    void writeDatagram(const QByteArray &datagram, const QHostAddress &addr, quint16 port);

    // Streams (openStream): the paths raced for one, dial-backs and tunnels.
    struct StreamTarget {
        QHostAddress addr;
        quint16 port = 0;
        // Where its packets come from, which is one of its own addresses (no router between):
        // failing to reach it there is worth telling the peer (incomingBlocked).
        bool direct = false;
    };
    std::vector<StreamTarget> streamTargets(const Peer &peer, bool dialBack = false) const;
    SecureStream *startOutgoing(Peer &peer, const QByteArray &dialNonce);
    void stepOutgoing(SecureStream *proxy);
    void startAttempt(SecureStream *proxy, SecureStream *attempt, const QString &path, const StreamTarget &target);
    void finishOutgoing(SecureStream *proxy, SecureStream *winner, const QString &path);
    void failOutgoing(SecureStream *proxy);
    void handleDialRequest(Peer &peer, const QCborMap &msg);
    void handleDialBack(SecureStream *stream, const QByteArray &nonce);
    void watchAccepted(SecureStream *stream, const QHostAddress &address, bool tcp);
    void handleTunnelMessage(Peer &peer, const QByteArray &message);
    void sendTunnel(const QByteArray &peerId, quint8 op, quint64 key, const QByteArray &payload);
    qint64 tunnelBacklog(const QByteArray &peerId) const;
    void dropTunnel(const QByteArray &peerId, quint64 key);
    void loseTunnels(Peer &peer, const QString &reason);
    // Takes the boot id a new session announced; true if it differs from the previous one (the peer
    // restarted: its reliable channel and tunnels start over).
    bool adoptRemoteBoot(Peer &peer, const QByteArray &bootId);
    quint32 allocateIndex(const QByteArray &peerId);
    void releaseIndex(const std::unique_ptr<SecureSession> &session);
    Peer &peerFor(const QByteArray &id);
    QCborMap helloMessage(const QByteArray &peerId) const;

    Options m_options;
    Identity m_identity;
    PeerStore m_store;
    QUdpSocket m_socket;
    quint64 m_failedSends = 0;      // diagnostics
    quint64 m_rejectedPackets = 0;
    QUdpSocket m_discovery;
    QTcpServer m_tcp;
    QTimer m_tick;
    std::unique_ptr<PairingManager> m_pairing;
    std::map<QByteArray, std::unique_ptr<Peer>> m_peers;
    std::map<quint32, QByteArray> m_indexToPeer;
    // Pairings tied to users (setLocalUser).
    bool isUsable(const QByteArray &peerId) const;
    bool isViewGranted(const QByteArray &peerId) const; // the stored grant, remote view on or off
    void adoptUnownedPeers();

    // Unauthenticated traffic is rate limited, so that a flood costs little CPU and memory.
    struct RateLimit {
        double perSecond;
        double burst;
        double tokens;
        qint64 last = 0;
        RateLimit(double perSecond, double burst) : perSecond(perSecond), burst(burst), tokens(burst) {}
        bool take(qint64 nowMs);
    };
    RateLimit m_beaconLimit;
    RateLimit m_handshakeLimit;
    RateLimit m_responseLimit; // handshake responses that reach a pending initiation
    RateLimit m_pairingLimit;
    QHash<std::pair<QHostAddress, quint16>, qint64> m_queryAnswered; // beacon queries answered, by sender
    // Accepted TCP connections not authenticated yet, oldest first (see start()).
    struct PendingStream {
        SecureStream *stream;
        QHostAddress address;
    };
    std::vector<PendingStream> m_pendingStreams;
    std::map<QByteArray, int> m_acceptedStreams;     // authenticated, by peer
    quint64 m_handshakeCounter = 0;                  // grows with every initiation we send

    QHash<QByteArray, TrustLevel> m_pairTrust; // what the pairings under way make the peer
    QHash<QByteArray, TrustLevel> m_pairSuggested; // answering side: the requester's suggestion
    QHash<QByteArray, TrustLevel> m_levelCheck; // paired, first hello awaited: the level chosen
    QSet<QByteArray> m_skippedNoticed;         // forgotten devices whose introduction was reported
    bool m_usersSeparate = false;
    bool m_remoteViewEnabled = true;
    QString m_localUser;
    QByteArray m_bootId;
    bool m_exploring = false;
    qint64 m_explorationStarted = 0;
    qint64 m_lastBeacon = -1000000;
    QString m_updateVersion;
    QString m_updatePlatform;
    QStringList m_capabilities;
    std::function<void(QCborMap &)> m_helloOverride; // tests
    // This device's interfaces (the hello's self-reported addresses; NAT and subnet checks).
    QList<QHostAddress> m_localAddresses;
    QList<std::pair<QHostAddress, int>> m_localSubnets;
    qint64 m_lastInterfaceCheck = 0;
    // Hosts typed by the user (addManualPeer): asked again until a device answers from there.
    struct ManualHost {
        QHostAddress addr;
        quint16 port = 0;
        qint64 lastQuery = 0;
        QByteArray peerId; // the device that answered from there, once one did
    };
    std::vector<ManualHost> m_manualHosts;
    std::map<SecureStream *, std::unique_ptr<OutgoingStream>> m_outgoing;
    // Dial-backs this device asked for (openStream): nonce -> the pending stream it completes.
    struct DialWait {
        QByteArray peerId;
        QPointer<SecureStream> proxy;
        qint64 expires = 0;
    };
    QHash<QByteArray, DialWait> m_dialWaits;
};

} // namespace df
