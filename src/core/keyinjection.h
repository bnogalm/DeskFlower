#pragma once

// How the Windows helper turns keys into SendInput keyboard events and low-level hook events into
// keys (keymap.h: PS/2 set-1 make codes, 0xE0xx for extended keys). Pure functions with the
// Win32 values spelled out, so that they are unit tested on every platform; the backend
// (src/session/inputbackend_win.cpp) checks them against <windows.h>.
//
// Injection: by scan code (KEYEVENTF_SCANCODE), so that the target's own keyboard layout applies,
// except the Windows keys and the Menu key (0xE05B, 0xE05C, 0xE05D), which go by virtual key
// (VK_LWIN, VK_RWIN, VK_APPS) with their scan code and KEYEVENTF_EXTENDEDKEY: the shell's Win
// shortcuts (Win+D, Win+E, Win+R...) did not react to the Windows key injected by scan code alone.
//
// Capture: the hook's scan code and LLKHF_EXTENDED, except Right Shift (flagged extended, but
// E0 36 is not Shift when injected: 0x36) and the Windows and Menu keys, taken from the virtual
// key, since a keyboard (or keyboard software) may report them without the extended flag or with
// no scan code at all, and the target would then get some other key, or none.

#include <QtGlobal>

namespace df::wininput {

// Virtual keys (WinUser.h).
inline constexpr quint32 kVkRShift = 0xA1;
inline constexpr quint32 kVkLWin = 0x5B;
inline constexpr quint32 kVkRWin = 0x5C;
inline constexpr quint32 kVkApps = 0x5D;
// KEYBDINPUT::dwFlags (WinUser.h).
inline constexpr quint32 kExtendedKey = 0x0001; // KEYEVENTF_EXTENDEDKEY
inline constexpr quint32 kKeyUp = 0x0002;       // KEYEVENTF_KEYUP
inline constexpr quint32 kScanCode = 0x0008;    // KEYEVENTF_SCANCODE

// The fields of one KEYBDINPUT.
struct KeyInput {
    quint16 vk = 0;
    quint16 scan = 0;
    quint32 flags = 0;
    bool operator==(const KeyInput &) const = default;
};

// The virtual key a set-1 code is injected with (0: by scan code).
constexpr quint16 injectedVirtualKey(quint32 set1)
{
    switch (set1) {
    case 0xE05B: return quint16(kVkLWin);
    case 0xE05C: return quint16(kVkRWin);
    case 0xE05D: return quint16(kVkApps);
    default: return 0;
    }
}

constexpr KeyInput keyInput(quint32 set1, bool down)
{
    if (set1 == 0xE036)
        set1 = 0x36; // Right Shift as sent by older versions
    KeyInput in;
    in.scan = quint16(set1 & 0xFF);
    in.vk = injectedVirtualKey(set1);
    if (!in.vk)
        in.flags |= kScanCode;
    if (set1 & 0xFF00)
        in.flags |= kExtendedKey;
    if (!down)
        in.flags |= kKeyUp;
    return in;
}

// The set-1 code of a low-level hook event (KBDLLHOOKSTRUCT vkCode, scanCode, LLKHF_EXTENDED).
// 0 when it has none (no scan code: the backend may map the virtual key itself).
constexpr quint32 hookScancode(quint32 vk, quint32 scanCode, bool extended)
{
    switch (vk) {
    case kVkRShift: return 0x36;
    case kVkLWin: return 0xE05B;
    case kVkRWin: return 0xE05C;
    case kVkApps: return 0xE05D;
    default: break;
    }
    if ((scanCode & 0xFF) == 0)
        return 0;
    return (scanCode & 0xFF) | (extended ? 0xE000 : 0);
}

} // namespace df::wininput
