// macOS input backend.
//  - Capture and edge detection: an active CGEventTap on the session (requires the Accessibility
//    permission; macOS shows the prompt the first time). While capturing, the cursor is frozen with
//    CGAssociateMouseAndMouseCursorPosition(false) and events are swallowed; the tap still reports
//    the mouse deltas.
//  - Injection: CGEventPost at the HID level. Events carry a marker so our own tap ignores them, and
//    the modifier flags and click counts that macOS applications expect.
// Coordinates are global display points (origin at the top-left of the main display), the same
// space QScreen::geometry() uses.
// NOTE: written without a macOS build environment; see the README.

#include "inputbackend.h"
#include "keymap.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <ApplicationServices/ApplicationServices.h>

#include <cmath>
#include <set>
#include <vector>

Q_LOGGING_CATEGORY(lcMacInput, "deskflower.input.mac")

namespace {

constexpr int64_t kInjectedMarker = 0x44464C57; // "DFLW" in kCGEventSourceUserData
constexpr qint64 kEdgeThrottleMs = 15;
constexpr qint64 kDoubleClickMs = 400;
constexpr double kDoubleClickDistance = 4.0;

CGEventFlags modifierFlag(int keycode)
{
    switch (keycode) {
    case 0x38: case 0x3C: return kCGEventFlagMaskShift;
    case 0x3B: case 0x3E: return kCGEventFlagMaskControl;
    case 0x3A: case 0x3D: return kCGEventFlagMaskAlternate;
    case 0x37: case 0x36: return kCGEventFlagMaskCommand;
    case 0x39: return kCGEventFlagMaskAlphaShift;
    case 0x3F: return kCGEventFlagMaskSecondaryFn;
    default: return 0;
    }
}

class MacInputBackend : public InputBackend
{
public:
    explicit MacInputBackend(QObject *parent) : InputBackend(parent) {}

