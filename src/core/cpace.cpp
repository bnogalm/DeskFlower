#include "cpace.h"

#include "crypto.h"

#include <QtEndian>
#include <sodium.h>

namespace df {

namespace {
unsigned char *u(QByteArray &b) { return reinterpret_cast<unsigned char *>(b.data()); }
const unsigned char *u(const QByteArray &b) { return reinterpret_cast<const unsigned char *>(b.constData()); }

// Length-value encoding so that concatenated fields cannot be confused.
QByteArray lv(const QByteArray &data)
{
    QByteArray out(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(quint32(data.size()), out.data());
    return out + data;
}
} // namespace

CPace::CPace(const QString &pin, const QByteArray &sessionId, const QByteArray &channelId) : m_sid(sessionId)
{
    const QByteArray genInput =
        lv("DeskflowerCPace-ristretto255-v1") + lv(pin.toUtf8()) + lv(sessionId) + lv(channelId);
    QByteArray hash = crypto::sha512(genInput);
    QByteArray g(crypto_core_ristretto255_BYTES, Qt::Uninitialized);
    crypto_core_ristretto255_from_hash(u(g), u(hash));

    m_y = QByteArray(crypto_core_ristretto255_SCALARBYTES, Qt::Uninitialized);
    crypto_core_ristretto255_scalar_random(u(m_y));
    QByteArray Y(crypto_core_ristretto255_BYTES, Qt::Uninitialized);
    if (crypto_scalarmult_ristretto255(u(Y), u(m_y), u(g)) == 0)
        m_Y = Y;
    crypto::wipe(hash);
    crypto::wipe(g);
}

CPace::~CPace() { crypto::wipe(m_y); }

std::optional<QByteArray> CPace::finish(const QByteArray &peerShare, bool isInitiator)
{
    if (!isValid() || peerShare.size() != crypto_core_ristretto255_BYTES)
        return std::nullopt;
    if (crypto_core_ristretto255_is_valid_point(u(peerShare)) != 1)
        return std::nullopt;
    if (crypto::constantTimeEqual(peerShare, m_Y))
        return std::nullopt; // reflection
    QByteArray K(crypto_core_ristretto255_BYTES, Qt::Uninitialized);
    if (crypto_scalarmult_ristretto255(u(K), u(m_y), u(peerShare)) != 0)
        return std::nullopt; // identity element
    const QByteArray &Ya = isInitiator ? m_Y : peerShare;
    const QByteArray &Yb = isInitiator ? peerShare : m_Y;
    QByteArray isk = crypto::sha512(lv("DeskflowerCPace-ISK") + lv(m_sid) + lv(K) + lv(Ya) + lv(Yb));
    crypto::wipe(K);
    return isk;
}

QByteArray CPace::confirmationTag(const QByteArray &isk, char role)
{
    return crypto::hmacSha256(isk, QByteArray("DeskflowerCPace-confirm-") + role);
}

} // namespace df
