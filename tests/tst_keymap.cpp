#include "keymap.h"

#include <QSet>
#include <QTest>

#include <set>

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

    // Paste chords (paste evidence): Ctrl+V, Shift+Insert, Ctrl+Shift+V, either hand's modifiers;
    // Command (the Windows key's codes) only where Command pastes; V or Insert alone never;
    // Shift with the keypad's Ins off a Mac.
    void pasteChords()
    {
        const quint32 lCtrl = 0x1D, rCtrl = 0xE01D, lShift = 0x2A, rShift = 0x36, lWin = 0xE05B, rWin = 0xE05C;
        const quint32 v = 0x2F, insert = 0xE052, keypadInsert = 0x52, a = 0x1E, alt = 0x38;
        for (const bool mac : {false, true}) {
            QVERIFY(isPasteChord(QSet<quint32>{lCtrl}, v, mac));
            QVERIFY(isPasteChord(QSet<quint32>{rCtrl}, v, mac)); // right-hand Control
            QVERIFY(isPasteChord(QSet<quint32>{lShift}, insert, mac));
            QVERIFY(isPasteChord(QSet<quint32>{rShift}, insert, mac)); // right-hand Shift
            QVERIFY(isPasteChord(QSet<quint32>{lCtrl, lShift}, v, mac));
            QVERIFY(isPasteChord(QSet<quint32>{rCtrl, rShift}, v, mac));
            QVERIFY(!isPasteChord(QSet<quint32>{}, v, mac));      // V alone
            QVERIFY(!isPasteChord(QSet<quint32>{}, insert, mac)); // Insert alone
            QVERIFY(!isPasteChord(QSet<quint32>{lShift}, v, mac)); // a capital V
            QVERIFY(!isPasteChord(QSet<quint32>{alt}, v, mac));
            QVERIFY(!isPasteChord(QSet<quint32>{lCtrl}, a, mac));
            QVERIFY(!isPasteChord(QSet<quint32>{lCtrl}, lCtrl, mac)); // Control's own press
            // Win+V opens the clipboard history on Windows; Command+V pastes on a Mac.
            QCOMPARE(isPasteChord(QSet<quint32>{lWin}, v, mac), mac);
            QCOMPARE(isPasteChord(QSet<quint32>{rWin}, v, mac), mac);
            QVERIFY(!isPasteChord(QSet<quint32>{lWin}, a, mac));
            // Shift with the keypad's 0 (Ins with Num Lock off) pastes, except on a Mac.
            QCOMPARE(isPasteChord(QSet<quint32>{lShift}, keypadInsert, mac), !mac);
            QCOMPARE(isPasteChord(QSet<quint32>{rShift}, keypadInsert, mac), !mac);
            QVERIFY(!isPasteChord(QSet<quint32>{}, keypadInsert, mac)); // keypad 0 alone
        }
        // The backends' std::set works the same.
        QVERIFY(isPasteChord(std::set<quint32>{lCtrl}, v, false));
        QVERIFY(!isPasteChord(std::set<quint32>{}, v, false));
    }

    // Gesture keys: Enter (main and keypad, whatever is held), the Apps key, Shift+F10 (either
    // Shift); F10 alone, letters and modifiers are not.
    void pasteGestureKeys()
    {
        const quint32 lShift = 0x2A, rShift = 0x36, lCtrl = 0x1D, f10 = 0x44;
        QVERIFY(isPasteGestureKey(QSet<quint32>{}, 0x1C));       // Enter
        QVERIFY(isPasteGestureKey(QSet<quint32>{}, 0xE01C));     // keypad Enter
        QVERIFY(isPasteGestureKey(QSet<quint32>{lCtrl}, 0x1C));  // Ctrl+Enter
        QVERIFY(isPasteGestureKey(QSet<quint32>{}, 0xE05D));     // Apps (context menu)
        QVERIFY(isPasteGestureKey(QSet<quint32>{lShift}, f10));  // Shift+F10
        QVERIFY(isPasteGestureKey(QSet<quint32>{rShift}, f10));
        QVERIFY(!isPasteGestureKey(QSet<quint32>{}, f10));       // F10 alone (the menu bar)
        QVERIFY(!isPasteGestureKey(QSet<quint32>{lCtrl}, f10));
        QVERIFY(!isPasteGestureKey(QSet<quint32>{}, 0x2F));      // V
        QVERIFY(!isPasteGestureKey(QSet<quint32>{}, 0x1E));      // A
        QVERIFY(!isPasteGestureKey(QSet<quint32>{}, lShift));    // a modifier alone
        QVERIFY(!isPasteGestureKey(QSet<quint32>{}, 0xE05B));    // the Windows key
        QVERIFY(isPasteGestureKey(std::set<quint32>{lShift}, f10));
        // Either kind is evidence.
        QVERIFY(isPasteEvidenceKey(QSet<quint32>{lCtrl}, 0x2F, false));
        QVERIFY(isPasteEvidenceKey(QSet<quint32>{}, 0x1C, false));
        QVERIFY(!isPasteEvidenceKey(QSet<quint32>{}, 0x2F, true));
    }
};

QTEST_GUILESS_MAIN(TestKeymap)
#include "tst_keymap.moc"
