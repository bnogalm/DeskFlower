#include "certificate.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>

namespace df::web {

namespace {

template <auto Fn>
struct Deleter {
    template <typename T>
    void operator()(T *p) const { Fn(p); }
};
using PKeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY_free>>;
using X509Ptr = std::unique_ptr<X509, Deleter<X509_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO_free>>;
using ExtPtr = std::unique_ptr<X509_EXTENSION, Deleter<X509_EXTENSION_free>>;

QByteArray bioBytes(BIO *bio)
{
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return QByteArray(data, int(len));
}

bool addExtension(X509 *cert, int nid, const QByteArray &value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    ExtPtr ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.constData()));
    return ext && X509_add_ext(cert, ext.get(), -1) == 1;
}

bool generate(const QStringList &ips, const QStringList &dns, QByteArray *certPem, QByteArray *keyPem, QString *error)
{
    PKeyPtr key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    X509Ptr cert(X509_new());
    if (!key || !cert) {
        *error = QStringLiteral("OpenSSL key generation failed");
        return false;
    }
    X509_set_version(cert.get(), X509_VERSION_3);

    unsigned char serial[16];
    RAND_bytes(serial, sizeof(serial));
    serial[0] &= 0x7F; // positive
    BIGNUM *bn = BN_bin2bn(serial, sizeof(serial), nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()));
    BN_free(bn);

    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -24 * 3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 825LL * 24 * 3600);
    X509_set_pubkey(cert.get(), key.get());

    X509_NAME *name = X509_get_subject_name(cert.get());
    const QByteArray cn = QByteArrayLiteral("Deskflower ") + (dns.isEmpty() ? QByteArray() : dns.first().toUtf8());
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>("Deskflower"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(cn.constData()), -1,
                               -1, 0);
    X509_set_issuer_name(cert.get(), name);

    QStringList san;
    for (const QString &ip : ips)
        san << QStringLiteral("IP:%1").arg(ip);
    for (const QString &d : dns)
        san << QStringLiteral("DNS:%1").arg(d);
    if (!addExtension(cert.get(), NID_basic_constraints, "critical,CA:FALSE")
        || !addExtension(cert.get(), NID_key_usage, "critical,digitalSignature")
        || !addExtension(cert.get(), NID_ext_key_usage, "serverAuth")
        || (!san.isEmpty() && !addExtension(cert.get(), NID_subject_alt_name, san.join(QLatin1Char(',')).toUtf8()))) {
        *error = QStringLiteral("Cannot add certificate extensions");
        return false;
    }
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
        *error = QStringLiteral("Cannot sign certificate");
        return false;
    }

    BioPtr certBio(BIO_new(BIO_s_mem()));
    BioPtr keyBio(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509(certBio.get(), cert.get());
    PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    *certPem = bioBytes(certBio.get());
    *keyPem = bioBytes(keyBio.get());
    return true;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    if (!f.commit())
        return false;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

bool loadOrCreateTlsIdentity(const QString &dir, const QStringList &ipAddresses, const QStringList &dnsNames,
                             TlsIdentity *out, QString *error)
{
    const QString certPath = QDir(dir).filePath(QStringLiteral("phone-cert.pem"));
    const QString keyPath = QDir(dir).filePath(QStringLiteral("phone-key.pem"));

    QSslCertificate cert(readFile(certPath), QSsl::Pem);
    QSslKey key(readFile(keyPath), QSsl::Ec, QSsl::Pem);
    const bool usable = !cert.isNull() && !key.isNull()
                        && cert.expiryDate() > QDateTime::currentDateTimeUtc().addDays(30);
    if (!usable) {
        QByteArray certPem, keyPem;
        if (!generate(ipAddresses, dnsNames, &certPem, &keyPem, error))
            return false;
        if (!writeFile(keyPath, keyPem) || !writeFile(certPath, certPem)) {
            *error = QStringLiteral("Cannot write the certificate to %1").arg(dir);
            return false;
        }
        cert = QSslCertificate(certPem, QSsl::Pem);
        key = QSslKey(keyPem, QSsl::Ec, QSsl::Pem);
        if (cert.isNull() || key.isNull()) {
            *error = QStringLiteral("Generated certificate cannot be loaded");
            return false;
        }
    }
    out->certificate = cert;
    out->key = key;
    return true;
}

} // namespace df::web
