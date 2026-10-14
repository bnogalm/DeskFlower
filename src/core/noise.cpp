#include "noise.h"

namespace df::noise {

namespace {
const QByteArray kProtocolName("Noise_IK_25519_ChaChaPoly_SHA256");
constexpr int kHashLen = 32;

// Noise HKDF with HMAC-SHA256, producing two outputs.
void hkdf2(const QByteArray &ck, const QByteArray &ikm, QByteArray *out1, QByteArray *out2)
{
    const QByteArray tempKey = crypto::hmacSha256(ck, ikm);
    *out1 = crypto::hmacSha256(tempKey, QByteArray(1, '\x01'));
    *out2 = crypto::hmacSha256(tempKey, *out1 + QByteArray(1, '\x02'));
}
} // namespace

SymmetricState::SymmetricState(const QByteArray &protocolName)
{
    if (protocolName.size() <= kHashLen) {
        m_h = protocolName;
        m_h.append(QByteArray(kHashLen - protocolName.size(), '\0'));
    } else {
        m_h = crypto::sha256(protocolName);
    }
    m_ck = m_h;
}

SymmetricState::~SymmetricState()
{
    crypto::wipe(m_ck);
    crypto::wipe(m_k);
}

void SymmetricState::mixHash(const QByteArray &data) { m_h = crypto::sha256(m_h + data); }

void SymmetricState::mixKey(const QByteArray &ikm)
{
    QByteArray tempK;
    hkdf2(m_ck, ikm, &m_ck, &tempK);
    m_k = tempK.left(crypto::kKeySize);
    m_n = 0;
}

QByteArray SymmetricState::encryptAndHash(const QByteArray &plaintext)
{
    const QByteArray ct = m_k.isEmpty() ? plaintext : crypto::aeadEncrypt(m_k, m_n++, m_h, plaintext);
    mixHash(ct);
    return ct;
}

std::optional<QByteArray> SymmetricState::decryptAndHash(const QByteArray &ciphertext)
{
    QByteArray pt;
    if (m_k.isEmpty()) {
        pt = ciphertext;
    } else {
        auto dec = crypto::aeadDecrypt(m_k, m_n, m_h, ciphertext);
        if (!dec)
            return std::nullopt;
        ++m_n;
        pt = *dec;
    }
    mixHash(ciphertext);
    return pt;
}

void SymmetricState::split(QByteArray *k1, QByteArray *k2) const
{
    hkdf2(m_ck, QByteArray(), k1, k2);
    k1->truncate(crypto::kKeySize);
    k2->truncate(crypto::kKeySize);
}

// --- Initiator ---------------------------------------------------------------------------------

IKInitiator::IKInitiator(const crypto::KeyPair &local, const QByteArray &remoteStatic, const QByteArray &prologue)
    : m_s(local), m_rs(remoteStatic), m_ss(kProtocolName)
{
    m_ss.mixHash(prologue);
    m_ss.mixHash(m_rs); // pre-message: <- s
}

QByteArray IKInitiator::writeMessage1(const QByteArray &payload)
{
    m_e = crypto::generateKeyPair();
    QByteArray msg = m_e.publicKey;
    m_ss.mixHash(m_e.publicKey);
    // es
    const auto es = crypto::dh(m_e.secretKey, m_rs);
    if (!es)
        return {};
    m_ss.mixKey(*es);
    // s
    msg += m_ss.encryptAndHash(m_s.publicKey);
    // ss
    const auto ss = crypto::dh(m_s.secretKey, m_rs);
    if (!ss)
        return {};
    m_ss.mixKey(*ss);
    msg += m_ss.encryptAndHash(payload);
    return msg;
}

std::optional<TransportKeys> IKInitiator::readMessage2(const QByteArray &message, QByteArray *payload)
{
    if (message.size() < kMessage2Overhead)
        return std::nullopt;
    const QByteArray re = message.left(32);
    m_ss.mixHash(re);
    // ee
    const auto ee = crypto::dh(m_e.secretKey, re);
    if (!ee)
        return std::nullopt;
    m_ss.mixKey(*ee);
    // se: initiator static, responder ephemeral
    const auto se = crypto::dh(m_s.secretKey, re);
    if (!se)
        return std::nullopt;
    m_ss.mixKey(*se);
    auto pt = m_ss.decryptAndHash(message.mid(32));
    if (!pt)
        return std::nullopt;
    if (payload)
        *payload = *pt;
    TransportKeys keys;
    m_ss.split(&keys.sendKey, &keys.recvKey);
    keys.handshakeHash = m_ss.handshakeHash();
    crypto::wipe(m_e.secretKey);
    return keys;
}

// --- Responder ---------------------------------------------------------------------------------

IKResponder::IKResponder(const crypto::KeyPair &local, const QByteArray &prologue)
    : m_s(local), m_ss(kProtocolName)
{
    m_ss.mixHash(prologue);
    m_ss.mixHash(m_s.publicKey); // pre-message: <- s
}

std::optional<QByteArray> IKResponder::readMessage1(const QByteArray &message, QByteArray *payload)
{
    if (message.size() < kMessage1Overhead)
        return std::nullopt;
    m_re = message.left(32);
    m_ss.mixHash(m_re);
    // es: initiator ephemeral, responder static
    const auto es = crypto::dh(m_s.secretKey, m_re);
    if (!es)
        return std::nullopt;
    m_ss.mixKey(*es);
    auto rs = m_ss.decryptAndHash(message.mid(32, 48));
    if (!rs || rs->size() != 32)
        return std::nullopt;
    m_rs = *rs;
    // ss
    const auto ss = crypto::dh(m_s.secretKey, m_rs);
    if (!ss)
        return std::nullopt;
    m_ss.mixKey(*ss);
    auto pt = m_ss.decryptAndHash(message.mid(80));
    if (!pt)
        return std::nullopt;
    if (payload)
        *payload = *pt;
    return m_rs;
}

std::optional<TransportKeys> IKResponder::writeMessage2(const QByteArray &payload, QByteArray *message)
{
    m_e = crypto::generateKeyPair();
    QByteArray msg = m_e.publicKey;
    m_ss.mixHash(m_e.publicKey);
    const auto ee = crypto::dh(m_e.secretKey, m_re);
    if (!ee)
        return std::nullopt;
    m_ss.mixKey(*ee);
    const auto se = crypto::dh(m_e.secretKey, m_rs);
    if (!se)
        return std::nullopt;
    m_ss.mixKey(*se);
    msg += m_ss.encryptAndHash(payload);
    *message = msg;
    TransportKeys keys;
    QByteArray k1, k2;
    m_ss.split(&k1, &k2);
    keys.sendKey = k2; // responder sends with the second key
    keys.recvKey = k1;
    keys.handshakeHash = m_ss.handshakeHash();
    crypto::wipe(m_e.secretKey);
    return keys;
}

} // namespace df::noise
