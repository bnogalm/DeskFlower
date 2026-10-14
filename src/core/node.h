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
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <map>
#include <memory>

namespace df {

struct PeerView {
    QByteArray id;
    QString name;
    QString os;
    QString fingerprint;
    QHostAddress address;
    quint16 port = 0;
    bool trusted = false;
    bool connected = false;
    bool exploring = false;
    bool allowView = false; // trusted peer may view this screen without asking
    // From the authenticated hello (connected trusted peers only).
    QString version;        // Deskflower version it runs
    QString platform;       // build::platform()
    QString updateVersion;  // signed update package it can serve (empty: none)
    QString updatePlatform;
};

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
    };

    explicit Node(Options options, QObject *parent = nullptr);
    ~Node() override;

    bool start(QString *error);

    QByteArray id() const { return m_identity.id(); }
    QString name() const { return m_identity.name(); }
    QString fingerprint() const { return m_identity.fingerprint(); }
    quint16 dataPort() const { return m_socket.localPort(); }
    quint16 tcpPort() const { return m_tcp.serverPort(); }
    bool setName(const QString &name);

    bool isExploring() const { return m_exploring; }
    void setExploring(bool on);

    QList<PeerView> peers() const;
    bool isConnected(const QByteArray &peerId) const;
    bool isTrusted(const QByteArray &peerId) const { return m_store.contains(peerId); }

    // Local grant letting a trusted peer view this screen without a prompt (persisted).
    bool isViewAllowed(const QByteArray &peerId) const;
    void setViewAllowed(const QByteArray &peerId, bool on);

    // Announces the signed update package this device serves to paired peers (in the hello).
    void setUpdateOffer(const QString &version, const QString &platform);

    // Asks a host directly (when multicast/broadcast discovery does not reach it).
    void addManualPeer(const QHostAddress &addr, quint16 port);

    bool requestPairing(const QByteArray &peerId);
    bool submitPin(const QByteArray &peerId, const QString &pin);
    void cancelPairing(const QByteArray &peerId);
    void unpair(const QByteArray &peerId);

    // Reliable, ordered, encrypted. Returns false if the peer is not trusted.
    bool sendMessage(const QByteArray &peerId, const QCborMap &message);
    // Unreliable, encrypted, latest-wins traffic (mouse moves). Dropped if not connected.
    bool sendDatagram(const QByteArray &peerId, const QByteArray &data);

    // Encrypted TCP stream to a trusted peer for bulk data (same port number as UDP).
    // The caller owns the returned stream; nullptr if the peer is unknown.
    SecureStream *openStream(const QByteArray &peerId);

signals:
    void peersChanged();
    void explorationChanged(bool exploring);
    void peerConnected(const QByteArray &peerId);
    void peerDisconnected(const QByteArray &peerId);
    void messageReceived(const QByteArray &peerId, const QCborMap &message);
    void datagramReceived(const QByteArray &peerId, const QByteArray &data);
    // A trusted peer opened a stream to us (already authenticated). It deletes itself when closed.
    void streamAccepted(df::SecureStream *stream);
    void pairingPinGenerated(const QByteArray &peerId, const QString &peerName, const QString &pin);
    void pairingPinRequired(const QByteArray &peerId, const QString &peerName);
    void pairingFinished(const QByteArray &peerId, bool ok, const QString &error);

private:
    struct Peer;

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

    void sendBeacons();
    QByteArray beaconPacket(bool query) const;
    void initiateHandshake(Peer &peer);
    void sendInner(Peer &peer, const QByteArray &inner);
    void setConnected(Peer &peer, bool connected);
    void dropSessions(Peer &peer);
    void setPeerAddress(Peer &peer, const QHostAddress &addr, quint16 port);
    quint32 allocateIndex(const QByteArray &peerId);
    void releaseIndex(const std::unique_ptr<SecureSession> &session);
    Peer &peerFor(const QByteArray &id);
    QCborMap helloMessage() const;

    Options m_options;
    Identity m_identity;
    PeerStore m_store;
    QUdpSocket m_socket;
    QUdpSocket m_discovery;
    QTcpServer m_tcp;
    QTimer m_tick;
    std::unique_ptr<PairingManager> m_pairing;
    std::map<QByteArray, std::unique_ptr<Peer>> m_peers;
    std::map<quint32, QByteArray> m_indexToPeer;
    QByteArray m_bootId;
    bool m_exploring = false;
    qint64 m_explorationStarted = 0;
    qint64 m_lastBeacon = -1000000;
    QString m_updateVersion;
    QString m_updatePlatform;
};

} // namespace df
