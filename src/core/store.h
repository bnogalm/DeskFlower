#pragma once

#include "crypto.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <optional>

namespace df {

// Long-term device identity and local settings, persisted in <configDir>/identity.json.
class Identity
{
public:
    bool loadOrCreate(const QString &configDir, QString *error);
    bool setName(const QString &name);

    const crypto::KeyPair &keys() const { return m_keys; }
    QByteArray id() const { return m_id; }
    QString name() const { return m_name; }
    QString fingerprint() const { return crypto::fingerprint(m_keys.publicKey); }

private:
    bool save() const;

    QString m_path;
    crypto::KeyPair m_keys;
    QByteArray m_id;
    QString m_name;
};

// How far this device trusts a paired one, chosen here (the peer chooses its own level for us).
// Ordered: the stricter level has the lower value, so the level two devices share is the minimum.
//   Guest:  cannot drive this device's keyboard and mouse or view its screens, and gets the
//           clipboard only when this device's user pastes on it.
//   Device: one of the user's devices, trusted by this one only (a PIN per pair).
//   Group:  one of the user's devices, trusted by every device of the group: group members
//           introduce each other, so one PIN pairs a device with all of them (Node).
enum class TrustLevel { Guest, Device, Group };

inline QString trustName(TrustLevel level)
{
    switch (level) {
    case TrustLevel::Guest:
        return QStringLiteral("guest");
    case TrustLevel::Group:
        return QStringLiteral("group");
    case TrustLevel::Device:
        break;
    }
    return QStringLiteral("device");
}

// "guest" / "device" / "group"; anything else is `fallback` (an absent field, an older version).
inline TrustLevel trustFromName(const QString &name, TrustLevel fallback = TrustLevel::Device)
{
    if (name == QLatin1String("guest"))
        return TrustLevel::Guest;
    if (name == QLatin1String("group"))
        return TrustLevel::Group;
    if (name == QLatin1String("device"))
        return TrustLevel::Device;
    return fallback;
}

// Where a paired device's endpoints come from, in the order they are preferred:
//   Configured:   typed by the user (addPeer) and answered by that device.
//   SelfReported: an address of its own interfaces, from its authenticated hello.
//   Observed:     where its authenticated packets came from (behind a NAT: the router's mapping).
//   GroupHint:    told by a group member.
//   Beacon:       claimed by an unauthenticated beacon (never stored).
enum class EndpointSource { Configured, SelfReported, Observed, GroupHint, Beacon };

inline QString endpointSourceName(EndpointSource source)
{
    switch (source) {
    case EndpointSource::Configured:
        return QStringLiteral("configured");
    case EndpointSource::SelfReported:
        return QStringLiteral("self");
    case EndpointSource::Observed:
        return QStringLiteral("observed");
    case EndpointSource::GroupHint:
        return QStringLiteral("group");
    case EndpointSource::Beacon:
        break;
    }
    return QStringLiteral("beacon");
}

inline std::optional<EndpointSource> endpointSourceFromName(const QString &name)
{
    for (EndpointSource s : {EndpointSource::Configured, EndpointSource::SelfReported, EndpointSource::Observed,
                             EndpointSource::GroupHint, EndpointSource::Beacon}) {
        if (endpointSourceName(s) == name)
            return s;
    }
    return std::nullopt;
}

// An endpoint kept across restarts (Node persists a bounded list per device).
struct StoredEndpoint {
    QString address;
    quint16 port = 0;    // UDP
    quint16 tcpPort = 0; // 0: not known
    EndpointSource source = EndpointSource::Observed;
    qint64 lastOk = 0; // ms since the epoch of the last authenticated contact through it (0: never)

    bool operator==(const StoredEndpoint &) const = default;
};

struct TrustedPeer {
    QByteArray id;
    QByteArray publicKey;
    QString name;
    QDateTime pairedAt;
    bool allowView = false; // this device may show its screen to the peer without asking
    // Who paired it, when a device has several users (the Windows service: the console user's SID).
    // The peer only works while that user is signed in on this device. Empty: anyone (or not yet
    // known, for pairings made before this was recorded).
    QString owner;
    // Where it was last connected: tried at start, so a device that discovery cannot reach
    // (another network, added by IP) is found again after a restart.
    QString lastAddress;
    quint16 lastPort = 0;
    // Every way of reaching it known here (see EndpointSource), best first.
    QList<StoredEndpoint> endpoints;
    TrustLevel trust = TrustLevel::Device;
    // Group member that introduced it (empty: paired with a PIN). Kept so that what one member
    // introduces is bounded, and so that a device never heard from goes with its introducer.
    QByteArray introducedBy;
    bool everConnected = false; // a session with it was confirmed at least once

    bool isGuest() const { return trust == TrustLevel::Guest; }
};

// Peers accepted through PIN pairing (or introduced by a group member), persisted in
// <configDir>/peers.json as an array of {publicKey, name, pairedAt, allowView (only when true),
// owner (when known), address and port (where it was last connected, when known; what earlier
// versions read), addresses (the endpoints Node keeps for it, as {a, p, tp, src, ok}), guest (only
// for TrustLevel::Guest), group (only for TrustLevel::Group; neither flag: Device, so files
// written before groups existed read as before), introducedBy (the member that introduced it,
// only then), connected (only once a session was confirmed)}. Unpairing deletes the record and
// re-pairing replaces it, so a screen view grant never outlives the pairing.
//
// Forgotten devices are remembered apart, in <configDir>/forgotten.json (array of {id (base64),
// name}; a bare base64 id, as 0.1.20/0.1.21 wrote it, reads with no name), so that a group member
// cannot introduce them again until they are paired with a PIN once more or the user allows them
// again (unforget). A separate file because older builds take every entry of peers.json for a
// trusted peer.
class PeerStore
{
public:
    bool load(const QString &configDir);
    bool contains(const QByteArray &id) const { return m_peers.contains(id); }
    const TrustedPeer *find(const QByteArray &id) const;
    void upsert(const TrustedPeer &peer);
    void upsertAll(const QList<TrustedPeer> &peers); // one write for the lot
    void remove(const QByteArray &id);
    void rename(const QByteArray &id, const QString &name);
    void setAllowView(const QByteArray &id, bool on);
    void setOwner(const QByteArray &id, const QString &owner);
    void setLastAddress(const QByteArray &id, const QString &address, quint16 port);
    void setEndpoints(const QByteArray &id, const QList<StoredEndpoint> &endpoints);
    void setTrust(const QByteArray &id, TrustLevel level);
    void setConnectedOnce(const QByteArray &id);
    QList<TrustedPeer> all() const { return m_peers.values(); }
    // Records introduced by `introducer` (all of them, or only those never connected).
    QList<QByteArray> introducedBy(const QByteArray &introducer, bool neverConnected = false) const;
    int introducedCount() const; // records introduced by anyone

    // Tombstones of forgotten devices (see above).
    bool isForgotten(const QByteArray &id) const { return m_forgotten.contains(id); }
    void forget(const QByteArray &id, const QString &name);
    void unforget(const QByteArray &id);
    QHash<QByteArray, QString> forgotten() const { return m_forgotten; } // id -> name (maybe empty)

private:
    bool save() const;
    bool saveForgotten() const;

    QString m_path;
    QString m_forgottenPath;
    QHash<QByteArray, TrustedPeer> m_peers;
    QHash<QByteArray, QString> m_forgotten;
};

} // namespace df