    ~MacInputBackend() override
    {
        releaseAll();
        if (m_capturing)
            setCapture(false, QPoint(int(m_lastInjected.x), int(m_lastInjected.y)));
        if (m_source) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), m_source, kCFRunLoopCommonModes);
            CFRelease(m_source);
        }
        if (m_tap) {
            CGEventTapEnable(m_tap, false);
            CFRelease(m_tap);
        }
        if (m_eventSource)
            CFRelease(m_eventSource);
        CGDisplayRemoveReconfigurationCallback(&MacInputBackend::displaysChanged, this);
    }

    bool start() override
    {
        // Shows the system prompt the first time; the user has to enable DeskFlower in
        // System Settings > Privacy & Security > Accessibility.
        const void *keys[] = {kAXTrustedCheckOptionPrompt};
        const void *values[] = {kCFBooleanTrue};
        CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                     &kCFCopyStringDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        const bool trusted = AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
        if (!trusted)
            qCWarning(lcMacInput) << "Accessibility permission missing: allow deskflower-session in System Settings "
                                     "> Privacy & Security > Accessibility, then restart DeskFlower";

        m_eventSource = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
        refreshDisplays();
        CGDisplayRegisterReconfigurationCallback(&MacInputBackend::displaysChanged, this);

        const CGEventMask mask =
            CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged)
            | CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged)
            | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp)
            | CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp)
            | CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp)
            | CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
            | CGEventMaskBit(kCGEventFlagsChanged);
        m_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask,
                                 &MacInputBackend::tapCallback, this);
        if (!m_tap) {
            qCWarning(lcMacInput) << "Cannot create the event tap (Accessibility / Input Monitoring permission?)";
            return false; // injection still works once Accessibility is granted
        }
        m_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), m_source, kCFRunLoopCommonModes);
        CGEventTapEnable(m_tap, true);
        return true;
    }

    bool isCapturing() const override { return m_capturing; }
    void setReportEdges(bool report) override { m_reportEdges = report; }

    void setCapture(bool on, QPoint returnTo) override
    {
        if (on == m_capturing)
            return;
        if (on) {
            CGAssociateMouseAndMouseCursorPosition(false); // cursor frozen, deltas still reported
            CGDisplayHideCursor(kCGDirectMainDisplay);
            m_capturedModifiers.clear();
            m_capturing = true;
        } else {
            m_capturing = false;
            CGWarpMouseCursorPosition(CGPointMake(returnTo.x(), returnTo.y()));
            CGAssociateMouseAndMouseCursorPosition(true);
            CGDisplayShowCursor(kCGDirectMainDisplay);
        }
    }

    // --- Injection --------------------------------------------------------------------------

    void moveTo(QPoint p) override
    {
        m_lastInjected = CGPointMake(p.x(), p.y());
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (m_injectedButtons.count(1)) {
            type = kCGEventLeftMouseDragged;
        } else if (m_injectedButtons.count(2)) {
            type = kCGEventRightMouseDragged;
            button = kCGMouseButtonRight;
        } else if (!m_injectedButtons.empty()) {
            type = kCGEventOtherMouseDragged;
            button = CGMouseButton(*m_injectedButtons.begin() == 3 ? 2 : *m_injectedButtons.begin() - 1);
        }
        post(CGEventCreateMouseEvent(m_eventSource, type, m_lastInjected, button));
    }

    void button(int button, bool down, QPoint at) override
    {
        moveTo(at);
        CGEventType type;
        CGMouseButton cgButton;
        switch (button) {
        case 1:
            type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
            cgButton = kCGMouseButtonLeft;
            break;
        case 2:
            type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
            cgButton = kCGMouseButtonRight;
            break;
        case 3: case 4: case 5:
            type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
            cgButton = CGMouseButton(button == 3 ? 2 : button - 1); // middle 2, back 3, forward 4
            break;
        default:
            return;
        }
        // macOS applications see double clicks only through the click count.
        if (down) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const bool again = button == m_lastClickButton && now - m_lastClickTime < kDoubleClickMs
                               && std::hypot(at.x() - m_lastClickPos.x(), at.y() - m_lastClickPos.y())
                                      < kDoubleClickDistance;
            m_clickCount = again ? m_clickCount + 1 : 1;
            m_lastClickButton = button;
            m_lastClickTime = now;
            m_lastClickPos = at;
            m_injectedButtons.insert(button);
        } else {
            m_injectedButtons.erase(button);
        }
        CGEventRef e = CGEventCreateMouseEvent(m_eventSource, type, m_lastInjected, cgButton);
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, m_clickCount);
        if (button >= 3)
            CGEventSetIntegerValueField(e, kCGMouseEventButtonNumber, cgButton);
        post(e);
    }

    void wheel(int dx, int dy) override
    {
        // Line units: one notch (120) is one line. Positive dy scrolls up on both sides; macOS's
        // horizontal axis is positive to the left.
        m_wheelX += dx;
        m_wheelY += dy;
        const int lines = m_wheelY / 120;
        const int columns = m_wheelX / 120;
        m_wheelY -= lines * 120;
        m_wheelX -= columns * 120;
        if (lines || columns)
            post(CGEventCreateScrollWheelEvent(m_eventSource, kCGScrollEventUnitLine, 2, int32_t(lines),
                                               int32_t(-columns)));
    }

    void key(quint32 scancode, bool down) override
    {
        const int keycode = df::keymap::macFromSet1(scancode);
        if (keycode < 0)
            return;
        if (const CGEventFlags flag = modifierFlag(keycode)) {
            if (keycode == 0x39) { // Caps Lock toggles on press
                if (down)
                    m_injectedFlags ^= flag;
            } else if (down) {
                m_injectedFlags |= flag;
            } else {
                m_injectedFlags &= ~flag;
            }
        }
        if (down)
            m_injectedKeys.insert(keycode);
        else
            m_injectedKeys.erase(keycode);
        post(CGEventCreateKeyboardEvent(m_eventSource, CGKeyCode(keycode), down));
    }

    void releaseAll() override
    {
        for (int keycode : std::set<int>(m_injectedKeys))
            key(quint32(df::keymap::set1FromMac(keycode)), false);
        for (int b : std::set<int>(m_injectedButtons))
            button(b, false, QPoint(int(m_lastInjected.x), int(m_lastInjected.y)));
        m_injectedKeys.clear();
        m_injectedButtons.clear();
        m_injectedFlags &= kCGEventFlagMaskAlphaShift; // Caps Lock is a state, not a held key
    }

