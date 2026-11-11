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
    m_forgottenPath = QDir(configDir).filePath(QStringLiteral("forgotten.json"));
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
        p.lastAddress = o.value(QStringLiteral("address")).toString();
        p.lastPort = quint16(o.value(QStringLiteral("port")).toInt());
        for (const QJsonValue &e : o.value(QStringLiteral("addresses")).toArray()) {
            const QJsonObject eo = e.toObject();
            StoredEndpoint ep;
            ep.address = eo.value(QStringLiteral("a")).toString();
            ep.port = quint16(eo.value(QStringLiteral("p")).toInt());
            ep.tcpPort = quint16(eo.value(QStringLiteral("tp")).toInt());
            const auto source = endpointSourceFromName(eo.value(QStringLiteral("src")).toString());
            ep.lastOk = qint64(eo.value(QStringLiteral("ok")).toDouble());
            if (ep.address.isEmpty() || !ep.port || !source || *source == EndpointSource::Beacon)
                continue;
            ep.source = *source;
            p.endpoints.append(ep);
        }
        p.trust = o.value(QStringLiteral("guest")).toBool()   ? TrustLevel::Guest
                  : o.value(QStringLiteral("group")).toBool() ? TrustLevel::Group
                                                              : TrustLevel::Device;
        p.introducedBy = unb64(o.value(QStringLiteral("introducedBy")));
        p.everConnected = o.value(QStringLiteral("connected")).toBool();
        m_peers.insert(p.id, p);
    }
    m_forgotten.clear();
    const QJsonArray forgotten = readJson(m_forgottenPath).array();
    for (const QJsonValue &v : forgotten) {
        const QJsonObject o = v.toObject();
        const QByteArray id = v.isObject() ? unb64(o.value(QStringLiteral("id"))) : unb64(v);
        if (!id.isEmpty())
            m_forgotten.insert(id, o.value(QStringLiteral("name")).toString().left(64));
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

void PeerStore::upsertAll(const QList<TrustedPeer> &peers)
{
    if (peers.isEmpty())
        return;
    for (const TrustedPeer &peer : peers)
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

void PeerStore::setLastAddress(const QByteArray &id, const QString &address, quint16 port)
{
    auto it = m_peers.find(id);
    if (it != m_peers.end() && (it->lastAddress != address || it->lastPort != port)) {
        it->lastAddress = address;
        it->lastPort = port;
        save();
    }
}

void PeerStore::setEndpoints(const QByteArray &id, const QList<StoredEndpoint> &endpoints)
{
    auto it = m_peers.find(id);
    if (it != m_peers.end() && it->endpoints != endpoints) {
        it->endpoints = endpoints;
        save();
    }
}

void PeerStore::setTrust(const QByteArray &id, TrustLevel level)
{
    auto it = m_peers.find(id);
    if (it == m_peers.end() || it->trust == level)
        return;
    it->trust = level;
    if (level == TrustLevel::Guest)
        it->allowView = false; // a guest never views this screen
    save();
}

void PeerStore::setConnectedOnce(const QByteArray &id)
{
    auto it = m_peers.find(id);
    if (it != m_peers.end() && !it->everConnected) {
        it->everConnected = true;
        save();
    }
}

QList<QByteArray> PeerStore::introducedBy(const QByteArray &introducer, bool neverConnected) const
{
    QList<QByteArray> out;
    if (introducer.isEmpty())
        return out;
    for (const TrustedPeer &p : m_peers) {
        if (p.introducedBy == introducer && (!neverConnected || !p.everConnected))
            out.append(p.id);
    }
    return out;
}

int PeerStore::introducedCount() const
{
    int n = 0;
    for (const TrustedPeer &p : m_peers)
        n += p.introducedBy.isEmpty() ? 0 : 1;
    return n;
}

void PeerStore::forget(const QByteArray &id, const QString &name)
{
    if (id.isEmpty() || (m_forgotten.contains(id) && m_forgotten.value(id) == name))
        return;
    m_forgotten.insert(id, name.left(64));
    saveForgotten();
}

void PeerStore::unforget(const QByteArray &id)
{
    if (m_forgotten.remove(id))
        saveForgotten();
}

bool PeerStore::saveForgotten() const
{
    QJsonArray arr;
    for (auto it = m_forgotten.constBegin(); it != m_forgotten.constEnd(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), b64(it.key()));
        o.insert(QStringLiteral("name"), it.value());
        arr.append(o);
    }
    return writeJson(m_forgottenPath, QJsonDocument(arr));
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
        if (p.trust == TrustLevel::Guest)
            o.insert(QStringLiteral("guest"), true);
        else if (p.trust == TrustLevel::Group)
            o.insert(QStringLiteral("group"), true);
        if (!p.lastAddress.isEmpty()) {
            o.insert(QStringLiteral("address"), p.lastAddress);
            o.insert(QStringLiteral("port"), p.lastPort);
        }
        if (!p.endpoints.isEmpty()) {
            QJsonArray addresses;
            for (const StoredEndpoint &e : p.endpoints) {
                QJsonObject eo;
                eo.insert(QStringLiteral("a"), e.address);
                eo.insert(QStringLiteral("p"), e.port);
                if (e.tcpPort)
                    eo.insert(QStringLiteral("tp"), e.tcpPort);
                eo.insert(QStringLiteral("src"), endpointSourceName(e.source));
                if (e.lastOk)
                    eo.insert(QStringLiteral("ok"), double(e.lastOk));
                addresses.append(eo);
            }
            o.insert(QStringLiteral("addresses"), addresses);
        }
        if (!p.introducedBy.isEmpty())
            o.insert(QStringLiteral("introducedBy"), b64(p.introducedBy));
        if (p.everConnected)
            o.insert(QStringLiteral("connected"), true);
        arr.append(o);
    }
    return writeJson(m_path, QJsonDocument(arr));
}

} // namespace df
