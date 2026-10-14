#include "store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>

namespace df {

namespace {
bool writeJson(const QString &path, const QJsonDocument &doc)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(doc.toJson());
    if (!f.commit())
        return false;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

QJsonDocument readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll());
}

QString b64(const QByteArray &b) { return QString::fromLatin1(b.toBase64()); }
QByteArray unb64(const QJsonValue &v) { return QByteArray::fromBase64(v.toString().toLatin1()); }
} // namespace

// --- Identity ----------------------------------------------------------------------------------

bool Identity::loadOrCreate(const QString &configDir, QString *error)
{
    if (!QDir().mkpath(configDir)) {
        if (error)
            *error = QStringLiteral("Cannot create config directory %1").arg(configDir);
        return false;
    }
    m_path = QDir(configDir).filePath(QStringLiteral("identity.json"));
    const QJsonObject obj = readJson(m_path).object();
    const QByteArray secret = unb64(obj.value(QStringLiteral("secretKey")));
    if (secret.size() == crypto::kKeySize) {
        m_keys.secretKey = secret;
        m_keys.publicKey = crypto::publicKeyFromSecret(secret);
        m_name = obj.value(QStringLiteral("name")).toString();
    } else {
        m_keys = crypto::generateKeyPair();
    }
    if (m_name.isEmpty())
        m_name = QSysInfo::machineHostName();
    m_id = crypto::deviceIdFromPublicKey(m_keys.publicKey);
    if (!save()) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(m_path);
        return false;
    }
    return true;
}

bool Identity::setName(const QString &name)
{
    const QString trimmed = name.trimmed().left(64);
    if (trimmed.isEmpty())
        return false;
    m_name = trimmed;
    return save();
}

bool Identity::save() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("secretKey"), b64(m_keys.secretKey));
    obj.insert(QStringLiteral("name"), m_name);
    return writeJson(m_path, QJsonDocument(obj));
}

// --- PeerStore ---------------------------------------------------------------------------------

bool PeerStore::load(const QString &configDir)
{
    m_path = QDir(configDir).filePath(QStringLiteral("peers.json"));
    m_peers.clear();
    const QJsonArray arr = readJson(m_path).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        TrustedPeer p;
        p.publicKey = unb64(o.value(QStringLiteral("publicKey")));
        if (p.publicKey.size() != crypto::kKeySize)
            continue;
        p.id = crypto::deviceIdFromPublicKey(p.publicKey);
        p.name = o.value(QStringLiteral("name")).toString();
        p.pairedAt = QDateTime::fromString(o.value(QStringLiteral("pairedAt")).toString(), Qt::ISODate);
        p.allowView = o.value(QStringLiteral("allowView")).toBool();
        p.owner = o.value(QStringLiteral("owner")).toString();
        m_peers.insert(p.id, p);
    }
    return true;
}

const TrustedPeer *PeerStore::find(const QByteArray &id) const
{
    auto it = m_peers.constFind(id);
    return it == m_peers.constEnd() ? nullptr : &it.value();
}

void PeerStore::upsert(const TrustedPeer &peer)
{
    m_peers.insert(peer.id, peer);
    save();
}

void PeerStore::remove(const QByteArray &id)
{
    if (m_peers.remove(id))
        save();
}

void PeerStore::rename(const QByteArray &id, const QString &name)
{
    auto it = m_peers.find(id);
    if (it != m_peers.end() && it->name != name) {
        it->name = name;
        save();
    }
}

void PeerStore::setAllowView(const QByteArray &id, bool on)
{
    auto it = m_peers.find(id);
    if (it != m_peers.end() && it->allowView != on) {
        it->allowView = on;
        save();
    }
}

void PeerStore::setOwner(const QByteArray &id, const QString &owner)
{
    auto it = m_peers.find(id);
    if (it != m_peers.end() && it->owner != owner) {
        it->owner = owner;
        save();
    }
}

bool PeerStore::save() const
{
    QJsonArray arr;
    for (const TrustedPeer &p : m_peers) {
        QJsonObject o;
        o.insert(QStringLiteral("publicKey"), b64(p.publicKey));
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("pairedAt"), p.pairedAt.toString(Qt::ISODate));
        if (p.allowView)
            o.insert(QStringLiteral("allowView"), true);
        if (!p.owner.isEmpty())
            o.insert(QStringLiteral("owner"), p.owner);
        arr.append(o);
    }
    return writeJson(m_path, QJsonDocument(arr));
}

} // namespace df
