#pragma once

// Key code translation. Keys travel between devices as PS/2 scan code set 1 "make" codes, with
// 0xE0xx for extended keys, exactly as the Windows low-level keyboard hook reports them (which
// includes its quirks: NumLock is 0xE045 and Pause is 0x0045; the one exception is Right Shift, which
// the hook flags as extended but always travels as 0x36). Each platform maps its own codes:
//  - Linux: evdev key codes (X11 key codes are evdev + 8; libei uses evdev codes directly).
//  - macOS: virtual key codes (kVK_* in Carbon's HIToolbox/Events.h).
// Unknown codes map to 0 (set 1) or -1 (platform).
//
// Paste evidence (ClipboardSync): a press that can be the user asking for a paste, judged on set-1
// codes at both ends (keys the master forwards, presses the local helper sees). `held` is the set
// of codes already down when `pressed` goes down (any set with contains(): QSet, std::set).
//  - isPasteChord: Ctrl+V, Shift+Insert, Ctrl+Shift+V and, when cmdPastes (the keys end up on a
//    Mac), Cmd+V. Command travels as the Windows key's codes; elsewhere Win+V opens the clipboard
//    history and pastes nothing. V alone is no chord. Off a Mac, Shift with the keypad's 0 (Ins
//    while Num Lock is off) counts as Shift+Insert too; on a Mac that key is only keypad 0.
//  - isPasteGestureKey: Enter (main or keypad), the Apps (context menu) key and Shift+F10, the
//    keys that pick Paste in a menu or confirm a dialog that pastes.
// Buttons are judged by their callers (any button press is a gesture; motion and the wheel never).

#include <QtGlobal>

namespace df::keymap {

quint32 set1FromEvdev(int evdev);
int evdevFromSet1(quint32 set1);

quint32 set1FromMac(int macKeyCode);
int macFromSet1(quint32 set1);

namespace paste {
inline constexpr quint32 kLeftCtrl = 0x1D, kRightCtrl = 0xE01D, kLeftShift = 0x2A, kRightShift = 0x36;
inline constexpr quint32 kLeftCmd = 0xE05B, kRightCmd = 0xE05C, kV = 0x2F, kInsert = 0xE052;
inline constexpr quint32 kKeypadInsert = 0x52; // keypad 0
inline constexpr quint32 kEnter = 0x1C, kKeypadEnter = 0xE01C, kApps = 0xE05D, kF10 = 0x44;
} // namespace paste

template <typename Held>
bool isPasteChord(const Held &held, quint32 pressed, bool cmdPastes)
{
    using namespace paste;
    const bool ctrl = held.contains(kLeftCtrl) || held.contains(kRightCtrl)
                      || (cmdPastes && (held.contains(kLeftCmd) || held.contains(kRightCmd)));
    const bool shift = held.contains(kLeftShift) || held.contains(kRightShift);
    return (pressed == kV && ctrl) || (shift && (pressed == kInsert || (pressed == kKeypadInsert && !cmdPastes)));
}

template <typename Held>
bool isPasteGestureKey(const Held &held, quint32 pressed)
{
    using namespace paste;
    if (pressed == kEnter || pressed == kKeypadEnter || pressed == kApps)
        return true;
    return pressed == kF10 && (held.contains(kLeftShift) || held.contains(kRightShift));
}

// Either of the above: a key press that counts as paste evidence.
template <typename Held>
bool isPasteEvidenceKey(const Held &held, quint32 pressed, bool cmdPastes)
{
    return isPasteChord(held, pressed, cmdPastes) || isPasteGestureKey(held, pressed);
}

} // namespace df::keymap
