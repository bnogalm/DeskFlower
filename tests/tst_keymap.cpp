#include "keymap.h"

#include <QSet>
#include <QTest>

using namespace df::keymap;

class TestKeymap : public QObject
{
    Q_OBJECT
private slots:
    void evdevKnownKeys()
    {
        QCOMPARE(set1FromEvdev(1), 0x01u);    // Esc
        QCOMPARE(set1FromEvdev(30), 0x1Eu);   // A
        QCOMPARE(set1FromEvdev(28), 0x1Cu);   // Enter
        QCOMPARE(set1FromEvdev(96), 0xE01Cu); // Keypad Enter
        QCOMPARE(set1FromEvdev(103), 0xE048u); // Up
        QCOMPARE(set1FromEvdev(125), 0xE05Bu); // Left Meta / Windows
        QCOMPARE(set1FromEvdev(69), 0xE045u); // NumLock, as the Windows hook reports it
        QCOMPARE(set1FromEvdev(119), 0x45u);  // Pause, idem
        QCOMPARE(set1FromEvdev(9999), 0u);
    }

    void evdevRoundTrip()
    {
        QSet<quint32> seen;
        for (int code = 0; code < 256; ++code) {
            const quint32 set1 = set1FromEvdev(code);
            if (!set1)
                continue;
            QVERIFY2(!seen.contains(set1), qPrintable(QString::number(code)));
            seen.insert(set1);
            QCOMPARE(evdevFromSet1(set1), code);
        }
        QVERIFY(seen.size() > 120);
    }

    void macKnownKeys()
    {
        QCOMPARE(set1FromMac(0x00), 0x1Eu);   // A
        QCOMPARE(set1FromMac(0x24), 0x1Cu);   // Return
        QCOMPARE(set1FromMac(0x37), 0xE05Bu); // Command -> Windows key
        QCOMPARE(set1FromMac(0x3A), 0x38u);   // Option -> Alt
        QCOMPARE(set1FromMac(0x7E), 0xE048u); // Up
        QCOMPARE(set1FromMac(0x3F), 0u);      // Fn has no PC equivalent
    }

    void macRoundTrip()
    {
        QSet<quint32> seen;
        int mapped = 0;
        for (int code = 0; code < 128; ++code) {
            const quint32 set1 = set1FromMac(code);
            if (!set1)
                continue;
            ++mapped;
            QVERIFY2(!seen.contains(set1), qPrintable(QString::number(code, 16)));
            seen.insert(set1);
            QCOMPARE(macFromSet1(set1), code);
        }
        QVERIFY(mapped > 110);
    }

    void rightShiftWithTheExtendedFlag()
    {
        // Older Windows masters sent Right Shift as E0 36 (the hook flags it as extended).
        QCOMPARE(set1FromEvdev(54), 0x36u);
        QCOMPARE(evdevFromSet1(0xE036), 54);
        QCOMPARE(macFromSet1(0xE036), 0x3C);
    }

    void macAndLinuxAgreeThroughSet1()
    {
        // A key typed on a Mac lands on the same physical key on Linux.
        QCOMPARE(evdevFromSet1(set1FromMac(0x00)), 30);  // A
        QCOMPARE(evdevFromSet1(set1FromMac(0x7B)), 105); // Left
        QCOMPARE(evdevFromSet1(set1FromMac(0x3D)), 100); // Right Option -> Right Alt
        QCOMPARE(macFromSet1(set1FromEvdev(125)), 0x37); // Meta -> Command
    }
};

QTEST_GUILESS_MAIN(TestKeymap)
#include "tst_keymap.moc"
