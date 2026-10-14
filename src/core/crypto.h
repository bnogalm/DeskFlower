#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace df::crypto {

constexpr int kKeySize = 32;
constexpr int kTagSize = 16;
constexpr int kDeviceIdSize = 16;

// Must be called once before any other function. Returns false if libsodium failed.
bool init();

QByteArray randomBytes(int n);
quint32 randomUniform(quint32 upperBound);

struct KeyPair {
    QByteArray publicKey;  // X25519, 32 bytes
    QByteArray secretKey;  // X25519, 32 bytes
};

KeyPair generateKeyPair();
QByteArray publicKeyFromSecret(const QByteArray &secretKey);

// X25519. Returns nullopt for low-order / invalid public keys.
std::optional<QByteArray> dh(const QByteArray &secretKey, const QByteArray &publicKey);

QByteArray sha256(const QByteArray &data);
QByteArray sha512(const QByteArray &data);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);

// ChaCha20-Poly1305 (IETF), nonce = 32 zero bits || little-endian 64-bit counter (Noise convention).
QByteArray aeadEncrypt(const QByteArray &key, quint64 nonce, const QByteArray &ad, const QByteArray &plaintext);
std::optional<QByteArray> aeadDecrypt(const QByteArray &key, quint64 nonce, const QByteArray &ad,
                                      const QByteArray &ciphertext);

// Stable device identifier derived from the long-term public key.
QByteArray deviceIdFromPublicKey(const QByteArray &publicKey);
// Human-readable fingerprint ("AB12-CD34-...") for display.
QString fingerprint(const QByteArray &publicKey);

bool constantTimeEqual(const QByteArray &a, const QByteArray &b);
void wipe(QByteArray &data);

} // namespace df::crypto
