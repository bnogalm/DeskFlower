#include "crypto.h"

#include <QStringList>
#include <QtEndian>
#include <cstring>
#include <sodium.h>

namespace df::crypto {

namespace {
const unsigned char *u(const QByteArray &b) { return reinterpret_cast<const unsigned char *>(b.constData()); }
unsigned char *u(QByteArray &b) { return reinterpret_cast<unsigned char *>(b.data()); }

void makeNonce(quint64 counter, unsigned char out[crypto_aead_chacha20poly1305_IETF_NPUBBYTES])
{
    static_assert(crypto_aead_chacha20poly1305_IETF_NPUBBYTES == 12);
    std::memset(out, 0, 4);
    qToLittleEndian<quint64>(counter, out + 4);
}
} // namespace

bool init() { return sodium_init() >= 0; }

QByteArray randomBytes(int n)
{
    QByteArray out(n, Qt::Uninitialized);
    randombytes_buf(out.data(), size_t(n));
    return out;
}

quint32 randomUniform(quint32 upperBound) { return randombytes_uniform(upperBound); }

KeyPair generateKeyPair()
{
    KeyPair kp;
    kp.secretKey = randomBytes(crypto_scalarmult_SCALARBYTES);
    kp.publicKey = publicKeyFromSecret(kp.secretKey);
    return kp;
}

QByteArray publicKeyFromSecret(const QByteArray &secretKey)
{
    QByteArray pub(crypto_scalarmult_BYTES, Qt::Uninitialized);
    crypto_scalarmult_base(u(pub), u(secretKey));
    return pub;
}

std::optional<QByteArray> dh(const QByteArray &secretKey, const QByteArray &publicKey)
{
    if (secretKey.size() != kKeySize || publicKey.size() != kKeySize)
        return std::nullopt;
    QByteArray shared(crypto_scalarmult_BYTES, Qt::Uninitialized);
    if (crypto_scalarmult(u(shared), u(secretKey), u(publicKey)) != 0)
        return std::nullopt;
    return shared;
}

QByteArray sha256(const QByteArray &data)
{
    QByteArray out(crypto_hash_sha256_BYTES, Qt::Uninitialized);
    crypto_hash_sha256(u(out), u(data), quint64(data.size()));
    return out;
}

QByteArray sha512(const QByteArray &data)
{
    QByteArray out(crypto_hash_sha512_BYTES, Qt::Uninitialized);
    crypto_hash_sha512(u(out), u(data), quint64(data.size()));
    return out;
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, u(key), size_t(key.size()));
    crypto_auth_hmacsha256_update(&st, u(data), quint64(data.size()));
    QByteArray out(crypto_auth_hmacsha256_BYTES, Qt::Uninitialized);
    crypto_auth_hmacsha256_final(&st, u(out));
    return out;
}

QByteArray aeadEncrypt(const QByteArray &key, quint64 nonce, const QByteArray &ad, const QByteArray &plaintext)
{
    unsigned char n[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
    makeNonce(nonce, n);
    QByteArray out(plaintext.size() + kTagSize, Qt::Uninitialized);
    unsigned long long outLen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(u(out), &outLen, u(plaintext), quint64(plaintext.size()), u(ad),
                                              quint64(ad.size()), nullptr, n, u(key));
    out.resize(qsizetype(outLen));
    return out;
}

std::optional<QByteArray> aeadDecrypt(const QByteArray &key, quint64 nonce, const QByteArray &ad,
                                      const QByteArray &ciphertext)
{
    if (ciphertext.size() < kTagSize || key.size() != kKeySize)
        return std::nullopt;
    unsigned char n[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
    makeNonce(nonce, n);
    QByteArray out(ciphertext.size() - kTagSize, Qt::Uninitialized);
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(u(out), &outLen, nullptr, u(ciphertext), quint64(ciphertext.size()),
                                                  u(ad), quint64(ad.size()), n, u(key)) != 0)
        return std::nullopt;
    out.resize(qsizetype(outLen));
    return out;
}

QByteArray deviceIdFromPublicKey(const QByteArray &publicKey)
{
    static const QByteArray kPersonal("deskflower-id");
    QByteArray out(kDeviceIdSize, Qt::Uninitialized);
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, size_t(out.size()));
    crypto_generichash_update(&st, u(kPersonal), quint64(kPersonal.size()));
    crypto_generichash_update(&st, u(publicKey), quint64(publicKey.size()));
    crypto_generichash_final(&st, u(out), size_t(out.size()));
    return out;
}

QString fingerprint(const QByteArray &publicKey)
{
    const QString hex = QString::fromLatin1(sha256(publicKey).left(8).toHex().toUpper());
    QStringList groups;
    for (int i = 0; i < hex.size(); i += 4)
        groups << hex.mid(i, 4);
    return groups.join(QLatin1Char('-'));
}

bool constantTimeEqual(const QByteArray &a, const QByteArray &b)
{
    return a.size() == b.size() && sodium_memcmp(a.constData(), b.constData(), size_t(a.size())) == 0;
}

void wipe(QByteArray &data)
{
    if (!data.isEmpty())
        sodium_memzero(data.data(), size_t(data.size()));
    data.clear();
}

} // namespace df::crypto
