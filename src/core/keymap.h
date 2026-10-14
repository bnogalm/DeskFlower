#pragma once

// Key code translation. Keys travel between devices as PS/2 scan code set 1 "make" codes, with
// 0xE0xx for extended keys, exactly as the Windows low-level keyboard hook reports them (which
// includes its quirks: NumLock is 0xE045 and Pause is 0x0045; the one exception is Right Shift, which
// the hook flags as extended but always travels as 0x36). Each platform maps its own codes:
//  - Linux: evdev key codes (X11 key codes are evdev + 8; libei uses evdev codes directly).
//  - macOS: virtual key codes (kVK_* in Carbon's HIToolbox/Events.h).
// Unknown codes map to 0 (set 1) or -1 (platform).

#include <QtGlobal>

namespace df::keymap {

quint32 set1FromEvdev(int evdev);
int evdevFromSet1(quint32 set1);

quint32 set1FromMac(int macKeyCode);
int macFromSet1(quint32 set1);

} // namespace df::keymap
