// The agent's protocol alerts (src/agent/modeltypes.h, ProtocolAlerts): a paired device of another
// protocol is announced once per device and protocol, however often the service's state repeats it.

#include "modeltypes.h"

#include <QCborMap>
#include <QTest>

class TestProtocolAlert : public QObject
{
    Q_OBJECT
private slots:
    void stateCarriesTheProtocolFields()
    {
        QCborMap m;
        m.insert(QStringLiteral("id"), QStringLiteral("abcd"));
        m.insert(QStringLiteral("protocol"), 2);
        m.insert(QStringLiteral("protocolMismatch"), QStringLiteral("older"));
        m.insert(QStringLiteral("canServeUpdate"), true);
        const PeerInfo p = PeerInfo::fromCbor(m);
        QCOMPARE(p.protocol, 2);
        QCOMPARE(p.protocolMismatch, QStringLiteral("older"));
        QVERIFY(p.canServeUpdate);
        // An older service leaves them out: nothing to alert.
        const PeerInfo none = PeerInfo::fromCbor(QCborMap{{QStringLiteral("id"), QStringLiteral("x")}});
        QCOMPARE(none.protocol, 0);
        QVERIFY(none.protocolMismatch.isEmpty());
        QVERIFY(!none.canServeUpdate);
    }

    void alertedOncePerDeviceAndProtocol()
    {
        auto peer = [](const char *id, int protocol, const char *mismatch) {
            PeerInfo p;
            p.id = QString::fromLatin1(id);
            p.protocol = protocol;
            p.protocolMismatch = QString::fromLatin1(mismatch);
            return p;
        };
        ProtocolAlerts alerts;
        const QList<PeerInfo> first = alerts.fresh({peer("a", 2, "older"), peer("b", 3, ""), peer("c", 4, "newer")});
        QCOMPARE(first.size(), 2);
        QCOMPARE(first.at(0).id, QStringLiteral("a"));
        QCOMPARE(first.at(1).id, QStringLiteral("c"));
        // The same state again (every state event repeats it): nothing new.
        QVERIFY(alerts.fresh({peer("a", 2, "older"), peer("b", 3, ""), peer("c", 4, "newer")}).isEmpty());
        // Gone and back with the same protocol: still told already.
        QVERIFY(alerts.fresh({}).isEmpty());
        QVERIFY(alerts.fresh({peer("a", 2, "older")}).isEmpty());
        // Updated, but to yet another protocol: told again.
        const QList<PeerInfo> again = alerts.fresh({peer("a", 4, "newer")});
        QCOMPARE(again.size(), 1);
        QCOMPARE(again.at(0).protocol, 4);
        // Updated to this device's protocol: no alert.
        QVERIFY(alerts.fresh({peer("a", 3, "")}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestProtocolAlert)
#include "tst_protocolalert.moc"
