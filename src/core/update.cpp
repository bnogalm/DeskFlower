#include "update.h"

#include "crypto.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <sodium.h>

namespace df::update {

namespace {
const unsigned char *u(const QByteArray &b) { return reinterpret_cast<const unsigned char *>(b.constData()); }

std::optional<QList<quint64>> parseVersion(const QString &v)
{
    static const QRegularExpression re(QStringLiteral("^\\d{1,9}(\\.\\d{1,9}){0,3}$"));
    if (!re.match(v).hasMatch())
        return std::nullopt;
    QList<quint64> parts;
    for (const QString &p : v.split(QLatin1Char('.')))
        parts.append(p.toULongLong());
    return parts;
}

template <typename T>
T fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return T{};
}
} // namespace

SigningKey generateSigningKey()
{
    crypto::init();
    SigningKey key;
    key.publicKey.resize(crypto_sign_PUBLICKEYBYTES);
    key.secretKey.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_keypair(reinterpret_cast<unsigned char *>(key.publicKey.data()),
                        reinterpret_cast<unsigned char *>(key.secretKey.data()));
    return key;
}

QByteArray sign(const QByteArray &secretKey, const QByteArray &data)
{
    static_assert(crypto_sign_BYTES == kSignatureSize && crypto_sign_SECRETKEYBYTES == kSecretKeySize);
    crypto::init();
    if (secretKey.size() != kSecretKeySize)
        return {};
    QByteArray sig(kSignatureSize, Qt::Uninitialized);
    crypto_sign_detached(reinterpret_cast<unsigned char *>(sig.data()), nullptr, u(data), quint64(data.size()),
                         u(secretKey));
    return sig;
}

bool verify(const QByteArray &publicKey, const QByteArray &data, const QByteArray &signature)
{
    static_assert(crypto_sign_PUBLICKEYBYTES == kPublicKeySize);
    crypto::init();
    if (publicKey.size() != kPublicKeySize || signature.size() != kSignatureSize)
        return false;
    return crypto_sign_verify_detached(u(signature), u(data), quint64(data.size()), u(publicKey)) == 0;
}

QByteArray encodeSignature(const QByteArray &signature) { return signature.toHex() + '\n'; }

QByteArray decodeSignature(const QByteArray &text)
{
    const QByteArray hex = text.trimmed();
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{%1}$").arg(kSignatureSize * 2));
    return re.match(QString::fromLatin1(hex)).hasMatch() ? QByteArray::fromHex(hex) : QByteArray();
}

std::optional<int> compareVersions(const QString &a, const QString &b)
{
    auto pa = parseVersion(a), pb = parseVersion(b);
    if (!pa || !pb)
        return std::nullopt;
    for (qsizetype i = 0; i < qMax(pa->size(), pb->size()); ++i) {
        const quint64 x = pa->value(i), y = pb->value(i);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

bool isSafeFileName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return re.match(name).hasMatch() && !name.contains(QLatin1String(".."));
}

QByteArray Manifest::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("product"), product);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("platform"), platform);
    o.insert(QStringLiteral("file"), file);
    o.insert(QStringLiteral("size"), size);
    o.insert(QStringLiteral("sha256"), QString::fromLatin1(sha256.toHex()));
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

std::optional<Manifest> Manifest::fromJson(const QByteArray &json, QString *error)
{
    using R = std::optional<Manifest>;
    if (json.size() > kMaxManifestSize)
        return fail<R>(error, QStringLiteral("Manifest too large"));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject())
        return fail<R>(error, QStringLiteral("Malformed manifest"));
    const QJsonObject o = doc.object();
    Manifest m;
    m.product = o.value(QStringLiteral("product")).toString();
    m.version = o.value(QStringLiteral("version")).toString();
    m.platform = o.value(QStringLiteral("platform")).toString();
    m.file = o.value(QStringLiteral("file")).toString();
    m.size = o.value(QStringLiteral("size")).toInteger(-1);
    const QString hash = o.value(QStringLiteral("sha256")).toString();
    m.sha256 = QByteArray::fromHex(hash.toLatin1());
    if (m.product != kProduct)
        return fail<R>(error, QStringLiteral("The package is not DeskFlower"));
    if (!compareVersions(m.version, m.version))
        return fail<R>(error, QStringLiteral("Invalid version in manifest"));
    if (m.platform.isEmpty() || !isSafeFileName(m.file) || m.size <= 0 || hash.size() != 64 || m.sha256.size() != 32)
        return fail<R>(error, QStringLiteral("Invalid manifest"));
    return m;
}

std::optional<Manifest> describe(const QString &packagePath, const QString &version, const QString &platform,
                                 QString *error)
{
    using R = std::optional<Manifest>;
    QFile file(packagePath);
    if (!file.open(QIODevice::ReadOnly))
        return fail<R>(error, QStringLiteral("Cannot read %1").arg(packagePath));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return fail<R>(error, QStringLiteral("Cannot read %1").arg(packagePath));
    Manifest m;
    m.product = kProduct;
    m.version = version;
    m.platform = platform;
    m.file = QFileInfo(packagePath).fileName();
    m.size = file.size();
    m.sha256 = hash.result();
    // Same checks as a receiver makes.
    if (!Manifest::fromJson(m.toJson(), error))
        return std::nullopt;
    return m;
}

std::optional<Manifest> open(const QByteArray &publicKey, const QByteArray &json, const QByteArray &signature,
                             QString *error)
{
    if (publicKey.size() != kPublicKeySize)
        return fail<std::optional<Manifest>>(error, QStringLiteral("Updates are not configured in this build"));
    if (!verify(publicKey, json, signature))
        return fail<std::optional<Manifest>>(error, QStringLiteral("Invalid signature on the update manifest"));
    return Manifest::fromJson(json, error);
}

bool isApplicable(const Manifest &manifest, const QString &currentVersion, const QString &platform, QString *error)
{
    if (manifest.platform != platform)
        return fail<bool>(error, QStringLiteral("The package is for %1, this device is %2")
                                     .arg(manifest.platform, platform));
    const auto cmp = compareVersions(manifest.version, currentVersion);
    if (!cmp)
        return fail<bool>(error, QStringLiteral("Cannot compare version %1 with %2")
                                     .arg(manifest.version, currentVersion));
    if (*cmp <= 0)
        return fail<bool>(error, QStringLiteral("Version %1 is not newer than the installed %2")
                                     .arg(manifest.version, currentVersion));
    return true;
}

bool matchesContent(const Manifest &manifest, QIODevice *device, QString *error)
{
    if (!device->seek(0))
        return fail<bool>(error, QStringLiteral("Cannot read the package"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 n = device->read(buffer.data(), buffer.size());
        if (n < 0)
            return fail<bool>(error, QStringLiteral("Cannot read the package"));
        if (n == 0)
            break;
        total += n;
        if (total > manifest.size)
            return fail<bool>(error, QStringLiteral("The package is larger than its manifest says"));
        hash.addData(QByteArrayView(buffer.constData(), n));
    }
    if (total != manifest.size)
        return fail<bool>(error, QStringLiteral("The package size does not match its manifest"));
    if (!crypto::constantTimeEqual(hash.result(), manifest.sha256))
        return fail<bool>(error, QStringLiteral("The package does not match its manifest (SHA-256)"));
    return true;
}

} // namespace df::update
