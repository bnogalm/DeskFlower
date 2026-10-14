// End-to-end test: two nodes on localhost pair with a PIN, open an encrypted session and exchange
// messages. Discovery is disabled; peers are found through a unicast beacon query.

#include "node.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
std::unique_ptr<Node> makeNode(const QString &dir)
{
    Node::Options o;
    o.configDir = dir;
    o.dataPort = 0;
    o.enableDiscovery = false;
    auto node = std::make_unique<Node>(o);
    QString error;
    if (!node->start(&error))
        qWarning() << error;
    return node;
}

bool knows(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return true;
    }
    return false;
}
} // namespace

class TestNode : public QObject
{
    Q_OBJECT
private slots:
    void pairConnectAndTalk()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(true);

        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);

        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(pinShown.size(), 1);
        QTRY_COMPARE(pinAsked.size(), 1);
        const QString pin = pinShown.first().at(2).toString();
        QCOMPARE(pin.size(), 6);
        QVERIFY(a->submitPin(b->id(), pin));

        QTRY_COMPARE(finishedA.size(), 1);
        QTRY_COMPARE(finishedB.size(), 1);
        QVERIFY(finishedA.first().at(1).toBool());
        QVERIFY(finishedB.first().at(1).toBool());

        QTRY_VERIFY(a->isConnected(b->id()));
        QTRY_VERIFY(b->isConnected(a->id()));

        QSignalSpy gotB(b.get(), &Node::messageReceived);
        QCborMap msg;
        msg.insert(QStringLiteral("t"), QStringLiteral("test"));
        msg.insert(QStringLiteral("big"), QByteArray(20000, 'x'));
        QVERIFY(a->sendMessage(b->id(), msg));
        QTRY_COMPARE(gotB.size(), 1);
        QCOMPARE(gotB.first().at(1).value<QCborMap>(), msg);

        QSignalSpy datagrams(a.get(), &Node::datagramReceived);
        QVERIFY(b->sendDatagram(a->id(), "mouse"));
        QTRY_COMPARE(datagrams.size(), 1);
        QCOMPARE(datagrams.first().at(1).toByteArray(), QByteArray("mouse"));

        // Restarting B (same identity) must reconnect automatically.
        const QByteArray idB = b->id();
        b.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!a->isConnected(idB), 10000);
        b = makeNode(dirB.path());
        QCOMPARE(b->id(), idB);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(idB) && b->isConnected(a->id()), 10000);

        QSignalSpy gotB2(b.get(), &Node::messageReceived);
        QVERIFY(a->sendMessage(idB, msg));
        QTRY_COMPARE(gotB2.size(), 1);

        // Unpairing on one side removes the trust on both.
        a->unpair(idB);
        QTRY_VERIFY(!b->isConnected(a->id()));
        bool stillTrusted = false;
        for (const PeerView &p : b->peers())
            stillTrusted |= p.id == a->id() && p.trusted;
        QVERIFY(!stillTrusted);
    }

    void wrongPinFails()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(true);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy pinShown(b.get(), &Node::pairingPinGenerated);
        QSignalSpy pinAsked(a.get(), &Node::pairingPinRequired);
        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QSignalSpy finishedB(b.get(), &Node::pairingFinished);
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(pinAsked.size(), 1);
        const QString pin = pinShown.first().at(2).toString();
        const QString wrong = QStringLiteral("%1").arg((pin.toInt() + 1) % 1000000, 6, 10, QLatin1Char('0'));
        QVERIFY(a->submitPin(b->id(), wrong));

        QTRY_COMPARE(finishedA.size(), 1);
        QTRY_COMPARE(finishedB.size(), 1);
        QVERIFY(!finishedA.first().at(1).toBool());
        QVERIFY(!finishedB.first().at(1).toBool());
        QTest::qWait(500);
        QVERIFY(!a->isConnected(b->id()));
    }

    void pairingRequiresExploration()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        b->setExploring(false);
        a->addManualPeer(QHostAddress::LocalHost, b->dataPort());
        QTRY_VERIFY(knows(*a, b->id()));

        QSignalSpy finishedA(a.get(), &Node::pairingFinished);
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(finishedA.size(), 1);
        QVERIFY(!finishedA.first().at(1).toBool());
    }
};

QTEST_GUILESS_MAIN(TestNode)
#include "tst_node.moc"
