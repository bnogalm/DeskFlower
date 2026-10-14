#include "keymap.h"

#include <array>
#include <utility>

namespace df::keymap {

namespace {

// evdev codes 1..88 are the set 1 make codes themselves (KEY_ESC = 1 ... KEY_F12 = 88), except
// the ones listed in kEvdevExceptions.
constexpr std::pair<int, quint32> kEvdevExceptions[] = {
    {69, 0xE045},  // KEY_NUMLOCK (Windows hook quirk)
};

// evdev codes above 88 (and other non-identity cases).
constexpr std::pair<int, quint32> kEvdevExtra[] = {
    {89, 0x73},    // KEY_RO
    {92, 0x79},    // KEY_HENKAN
    {93, 0x70},    // KEY_KATAKANAHIRAGANA
    {94, 0x7B},    // KEY_MUHENKAN
    {96, 0xE01C},  // KEY_KPENTER
    {97, 0xE01D},  // KEY_RIGHTCTRL
    {98, 0xE035},  // KEY_KPSLASH
    {99, 0xE037},  // KEY_SYSRQ (Print Screen)
    {100, 0xE038}, // KEY_RIGHTALT
    {102, 0xE047}, // KEY_HOME
    {103, 0xE048}, // KEY_UP
    {104, 0xE049}, // KEY_PAGEUP
    {105, 0xE04B}, // KEY_LEFT
    {106, 0xE04D}, // KEY_RIGHT
    {107, 0xE04F}, // KEY_END
    {108, 0xE050}, // KEY_DOWN
    {109, 0xE051}, // KEY_PAGEDOWN
    {110, 0xE052}, // KEY_INSERT
    {111, 0xE053}, // KEY_DELETE
    {113, 0xE020}, // KEY_MUTE
    {114, 0xE02E}, // KEY_VOLUMEDOWN
    {115, 0xE030}, // KEY_VOLUMEUP
    {117, 0x59},   // KEY_KPEQUAL
    {119, 0x0045}, // KEY_PAUSE (Windows hook quirk)
    {121, 0x7E},   // KEY_KPCOMMA
    {124, 0x7D},   // KEY_YEN
    {125, 0xE05B}, // KEY_LEFTMETA
    {126, 0xE05C}, // KEY_RIGHTMETA
    {127, 0xE05D}, // KEY_COMPOSE (Menu)
    {163, 0xE019}, // KEY_NEXTSONG
    {164, 0xE022}, // KEY_PLAYPAUSE
    {165, 0xE010}, // KEY_PREVIOUSSONG
    {166, 0xE024}, // KEY_STOPCD
    {183, 0x64},   // KEY_F13
    {184, 0x65},   // KEY_F14
    {185, 0x66},   // KEY_F15
    {186, 0x67},   // KEY_F16
    {187, 0x68},   // KEY_F17
    {188, 0x69},   // KEY_F18
    {189, 0x6A},   // KEY_F19
    {190, 0x6B},   // KEY_F20
    {191, 0x6C},   // KEY_F21
    {192, 0x6D},   // KEY_F22
    {193, 0x6E},   // KEY_F23
    {194, 0x76},   // KEY_F24
};

// macOS virtual key codes (kVK_*).
constexpr std::pair<int, quint32> kMac[] = {
    {0x00, 0x1E}, // A
    {0x01, 0x1F}, // S
    {0x02, 0x20}, // D
    {0x03, 0x21}, // F
    {0x04, 0x23}, // H
    {0x05, 0x22}, // G
    {0x06, 0x2C}, // Z
    {0x07, 0x2D}, // X
    {0x08, 0x2E}, // C
    {0x09, 0x2F}, // V
    {0x0A, 0x56}, // ISO section (key left of 1 on ISO keyboards / 102nd key)
    {0x0B, 0x30}, // B
    {0x0C, 0x10}, // Q
    {0x0D, 0x11}, // W
    {0x0E, 0x12}, // E
    {0x0F, 0x13}, // R
    {0x10, 0x15}, // Y
    {0x11, 0x14}, // T
    {0x12, 0x02}, // 1
    {0x13, 0x03}, // 2
    {0x14, 0x04}, // 3
    {0x15, 0x05}, // 4
    {0x16, 0x07}, // 6
    {0x17, 0x06}, // 5
    {0x18, 0x0D}, // =
    {0x19, 0x0A}, // 9
    {0x1A, 0x08}, // 7
    {0x1B, 0x0C}, // -
    {0x1C, 0x09}, // 8
    {0x1D, 0x0B}, // 0
    {0x1E, 0x1B}, // ]
    {0x1F, 0x18}, // O
    {0x20, 0x16}, // U
    {0x21, 0x1A}, // [
    {0x22, 0x17}, // I
    {0x23, 0x19}, // P
    {0x24, 0x1C}, // Return
    {0x25, 0x26}, // L
    {0x26, 0x24}, // J
    {0x27, 0x28}, // '
    {0x28, 0x25}, // K
    {0x29, 0x27}, // ;
    {0x2A, 0x2B}, // backslash
    {0x2B, 0x33}, // ,
    {0x2C, 0x35}, // /
    {0x2D, 0x31}, // N
    {0x2E, 0x32}, // M
    {0x2F, 0x34}, // .
    {0x30, 0x0F}, // Tab
    {0x31, 0x39}, // Space
    {0x32, 0x29}, // `
    {0x33, 0x0E}, // Delete (backspace)
    {0x35, 0x01}, // Escape
    {0x36, 0xE05C}, // Right Command
    {0x37, 0xE05B}, // Command
    {0x38, 0x2A}, // Shift
    {0x39, 0x3A}, // Caps Lock
    {0x3A, 0x38}, // Option
    {0x3B, 0x1D}, // Control
    {0x3C, 0x36}, // Right Shift
    {0x3D, 0xE038}, // Right Option
    {0x3E, 0xE01D}, // Right Control
    {0x40, 0x68}, // F17
    {0x41, 0x53}, // Keypad .
    {0x43, 0x37}, // Keypad *
    {0x45, 0x4E}, // Keypad +
    {0x47, 0xE045}, // Keypad Clear (NumLock position)
    {0x48, 0xE030}, // Volume Up
    {0x49, 0xE02E}, // Volume Down
    {0x4A, 0xE020}, // Mute
    {0x4B, 0xE035}, // Keypad /
    {0x4C, 0xE01C}, // Keypad Enter
    {0x4E, 0x4A}, // Keypad -
    {0x4F, 0x69}, // F18
    {0x50, 0x6A}, // F19
    {0x51, 0x59}, // Keypad =
    {0x52, 0x52}, // Keypad 0
    {0x53, 0x4F}, // Keypad 1
    {0x54, 0x50}, // Keypad 2
    {0x55, 0x51}, // Keypad 3
    {0x56, 0x4B}, // Keypad 4
    {0x57, 0x4C}, // Keypad 5
    {0x58, 0x4D}, // Keypad 6
    {0x59, 0x47}, // Keypad 7
    {0x5A, 0x6B}, // F20
    {0x5B, 0x48}, // Keypad 8
    {0x5C, 0x49}, // Keypad 9
    {0x5D, 0x7D}, // JIS Yen
    {0x5E, 0x73}, // JIS Underscore (Ro)
    {0x5F, 0x7E}, // JIS Keypad comma
    {0x60, 0x3F}, // F5
    {0x61, 0x40}, // F6
    {0x62, 0x41}, // F7
    {0x63, 0x3D}, // F3
    {0x64, 0x42}, // F8
    {0x65, 0x43}, // F9
    {0x66, 0x7B}, // JIS Eisu (Muhenkan position)
    {0x67, 0x57}, // F11
    {0x68, 0x70}, // JIS Kana
    {0x69, 0x64}, // F13
    {0x6A, 0x67}, // F16
    {0x6B, 0x65}, // F14
    {0x6D, 0x44}, // F10
    {0x6E, 0xE05D}, // Context menu
    {0x6F, 0x58}, // F12
    {0x71, 0x66}, // F15
    {0x72, 0xE052}, // Help (Insert position)
    {0x73, 0xE047}, // Home
    {0x74, 0xE049}, // Page Up
    {0x75, 0xE053}, // Forward Delete
    {0x76, 0x3E}, // F4
    {0x77, 0xE04F}, // End
    {0x78, 0x3C}, // F2
    {0x79, 0xE051}, // Page Down
    {0x7A, 0x3B}, // F1
    {0x7B, 0xE04B}, // Left
    {0x7C, 0xE04D}, // Right
    {0x7D, 0xE050}, // Down
    {0x7E, 0xE048}, // Up
};

template <size_t N>
quint32 lookup(const std::pair<int, quint32> (&table)[N], int code)
{
    for (const auto &[from, to] : table) {
        if (from == code)
            return to;
    }
    return 0;
}

template <size_t N>
int reverse(const std::pair<int, quint32> (&table)[N], quint32 code)
{
    for (const auto &[from, to] : table) {
        if (to == code)
            return from;
    }
    return -1;
}

} // namespace

quint32 set1FromEvdev(int evdev)
{
    if (const quint32 special = lookup(kEvdevExceptions, evdev))
        return special;
    if (evdev >= 1 && evdev <= 88)
        return quint32(evdev);
    return lookup(kEvdevExtra, evdev);
}

int evdevFromSet1(quint32 set1)
{
    if (set1 == 0xE036)
        set1 = 0x36; // Right Shift as sent by older Windows masters
    if (const int special = reverse(kEvdevExceptions, set1); special >= 0)
        return special;
    if (const int extra = reverse(kEvdevExtra, set1); extra >= 0)
        return extra;
    if (set1 >= 1 && set1 <= 88 && set1 != 69)
        return int(set1);
    return -1;
}

quint32 set1FromMac(int macKeyCode) { return lookup(kMac, macKeyCode); }

int macFromSet1(quint32 set1) { return reverse(kMac, set1 == 0xE036 ? 0x36 : set1); }

} // namespace df::keymap
