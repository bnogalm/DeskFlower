#include "cpace.h"
#include "crypto.h"
#include "noise.h"
#include "session.h"

#include <QTest>

using namespace df;

class TestCrypto : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }

    void aeadRoundTrip()
    {
        const QByteArray key = crypto::randomBytes(32);
        const QByteArray ct = crypto::aeadEncrypt(key, 7, "ad", "hello");
        QCOMPARE(crypto::aeadDecrypt(key, 7, "ad", ct).value_or(QByteArray()), QByteArray("hello"));
        QVERIFY(!crypto::aeadDecrypt(key, 8, "ad", ct));
        QVERIFY(!crypto::aeadDecrypt(key, 7, "xx", ct));
    }

    // Sealing a clipboard format under a one-off key: a random nonce per seal, bound to its id and
    // mime, decrypted only with the same key and ad.
    void sealRoundTrip()
    {
        const QByteArray key = crypto::sealKey();
        QCOMPARE(key.size(), crypto::kKeySize);
        const QByteArray ad = QByteArray("kid-16-bytes----") + QByteArray("text/plain");
        const QByteArray plain = QByteArray("a copied password");
        const QByteArray sealed = crypto::seal(key, ad, plain);
        QVERIFY(sealed.size() == plain.size() + crypto::kSealOverhead);
        QCOMPARE(crypto::unseal(key, ad, sealed).value_or(QByteArray()), plain);
        // Each seal uses a fresh nonce, so the same plaintext seals differently.
        QVERIFY(crypto::seal(key, ad, plain) != sealed);
        // Wrong key, wrong ad (another id or mime) or tampered bytes do not decrypt.
        QVERIFY(!crypto::unseal(crypto::sealKey(), ad, sealed));
        QVERIFY(!crypto::unseal(key, ad + "x", sealed));
        QByteArray tampered = sealed;
        tampered[tampered.size() - 1] = char(tampered.back() ^ 1);
        QVERIFY(!crypto::unseal(key, ad, tampered));
        QVERIFY(!crypto::unseal(key, ad, QByteArray(8, '\0'))); // too short
    }

    void noiseHandshake()
    {
        const auto alice = crypto::generateKeyPair();
        const auto bob = crypto::generateKeyPair();

        noise::IKInitiator init(alice, bob.publicKey, "test");
        const QByteArray m1 = init.writeMessage1("hi bob");
        QVERIFY(!m1.isEmpty());

        noise::IKResponder resp(bob, "test");
        QByteArray payload;
        const auto rs = resp.readMessage1(m1, &payload);
        QVERIFY(rs);
        QCOMPARE(*rs, alice.publicKey);
        QCOMPARE(payload, QByteArray("hi bob"));

        QByteArray m2;
        const auto bobKeys = resp.writeMessage2("hi alice", &m2);
        QVERIFY(bobKeys);
        const auto aliceKeys = init.readMessage2(m2, &payload);
        QVERIFY(aliceKeys);
        QCOMPARE(payload, QByteArray("hi alice"));
        QCOMPARE(aliceKeys->sendKey, bobKeys->recvKey);
        QCOMPARE(aliceKeys->recvKey, bobKeys->sendKey);
        QVERIFY(aliceKeys->sendKey != aliceKeys->recvKey);
        QCOMPARE(aliceKeys->handshakeHash, bobKeys->handshakeHash);
    }

    void noiseRejectsWrongResponderKey()
    {
        const auto alice = crypto::generateKeyPair();
        const auto bob = crypto::generateKeyPair();
        const auto mallory = crypto::generateKeyPair();
        noise::IKInitiator init(alice, bob.publicKey, "test");
        const QByteArray m1 = init.writeMessage1({});
        noise::IKResponder resp(mallory, "test");
        QVERIFY(!resp.readMessage1(m1, nullptr));
    }

    void noiseRejectsTampering()
    {
        const auto alice = crypto::generateKeyPair();
        const auto bob = crypto::generateKeyPair();
        noise::IKInitiator init(alice, bob.publicKey, "test");
        QByteArray m1 = init.writeMessage1("x");
        m1[40] = char(m1[40] ^ 1);
        noise::IKResponder resp(bob, "test");
        QVERIFY(!resp.readMessage1(m1, nullptr));
    }

    void noiseInitiatorSurvivesAForgedResponse()
    {
        // Anyone can send a message 2 to a pending initiator (its index travels in clear): one
        // that does not authenticate must leave the initiator able to read the genuine one.
        const auto alice = crypto::generateKeyPair();
        const auto bob = crypto::generateKeyPair();
        noise::IKInitiator init(alice, bob.publicKey, "test");
        const QByteArray m1 = init.writeMessage1("hi bob");
        noise::IKResponder resp(bob, "test");
        QVERIFY(resp.readMessage1(m1, nullptr));
        QByteArray m2;
        const auto bobKeys = resp.writeMessage2("hi alice", &m2);
        QVERIFY(bobKeys);

        QVERIFY(!init.readMessage2(crypto::randomBytes(m2.size()), nullptr)); // forged
        QByteArray tampered = m2;
        tampered[tampered.size() - 1] = char(tampered[tampered.size() - 1] ^ 1);
        QVERIFY(!init.readMessage2(tampered, nullptr)); // corrupted
        QByteArray payload;
        const auto aliceKeys = init.readMessage2(m2, &payload);
        QVERIFY(aliceKeys);
        QCOMPARE(payload, QByteArray("hi alice"));
        QCOMPARE(aliceKeys->sendKey, bobKeys->recvKey);
        QCOMPARE(aliceKeys->recvKey, bobKeys->sendKey);
        QCOMPARE(aliceKeys->handshakeHash, bobKeys->handshakeHash);
    }

    void cpaceSamePin()
    {
        const QByteArray sid = crypto::randomBytes(16);
        CPace a("123456", sid, "channel");
        CPace b("123456", sid, "channel");
        const auto ka = a.finish(b.share(), true);
        const auto kb = b.finish(a.share(), false);
        QVERIFY(ka && kb);
        QCOMPARE(*ka, *kb);
        QCOMPARE(CPace::confirmationTag(*ka, 'A'), CPace::confirmationTag(*kb, 'A'));
    }

    void cpaceDifferentPinOrChannel()
    {
        const QByteArray sid = crypto::randomBytes(16);
        {
            CPace a("123456", sid, "channel");
            CPace b("123457", sid, "channel");
            QVERIFY(*a.finish(b.share(), true) != *b.finish(a.share(), false));
        }
        {
            CPace a("123456", sid, "channel-A");
            CPace b("123456", sid, "channel-B");
            QVERIFY(*a.finish(b.share(), true) != *b.finish(a.share(), false));
        }
    }

    void cpaceRejectsInvalidShares()
    {
        CPace a("123456", crypto::randomBytes(16), "c");
        QVERIFY(!a.finish(QByteArray(32, '\0'), true));
        QVERIFY(!a.finish(a.share(), true));
        QVERIFY(!a.finish(QByteArray(5, 'x'), true));
    }

    void replayWindow()
    {
        ReplayWindow w;
        auto accept = [&](quint64 c) {
            if (!w.check(c))
                return false;
            w.update(c);
            return true;
        };
        QVERIFY(accept(0));
        QVERIFY(!accept(0));
        QVERIFY(accept(5));
        QVERIFY(accept(3));
        QVERIFY(!accept(3));
        QVERIFY(accept(100));
        QVERIFY(!accept(5)); // too old now
        QVERIFY(accept(99));
    }

    void sessionSealOpen()
    {
        noise::TransportKeys a{crypto::randomBytes(32), crypto::randomBytes(32), {}};
        noise::TransportKeys b{a.recvKey, a.sendKey, {}};
        SecureSession sa(1, 2, a, true, 0);
        SecureSession sb(2, 1, b, false, 0);
        const QByteArray packet = sa.seal("payload", 0);
        QCOMPARE(sb.open(packet, 0).value_or(QByteArray()), QByteArray("payload"));
        QVERIFY(!sb.open(packet, 0)); // replay
        QByteArray tampered = sa.seal("payload", 0);
        tampered[3] = char(tampered[3] ^ 1); // header is authenticated
        QVERIFY(!sb.open(tampered, 0));
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "tst_crypto.moc"
