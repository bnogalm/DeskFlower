#include "session.h"

#include "wire.h"

namespace df {

bool ReplayWindow::check(quint64 counter) const
{
    if (!m_any || counter > m_top)
        return true;
    const quint64 diff = m_top - counter;
    if (diff >= 64)
        return false;
    return !(m_bitmap & (quint64(1) << diff));
}

void ReplayWindow::update(quint64 counter)
{
    if (!m_any) {
        m_any = true;
        m_top = counter;
        m_bitmap = 1;
        return;
    }
    if (counter > m_top) {
        const quint64 shift = counter - m_top;
        m_bitmap = shift >= 64 ? 1 : (m_bitmap << shift) | 1;
        m_top = counter;
    } else {
        m_bitmap |= quint64(1) << (m_top - counter);
    }
}

SecureSession::SecureSession(quint32 localIndex, quint32 remoteIndex, const noise::TransportKeys &keys,
                             bool initiator, qint64 nowMs)
    : m_localIndex(localIndex),
      m_remoteIndex(remoteIndex),
      m_sendKey(keys.sendKey),
      m_recvKey(keys.recvKey),
      m_initiator(initiator),
      m_confirmed(initiator),
      m_createdAt(nowMs),
      m_lastRecv(nowMs),
      m_lastSend(0)
{
}

SecureSession::~SecureSession()
{
    crypto::wipe(m_sendKey);
    crypto::wipe(m_recvKey);
}

QByteArray SecureSession::seal(const QByteArray &inner, qint64 nowMs)
{
    QByteArray packet = wire::header(wire::Type::Data);
    wire::appendU32(packet, m_remoteIndex);
    const quint64 counter = m_sendCounter++;
    wire::appendU64(packet, counter);
    packet += crypto::aeadEncrypt(m_sendKey, counter, packet, inner);
    m_lastSend = nowMs;
    return packet;
}

std::optional<QByteArray> SecureSession::open(const QByteArray &packet, qint64 nowMs)
{
    if (packet.size() < wire::kDataHeaderSize + crypto::kTagSize)
        return std::nullopt;
    const quint64 counter = wire::readU64(packet, wire::kHeaderSize + 4);
    if (!m_replay.check(counter))
        return std::nullopt;
    const QByteArray ad = packet.left(wire::kDataHeaderSize);
    auto inner = crypto::aeadDecrypt(m_recvKey, counter, ad, packet.mid(wire::kDataHeaderSize));
    if (!inner)
        return std::nullopt;
    m_replay.update(counter);
    m_lastRecv = nowMs;
    m_confirmed = true;
    return inner;
}

} // namespace df
