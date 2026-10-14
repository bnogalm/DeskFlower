#include "reliable.h"

#include <QRandomGenerator>
#include <QTest>

using namespace df;

namespace {
struct InFlight {
    qint64 deliverAt;
    QByteArray data;
};

// Simulated lossy link with random delay (reordering) and loss.
class Link
{
public:
    // capacity: packets the link can hold in flight (a socket buffer); beyond that it drops.
    Link(double loss, quint32 seed, int capacity = 1 << 30) : m_loss(loss), m_rng(seed), m_capacity(capacity) {}
    void push(const QByteArray &packet, qint64 now)
    {
        if (m_rng.generateDouble() < m_loss || int(m_queue.size()) >= m_capacity)
            return;
        m_queue.push_back({now + 5 + m_rng.bounded(40), packet});
    }
    QList<QByteArray> due(qint64 now)
    {
        QList<QByteArray> out;
        for (auto it = m_queue.begin(); it != m_queue.end();) {
            if (it->deliverAt <= now) {
                out.append(it->data);
                it = m_queue.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

private:
    double m_loss;
    QRandomGenerator m_rng;
    int m_capacity;
    std::vector<InFlight> m_queue;
};
} // namespace

class TestReliable : public QObject
{
    Q_OBJECT
private slots:
    void inOrderOverLossyLink_data()
    {
        QTest::addColumn<double>("loss");
        QTest::newRow("no loss") << 0.0;
        QTest::newRow("10% loss") << 0.1;
        QTest::newRow("40% loss") << 0.4;
    }

    void inOrderOverLossyLink()
    {
        QFETCH(double, loss);
        qint64 now = 0;
        Link ab(loss, 1), ba(loss, 2);
        ReliableChannel a([&](const QByteArray &p) { ab.push(p, now); });
        ReliableChannel b([&](const QByteArray &p) { ba.push(p, now); });

        QList<QByteArray> sent;
        for (int i = 0; i < 300; ++i) {
            const int size = (i % 37 == 0) ? 5000 + i : 10 + i % 50; // some messages need fragmentation
            QByteArray msg(size, char('a' + i % 26));
            msg.prepend(QByteArray::number(i) + ':');
            sent.append(msg);
            a.send(msg, now);
        }

        QList<QByteArray> received;
        for (; now < 60000 && received.size() < sent.size(); now += 5) {
            for (const QByteArray &p : ab.due(now))
                received += b.receive(p, now);
            for (const QByteArray &p : ba.due(now))
                a.receive(p, now);
            a.tick(now);
            b.tick(now);
        }
        QCOMPARE(received.size(), sent.size());
        QCOMPARE(received, sent);

        // Let the final acks arrive.
        for (int i = 0; i < 2000 && a.unacked() > 0; ++i, now += 5) {
            for (const QByteArray &p : ab.due(now))
                b.receive(p, now);
            for (const QByteArray &p : ba.due(now))
                a.receive(p, now);
            a.tick(now);
        }
        QCOMPARE(a.unacked(), 0);
    }

    void bulkTransferThroughSmallBuffer()
    {
        // 2 MB through a link that holds at most 48 packets (~53 KB): without congestion control
        // most of a full window would be dropped and recovered only by timeouts.
        qint64 now = 0;
        Link ab(0.0, 3, 48), ba(0.0, 4);
        ReliableChannel a([&](const QByteArray &p) { ab.push(p, now); });
        ReliableChannel b([&](const QByteArray &p) { ba.push(p, now); });
        const QByteArray big(2 * 1024 * 1024, 'z');
        a.send(big, now);
        QList<QByteArray> received;
        for (; now < 60000 && received.isEmpty(); now += 1) {
            for (const QByteArray &p : ab.due(now))
                received += b.receive(p, now);
            for (const QByteArray &p : ba.due(now))
                a.receive(p, now);
            a.tick(now);
        }
        QCOMPARE(received.size(), 1);
        QCOMPARE(received.first(), big);
        // ~1900 packets at <= 48 per ~25 ms round trip: about one second of simulated time.
        qInfo() << "2 MB delivered in" << now << "ms of simulated time";
        QVERIFY2(now < 3000, qPrintable(QString::number(now)));
    }

    void emptyMessage()
    {
        QList<QByteArray> got;
        ReliableChannel *bp = nullptr;
        ReliableChannel b([](const QByteArray &) {});
        bp = &b;
        ReliableChannel a([&](const QByteArray &p) { got += bp->receive(p, 0); });
        a.send(QByteArray(), 0);
        QCOMPARE(got.size(), 1);
        QVERIFY(got.first().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestReliable)
#include "tst_reliable.moc"
