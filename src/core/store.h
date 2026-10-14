#pragma once

#include "crypto.h"

#include <QDateTime>
#include <QHash>
#include <QString>

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

struct TrustedPeer {
    QByteArray id;
    QByteArray publicKey;
    QString name;
    QDateTime pairedAt;
    bool allowView = false; // this device may show its screen to the peer without asking
};

// Peers accepted through PIN pairing, persisted in <configDir>/peers.json as an array of
// {publicKey, name, pairedAt, allowView (only when true)}. Unpairing deletes the record and
// re-pairing replaces it, so a screen view grant never outlives the pairing.
class PeerStore
{
public:
    bool load(const QString &configDir);
    bool contains(const QByteArray &id) const { return m_peers.contains(id); }
    const TrustedPeer *find(const QByteArray &id) const;
    void upsert(const TrustedPeer &peer);
    void remove(const QByteArray &id);
    void rename(const QByteArray &id, const QString &name);
    void setAllowView(const QByteArray &id, bool on);
    QList<TrustedPeer> all() const { return m_peers.values(); }

private:
    bool save() const;

    QString m_path;
    QHash<QByteArray, TrustedPeer> m_peers;
};

} // namespace df
