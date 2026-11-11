// How the Windows helper builds its keyboard injections and reads its hook (keyinjection.h, pure
// functions: no desktop, no input injected).

#include "keyinjection.h"

#include <QTest>

namespace wi = df::wininput;

class TestKeyInjection : public QObject
{
    Q_OBJECT
private slots:
    // The Windows keys and Menu go by virtual key with their scan code and the extended flag (the
    // shell's Win shortcuts ignore them by scan code alone); every other key by scan code.
    void winKeysAreInjectedByVirtualKey()
    {
        const wi::KeyInput lwin = wi::keyInput(0xE05B, true);
        QCOMPARE(lwin.vk, quint16(wi::kVkLWin));
        QCOMPARE(lwin.scan, quint16(0x5B));
        QCOMPARE(lwin.flags, wi::kExtendedKey);
        QCOMPARE(wi::keyInput(0xE05B, false).flags, wi::kExtendedKey | wi::kKeyUp);
        QCOMPARE(wi::keyInput(0xE05C, true), (wi::KeyInput{quint16(wi::kVkRWin), 0x5C, wi::kExtendedKey}));
        QCOMPARE(wi::keyInput(0xE05D, false),
                 (wi::KeyInput{quint16(wi::kVkApps), 0x5D, wi::kExtendedKey | wi::kKeyUp}));

        // D, by scan code; an extended key (Right Ctrl, the arrows) by scan code with the flag.
        QCOMPARE(wi::keyInput(0x20, true), (wi::KeyInput{0, 0x20, wi::kScanCode}));
        QCOMPARE(wi::keyInput(0x20, false), (wi::KeyInput{0, 0x20, wi::kScanCode | wi::kKeyUp}));
        QCOMPARE(wi::keyInput(0xE01D, true), (wi::KeyInput{0, 0x1D, wi::kScanCode | wi::kExtendedKey}));
        QCOMPARE(wi::keyInput(0xE048, true), (wi::KeyInput{0, 0x48, wi::kScanCode | wi::kExtendedKey}));
        // Right Shift from an older version (0xE036) is plain 0x36.
        QCOMPARE(wi::keyInput(0xE036, true), (wi::KeyInput{0, 0x36, wi::kScanCode}));
        // A key with a virtual key of its own is never sent as scan code too.
        for (quint32 sc : {0xE05Bu, 0xE05Cu, 0xE05Du})
            QVERIFY(!(wi::keyInput(sc, true).flags & wi::kScanCode));
    }

    // The hook's events become set-1 codes; the Windows and Menu keys by virtual key whatever the
    // keyboard reported (no extended flag, no scan code).
    void hookEventsBecomeScancodes()
    {
        QCOMPARE(wi::hookScancode(wi::kVkLWin, 0x5B, true), 0xE05Bu);
        QCOMPARE(wi::hookScancode(wi::kVkLWin, 0x5B, false), 0xE05Bu);
        QCOMPARE(wi::hookScancode(wi::kVkLWin, 0, false), 0xE05Bu);
        QCOMPARE(wi::hookScancode(wi::kVkRWin, 0, false), 0xE05Cu);
        QCOMPARE(wi::hookScancode(wi::kVkApps, 0x5D, false), 0xE05Du);
        QCOMPARE(wi::hookScancode(wi::kVkRShift, 0x36, true), 0x36u);
        QCOMPARE(wi::hookScancode('D', 0x20, false), 0x20u);
        QCOMPARE(wi::hookScancode(0x25 /* VK_LEFT */, 0x4B, true), 0xE04Bu);
        QCOMPARE(wi::hookScancode('D', 0, false), 0u); // the backend maps the virtual key itself
    }
};

QTEST_MAIN(TestKeyInjection)
#include "tst_keyinjection.moc"
