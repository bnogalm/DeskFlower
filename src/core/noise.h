#pragma once

// Minimal Noise_IK_25519_ChaChaPoly_SHA256 implementation (https://noiseprotocol.org/noise.html).
//
//   <- s
//   ...
//   -> e, es, s, ss
//   <- e, ee, se
//
// The initiator knows the responder's static key (stored at pairing time). The responder learns
// the initiator's static key from message 1 and must check it against its trusted peer list.

#include "crypto.h"

#include <QByteArray>
#include <optional>

namespace df::noise {

struct TransportKeys {
    QByteArray sendKey;
    QByteArray recvKey;
    QByteArray handshakeHash;
};

class SymmetricState
{
public:
    explicit SymmetricState(const QByteArray &protocolName);
    ~SymmetricState();

    void mixHash(const QByteArray &data);
    void mixKey(const QByteArray &ikm);
    QByteArray encryptAndHash(const QByteArray &plaintext);
    std::optional<QByteArray> decryptAndHash(const QByteArray &ciphertext);
    void split(QByteArray *k1, QByteArray *k2) const;
    QByteArray handshakeHash() const { return m_h; }

private:
    QByteArray m_ck;
    QByteArray m_h;
    QByteArray m_k;
    quint64 m_n = 0;
};

class IKInitiator
{
public:
    IKInitiator(const crypto::KeyPair &local, const QByteArray &remoteStatic, const QByteArray &prologue);

    // Returns an empty array if the remote static key is invalid.
    QByteArray writeMessage1(const QByteArray &payload);
    // Returns keys and fills payload on success; nullopt if the message does not authenticate.
    std::optional<TransportKeys> readMessage2(const QByteArray &message, QByteArray *payload);

private:
    crypto::KeyPair m_s;
    crypto::KeyPair m_e;
    QByteArray m_rs;
    SymmetricState m_ss;
};

class IKResponder
{
public:
    IKResponder(const crypto::KeyPair &local, const QByteArray &prologue);

    // On success returns the initiator's static public key and fills payload.
    std::optional<QByteArray> readMessage1(const QByteArray &message, QByteArray *payload);
    std::optional<TransportKeys> writeMessage2(const QByteArray &payload, QByteArray *message);

private:
    crypto::KeyPair m_s;
    crypto::KeyPair m_e;
    QByteArray m_re;
    QByteArray m_rs;
    SymmetricState m_ss;
};

// Size of message 1 with an empty payload: e(32) + enc(s)(48) + tag(16).
constexpr int kMessage1Overhead = 32 + 48 + 16;
// Size of message 2 with an empty payload: e(32) + tag(16).
constexpr int kMessage2Overhead = 32 + 16;

} // namespace df::noise
