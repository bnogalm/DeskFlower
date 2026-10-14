#pragma once

// Reliable, ordered message stream on top of an unreliable datagram transport.
// Messages are fragmented, numbered, selectively acknowledged and retransmitted.
// Pure logic: time is passed in and outgoing packets go through a callback, so it is easy to test.
//
// Flow control is a simple AIMD congestion window (slow start, halve on loss) so a large transfer
// (a clipboard image) does not overflow socket buffers; holes reported by the SACK bitmap are
// retransmitted immediately instead of waiting for the timeout. On a LAN the round trip is so short
// that slow start would put a whole message on the wire at once, so the window has a ceiling that
// fits a default Linux receive buffer (about 90 datagrams), and timed-out fragments are resent at
// most a window at a time: resending them all together would overflow that buffer again.
//
// Inner packet formats (the first byte is the inner kind, see wire.h):
//   Reliable: [kind][seq u32][flags u8][fragment]      flags bit0 = more fragments follow
//   Ack:      [kind][next expected seq u32][sack u32]  sack bit i => seq (next+1+i) received

#include <QByteArray>
#include <QList>
#include <deque>
#include <functional>
#include <map>

namespace df {

class ReliableChannel
{
public:
    static constexpr int kMaxFragment = 1100;
    static constexpr int kWindow = 1024;      // max packets in flight / receive reordering
    static constexpr double kInitialCwnd = 16;
    static constexpr double kMinCwnd = 4;
    static constexpr double kMaxCwnd = 64;
    using Sender = std::function<void(const QByteArray &packet)>;

    explicit ReliableChannel(Sender sender);

    void send(const QByteArray &message, qint64 nowMs);
    // Handles a Reliable or Ack inner packet. Returns fully reassembled messages, in order.
    QList<QByteArray> receive(const QByteArray &packet, qint64 nowMs);
    // Retransmits timed-out fragments and pushes queued ones.
    void tick(qint64 nowMs);
    void reset();

    int unacked() const { return int(m_inFlight.size() + m_queue.size()); }
    int rtoMs() const { return m_rto; }
    double congestionWindow() const { return m_cwnd; }

private:
    struct Pending {
        QByteArray packet;
        qint64 sentAt = 0;
        qint64 firstSentAt = 0;
        int retries = 0;
        bool fastRetransmitted = false;
    };

    void flush(qint64 nowMs);
    void handleAck(quint32 next, quint32 sack, qint64 nowMs);
    void sendAck();
    void ackSample(const Pending &p, qint64 nowMs);
    void onLoss(quint32 seq);

    Sender m_sender;

    // Sending side
    quint32 m_nextSeq = 0;
    std::deque<QByteArray> m_queue; // packets not yet sent (window full)
    std::map<quint32, Pending> m_inFlight;
    int m_srtt = -1;
    int m_rto = 200;
    double m_cwnd = kInitialCwnd;
    double m_ssthresh = kWindow;
    quint32 m_recoverUntil = 0; // halve the window at most once per window of data

    // Receiving side
    quint32 m_expected = 0;
    std::map<quint32, QByteArray> m_outOfOrder; // seq -> [flags][fragment]
    QByteArray m_partial;
};

} // namespace df
