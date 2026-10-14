#include "reliable.h"

#include "wire.h"

#include <algorithm>

namespace df {

namespace {
// Serial-number arithmetic so that sequence wrap-around keeps working.
bool seqLess(quint32 a, quint32 b) { return qint32(a - b) < 0; }

constexpr quint8 kMoreFragments = 0x01;
constexpr int kMinRto = 30;
constexpr int kMaxRto = 1000;
} // namespace

ReliableChannel::ReliableChannel(Sender sender) : m_sender(std::move(sender)) {}

void ReliableChannel::reset()
{
    m_nextSeq = 0;
    m_queue.clear();
    m_inFlight.clear();
    m_srtt = -1;
    m_rto = 200;
    m_cwnd = kInitialCwnd;
    m_ssthresh = kWindow;
    m_recoverUntil = 0;
    m_expected = 0;
    m_outOfOrder.clear();
    m_partial.clear();
}

void ReliableChannel::send(const QByteArray &message, qint64 nowMs)
{
    qsizetype offset = 0;
    do {
        const QByteArray fragment = message.mid(offset, kMaxFragment);
        offset += fragment.size();
        QByteArray packet;
        packet.reserve(fragment.size() + 6);
        packet.append(char(wire::Inner::Reliable));
        wire::appendU32(packet, m_nextSeq++);
        packet.append(char(offset < message.size() ? kMoreFragments : 0));
        packet.append(fragment);
        m_queue.push_back(packet);
    } while (offset < message.size());
    flush(nowMs);
}

void ReliableChannel::flush(qint64 nowMs)
{
    while (!m_queue.empty() && int(m_inFlight.size()) < int(m_cwnd)) {
        QByteArray packet = std::move(m_queue.front());
        m_queue.pop_front();
        const quint32 seq = wire::readU32(packet, 1);
        m_sender(packet);
        m_inFlight[seq] = Pending{packet, nowMs, nowMs, 0};
    }
}

void ReliableChannel::onLoss(quint32 seq)
{
    if (seqLess(seq, m_recoverUntil))
        return; // already reacted to a loss in this window
    m_ssthresh = std::max(m_cwnd / 2, kMinCwnd);
    m_cwnd = m_ssthresh;
    m_recoverUntil = m_nextSeq;
}

void ReliableChannel::tick(qint64 nowMs)
{
    for (auto &[seq, p] : m_inFlight) {
        const int backoff = std::min(kMaxRto, m_rto << std::min(p.retries, 5));
        if (nowMs - p.sentAt >= backoff) {
            onLoss(seq);
            m_sender(p.packet);
            p.sentAt = nowMs;
            ++p.retries;
        }
    }
    flush(nowMs);
}

void ReliableChannel::ackSample(const Pending &p, qint64 nowMs)
{
    if (p.retries != 0)
        return; // Karn's algorithm: ambiguous sample
    const int sample = int(nowMs - p.firstSentAt);
    m_srtt = m_srtt < 0 ? sample : (7 * m_srtt + sample) / 8;
    m_rto = std::clamp(m_srtt * 2 + 10, kMinRto, kMaxRto);
}

void ReliableChannel::handleAck(quint32 next, quint32 sack, qint64 nowMs)
{
    for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
        const quint32 seq = it->first;
        bool acked = seqLess(seq, next);
        if (!acked && seq != next) {
            const quint32 bit = seq - next - 1;
            acked = bit < 32 && (sack & (1u << bit));
        }
        if (acked) {
            ackSample(it->second, nowMs);
            // Slow start below the threshold, then about one packet per window.
            m_cwnd = std::min<double>(kWindow, m_cwnd + (m_cwnd < m_ssthresh ? 1.0 : 1.0 / m_cwnd));
            it = m_inFlight.erase(it);
        } else {
            ++it;
        }
    }

    // Fast retransmit: packets older than something the peer already has were lost.
    if (sack != 0) {
        int highestBit = 31;
        while (highestBit > 0 && !(sack & (1u << highestBit)))
            --highestBit;
        const quint32 highest = next + 1 + quint32(highestBit);
        const int minAge = std::max(2, m_srtt < 0 ? 2 : m_srtt);
        for (auto &[seq, p] : m_inFlight) {
            if (!seqLess(seq, highest))
                break;
            if (!p.fastRetransmitted && nowMs - p.sentAt >= minAge) {
                onLoss(seq);
                p.fastRetransmitted = true;
                p.sentAt = nowMs;
                ++p.retries;
                m_sender(p.packet);
            }
        }
    }
    flush(nowMs);
}

void ReliableChannel::sendAck()
{
    quint32 sack = 0;
    for (const auto &[seq, data] : m_outOfOrder) {
        const quint32 bit = seq - m_expected - 1;
        if (bit < 32)
            sack |= 1u << bit;
    }
    QByteArray ack;
    ack.append(char(wire::Inner::Ack));
    wire::appendU32(ack, m_expected);
    wire::appendU32(ack, sack);
    m_sender(ack);
}

QList<QByteArray> ReliableChannel::receive(const QByteArray &packet, qint64 nowMs)
{
    QList<QByteArray> delivered;
    if (packet.isEmpty())
        return delivered;

    const auto kind = wire::Inner(quint8(packet[0]));
    if (kind == wire::Inner::Ack) {
        if (packet.size() >= 9)
            handleAck(wire::readU32(packet, 1), wire::readU32(packet, 5), nowMs);
        return delivered;
    }
    if (kind != wire::Inner::Reliable || packet.size() < 6)
        return delivered;

    const quint32 seq = wire::readU32(packet, 1);
    // Accept anything inside the receive window; duplicates are just acknowledged again.
    if (!seqLess(seq, m_expected) && seqLess(seq, m_expected + kWindow * 2))
        m_outOfOrder.emplace(seq, packet.mid(5));

    for (auto it = m_outOfOrder.find(m_expected); it != m_outOfOrder.end(); it = m_outOfOrder.find(m_expected)) {
        const quint8 flags = quint8(it->second[0]);
        m_partial.append(it->second.constData() + 1, it->second.size() - 1);
        if (!(flags & kMoreFragments)) {
            delivered.append(m_partial);
            m_partial.clear();
        }
        m_outOfOrder.erase(it);
        ++m_expected;
    }
    sendAck();
    return delivered;
}

} // namespace df
