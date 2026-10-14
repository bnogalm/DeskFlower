// End-to-end test: two nodes on localhost pair with a PIN, open an encrypted session and exchange
// messages. Discovery is disabled; peers are found through a unicast beacon query.

#include "node.h"
#include "pairing.h"
#include "testutil.h"
#include "wire.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>

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

    void repeatedWrongPinsBlockPairing()
    {
        // Each request gets a new PIN; after 3 failed attempts B refuses requests for 5 minutes,
        // even with the right PIN, and after the first failure its answers wait 300 ms.
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
        for (int attempt = 1; attempt <= PairingManager::kMaxFailedAttempts; ++attempt) {
            QVERIFY(a->requestPairing(b->id()));
            QTRY_COMPARE(pinShown.size(), attempt);
            QTRY_COMPARE(pinAsked.size(), attempt); // A got the challenge and waits for the PIN
            const QString pin = pinShown.last().at(2).toString();
            const QString wrong = QStringLiteral("%1").arg((pin.toInt() + 1) % 1000000, 6, 10, QLatin1Char('0'));
            QElapsedTimer answer;
            answer.start();
            QVERIFY(a->submitPin(b->id(), wrong));
            QTRY_COMPARE(finishedA.size(), attempt);
            QTRY_COMPARE(finishedB.size(), attempt);
            if (attempt > 1)
                QVERIFY2(answer.elapsed() >= PairingManager::kRetryDelayMs, qPrintable(QString::number(answer.elapsed())));
        }
        QVERIFY(finishedB.last().at(2).toString().contains(QStringLiteral("blocked")));

        // Locked: no PIN is shown any more, and the request fails.
        QVERIFY(a->requestPairing(b->id()));
        QTRY_COMPARE(finishedA.size(), PairingManager::kMaxFailedAttempts + 1);
        QVERIFY(!finishedA.last().at(1).toBool());
        QVERIFY2(finishedA.last().at(2).toString().contains(QStringLiteral("Too many")),
                 qPrintable(finishedA.last().at(2).toString()));
        QCOMPARE(pinShown.size(), PairingManager::kMaxFailedAttempts);
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

    void pairingsBelongToTheUserWhoMadeThem()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        a->setLocalUser(QStringLiteral("alice"));
        QVERIFY(test::pairNodes(*a, *b));

        // Another user signs in on A: B is out of reach, and not theirs to see, use or remove.
        a->setLocalUser(QStringLiteral("bob"));
        QVERIFY(!a->isConnected(b->id()));
        QVERIFY(!a->sendMessage(b->id(), QCborMap{{QStringLiteral("t"), QStringLiteral("x")}}));
        QVERIFY(!a->openStream(b->id()));
        bool otherUser = false;
        for (const PeerView &p : a->peers())
            otherUser |= p.id == b->id() && p.otherUser;
        QVERIFY(otherUser);
        a->unpair(b->id());
        QVERIFY(a->isTrusted(b->id()));
        QTest::qWait(1500); // B keeps trying: A does not answer
        QVERIFY(!a->isConnected(b->id()));

        // Back to the user who paired them: they reconnect.
        a->setLocalUser(QStringLiteral("alice"));
        QTRY_VERIFY_WITH_TIMEOUT(a->isConnected(b->id()) && b->isConnected(a->id()), 10000);

        // Nobody signed in (the sign-in screen): nothing either.
        a->setLocalUser(QString());
        QVERIFY(!a->isConnected(b->id()));

        // Pairings made before users were told apart go to the first user seen.
        QTemporaryDir dirC, dirD;
        auto c = makeNode(dirC.path());
        auto d = makeNode(dirD.path());
        QVERIFY(test::pairNodes(*c, *d));
        c->setLocalUser(QStringLiteral("carol"));
        QVERIFY(c->isConnected(d->id()));
        c->setLocalUser(QStringLiteral("dave"));
        QVERIFY(!c->isConnected(d->id()));
    }

    void beaconsCannotRenameOrFloodPeers()
    {
        QTemporaryDir dirA, dirB;
        auto a = makeNode(dirA.path());
        auto b = makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        auto nameOf = [&](const QByteArray &id) {
            for (const PeerView &p : a->peers()) {
                if (p.id == id)
                    return p.name;
            }
            return QString();
        };
        const QString name = nameOf(b->id());
        QVERIFY(!name.isEmpty());

        auto beacon = [](const QByteArray &id, const QByteArray &key, const QString &name) {
            QCborMap m;
            m.insert(QStringLiteral("i"), id);
            m.insert(QStringLiteral("n"), name);
            m.insert(QStringLiteral("k"), key);
            m.insert(QStringLiteral("o"), QStringLiteral("windows"));
            return wire::cborPacket(wire::Type::Beacon, m);
        };
        QUdpSocket attacker;
        QVERIFY(attacker.bind(QHostAddress::LocalHost, 0));
        // An unauthenticated beacon with B's identity and another name.
        QByteArray bKey;
        {
            QFile f(QDir(dirB.path()).filePath(QStringLiteral("identity.json")));
            QVERIFY(f.open(QIODevice::ReadOnly));
            bKey = crypto::publicKeyFromSecret(QByteArray::fromBase64(
                QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("secretKey")).toString().toLatin1()));
        }
        QCOMPARE(crypto::deviceIdFromPublicKey(bKey), b->id());
        attacker.writeDatagram(beacon(b->id(), bKey, QStringLiteral("Evil")), QHostAddress::LocalHost, a->dataPort());
        QTest::qWait(300);
        QCOMPARE(nameOf(b->id()), name);

        // Hundreds of made-up devices: only a bounded number is kept.
        for (int i = 0; i < 300; ++i) {
            const crypto::KeyPair k = crypto::generateKeyPair();
            attacker.writeDatagram(beacon(crypto::deviceIdFromPublicKey(k.publicKey), k.publicKey,
                                          QStringLiteral("fake %1").arg(i)),
                                   QHostAddress::LocalHost, a->dataPort());
        }
        QTest::qWait(500);
        QVERIFY2(a->peers().size() <= 65, qPrintable(QString::number(a->peers().size())));
        QVERIFY(a->isConnected(b->id()));
    }
};

QTEST_GUILESS_MAIN(TestNode)
#include "tst_node.moc"