private:
    void post(CGEventRef e)
    {
        if (!e)
            return;
        CGEventSetIntegerValueField(e, kCGEventSourceUserData, kInjectedMarker);
        CGEventSetFlags(e, m_injectedFlags);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    static void displaysChanged(CGDirectDisplayID, CGDisplayChangeSummaryFlags flags, void *self)
    {
        if (!(flags & kCGDisplayBeginConfigurationFlag))
            static_cast<MacInputBackend *>(self)->refreshDisplays();
    }

    void refreshDisplays()
    {
        CGDirectDisplayID ids[32];
        uint32_t count = 0;
        m_displays.clear();
        if (CGGetActiveDisplayList(32, ids, &count) == kCGErrorSuccess) {
            for (uint32_t i = 0; i < count; ++i)
                m_displays.push_back(CGDisplayBounds(ids[i]));
        }
    }

    bool onAnyDisplay(double x, double y) const
    {
        for (const CGRect &r : m_displays) {
            if (CGRectContainsPoint(r, CGPointMake(x, y)))
                return true;
        }
        return false;
    }

    // Side (0 left, 1 right, 2 top, 3 bottom) if p is on an edge with no display beyond.
    int outerEdge(CGPoint p) const
    {
        for (const CGRect &r : m_displays) {
            if (!CGRectContainsPoint(r, p))
                continue;
            const double left = CGRectGetMinX(r), right = CGRectGetMaxX(r) - 1;
            const double top = CGRectGetMinY(r), bottom = CGRectGetMaxY(r) - 1;
            if (p.x <= left && !onAnyDisplay(p.x - 1, p.y))
                return 0;
            if (p.x >= right && !onAnyDisplay(p.x + 1, p.y))
                return 1;
            if (p.y <= top && !onAnyDisplay(p.x, p.y - 1))
                return 2;
            if (p.y >= bottom && !onAnyDisplay(p.x, p.y + 1))
                return 3;
        }
        return -1;
    }

    static int buttonOf(CGEventType type, CGEventRef e, bool *down)
    {
        switch (type) {
        case kCGEventLeftMouseDown: *down = true; return 1;
        case kCGEventLeftMouseUp: *down = false; return 1;
        case kCGEventRightMouseDown: *down = true; return 2;
        case kCGEventRightMouseUp: *down = false; return 2;
        case kCGEventOtherMouseDown:
        case kCGEventOtherMouseUp: {
            *down = type == kCGEventOtherMouseDown;
            const int64_t n = CGEventGetIntegerValueField(e, kCGMouseEventButtonNumber);
            return n == 2 ? 3 : n == 3 ? 4 : n == 4 ? 5 : 0;
        }
        default:
            return 0;
        }
    }

    static CGEventRef tapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void *user)
    {
        auto *self = static_cast<MacInputBackend *>(user);
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            CGEventTapEnable(self->m_tap, true); // macOS disables slow taps: switch it back on
            return event;
        }
        if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kInjectedMarker)
            return event; // ours
        return self->m_capturing ? self->captured(type, event) : self->observed(type, event);
    }

    // Not capturing: track what is held locally and report pushes against outer edges.
    CGEventRef observed(CGEventType type, CGEventRef event)
    {
        bool down = false;
        if (const int b = buttonOf(type, event, &down)) {
            if (down)
                m_localButtons.insert(b);
            else
                m_localButtons.erase(b);
        } else if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
            const quint32 set1 = df::keymap::set1FromMac(int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)));
            if (type == kCGEventKeyDown)
                m_localKeys.insert(set1);
            else
                m_localKeys.erase(set1);
        } else if (m_reportEdges && (type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged
                                     || type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDragged)) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const CGPoint p = CGEventGetLocation(event);
            const int side = outerEdge(p);
            if (side >= 0 && now - m_lastEdge >= kEdgeThrottleMs) {
                m_lastEdge = now;
                emit edgeHit(side, QPoint(int(p.x), int(p.y)));
            }
        }
        return event;
    }

    // Capturing: everything goes to the remote device and is swallowed here.
    CGEventRef captured(CGEventType type, CGEventRef event)
    {
        bool down = false;
        switch (type) {
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged: {
            const int dx = int(CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
            const int dy = int(CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
            if (dx || dy)
                emit moved(dx, dy);
            return nullptr;
        }
        case kCGEventScrollWheel: {
            int dx, dy;
            if (CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous)) {
                // Trackpads: pixels, roughly 50 per notch.
                dy = int(std::lround(CGEventGetDoubleValueField(event, kCGScrollWheelEventPointDeltaAxis1) * 2.4));
                dx = int(std::lround(-CGEventGetDoubleValueField(event, kCGScrollWheelEventPointDeltaAxis2) * 2.4));
            } else {
                dy = int(CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1)) * 120;
                dx = int(-CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2)) * 120;
            }
            if (dx || dy)
                emit wheelEvent(dx, dy);
            return nullptr;
        }
        case kCGEventKeyDown:
        case kCGEventKeyUp: {
            down = type == kCGEventKeyDown;
            const quint32 set1 = df::keymap::set1FromMac(int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)));
            if (!set1)
                return nullptr;
            if (!down && m_localKeys.erase(set1))
                return event; // pressed before the capture started: release it locally
            emit keyEvent(set1, down);
            return nullptr;
        }
        case kCGEventFlagsChanged: {
            // Modifiers only report "flags changed": a second change of the same key is its release.
            const int keycode = int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
            const quint32 set1 = df::keymap::set1FromMac(keycode);
            if (!set1)
                return nullptr;
            if (m_localKeys.count(set1)) {
                m_localKeys.erase(set1);
                return event;
            }
            if (keycode == 0x39) { // Caps Lock: one change per press
                emit keyEvent(set1, true);
                emit keyEvent(set1, false);
                return nullptr;
            }
            const bool pressed = !m_capturedModifiers.count(keycode);
            if (pressed)
                m_capturedModifiers.insert(keycode);
            else
                m_capturedModifiers.erase(keycode);
            emit keyEvent(set1, pressed);
            return nullptr;
        }
        default:
            if (const int b = buttonOf(type, event, &down)) {
                if (!down && m_localButtons.erase(b))
                    return event;
                emit buttonEvent(b, down);
                return nullptr;
            }
            return event;
        }
    }

    CFMachPortRef m_tap = nullptr;
    CFRunLoopSourceRef m_source = nullptr;
    CGEventSourceRef m_eventSource = nullptr;
    std::vector<CGRect> m_displays;
    bool m_capturing = false;
    bool m_reportEdges = false;
    qint64 m_lastEdge = 0;
    std::set<quint32> m_localKeys;
    std::set<int> m_localButtons;
    std::set<int> m_capturedModifiers;

    CGPoint m_lastInjected = CGPointMake(0, 0);
    CGEventFlags m_injectedFlags = 0;
    std::set<int> m_injectedKeys;
    std::set<int> m_injectedButtons;
    int m_wheelX = 0;
    int m_wheelY = 0;
    int m_clickCount = 0;
    int m_lastClickButton = 0;
    qint64 m_lastClickTime = 0;
    QPoint m_lastClickPos;
};

} // namespace

InputBackend *InputBackend::create(QObject *parent) { return new MacInputBackend(parent); }
