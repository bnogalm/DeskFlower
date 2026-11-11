// macOS input backend.
//  - Capture and edge detection: an active CGEventTap on the session (requires the Accessibility
//    permission; macOS shows the prompt the first time). While capturing, the cursor is frozen with
//    CGAssociateMouseAndMouseCursorPosition(false) and events are swallowed; the tap still reports
//    the mouse deltas.
//  - Injection: CGEventPost at the HID level. Events carry a marker so our own tap ignores them, and
//    the modifier flags and click counts that macOS applications expect.
//  - Cursor hiding: CGDisplayHideCursor only works for the frontmost application, which this
//    background helper never is, unless its connection to the window server has the (private)
//    "SetsCursorInBackground" property, as other keyboard/mouse sharing tools do.
//  - Problems the user must fix (problem()): the Accessibility permission, the event tap (Input
//    Monitoring), Secure Event Input (a password field or Terminal's Secure Keyboard Entry: the tap
//    gets no keys, they stay on this Mac) and the Screen Recording permission (remote view).
// Coordinates are global display points (origin at the top-left of the main display), the same
// space QScreen::geometry() uses.
// NOTE: written without a macOS build environment; see the README.

#include "inputbackend.h"
#include "keymap.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QTimer>

#include <ApplicationServices/ApplicationServices.h>

#include <cmath>
#include <dlfcn.h>
#include <set>
#include <vector>

Q_LOGGING_CATEGORY(lcMacInput, "deskflower.input.mac")

// Carbon (HIToolbox, not in ApplicationServices' headers): true while some application asked for
// secure keyboard entry.
extern "C" Boolean IsSecureEventInputEnabled(void);

namespace {

constexpr int64_t kInjectedMarker = 0x44464C57; // "DFLW" in kCGEventSourceUserData (the agent uses it too)
constexpr qint64 kEdgeThrottleMs = 15;
constexpr qint64 kDoubleClickMs = 400;
constexpr double kDoubleClickDistance = 4.0;
constexpr int kStatusPollMs = 1000; // permissions granted later, Secure Event Input

// Device-dependent modifier bits of the event flags (IOKit's NX_DEVICE*KEYMASK): left and right
// keys of one kind share the generic flag, these tell them apart.
constexpr CGEventFlags kDeviceLeftCtrl = 0x00000001, kDeviceLeftShift = 0x00000002,
                       kDeviceRightShift = 0x00000004, kDeviceLeftCmd = 0x00000008,
                       kDeviceRightCmd = 0x00000010, kDeviceLeftAlt = 0x00000020,
                       kDeviceRightAlt = 0x00000040, kDeviceRightCtrl = 0x00002000;

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

// The device bit of one modifier key, and those of both keys of its kind (0: none, e.g. Fn).
CGEventFlags deviceFlag(int keycode)
{
    switch (keycode) {
    case 0x38: return kDeviceLeftShift;
    case 0x3C: return kDeviceRightShift;
    case 0x3B: return kDeviceLeftCtrl;
    case 0x3E: return kDeviceRightCtrl;
    case 0x3A: return kDeviceLeftAlt;
    case 0x3D: return kDeviceRightAlt;
    case 0x37: return kDeviceLeftCmd;
    case 0x36: return kDeviceRightCmd;
    default: return 0;
    }
}

CGEventFlags devicePair(int keycode)
{
    switch (keycode) {
    case 0x38: case 0x3C: return kDeviceLeftShift | kDeviceRightShift;
    case 0x3B: case 0x3E: return kDeviceLeftCtrl | kDeviceRightCtrl;
    case 0x3A: case 0x3D: return kDeviceLeftAlt | kDeviceRightAlt;
    case 0x37: case 0x36: return kDeviceLeftCmd | kDeviceRightCmd;
    default: return 0;
    }
}

// Whether the modifier key `keycode` is down after a kCGEventFlagsChanged carrying `flags` (a
// modifier only reports "flags changed"; the flags say whether this was its press or release).
bool modifierDown(int keycode, CGEventFlags flags)
{
    const CGEventFlags generic = modifierFlag(keycode);
    if (!generic || !(flags & generic))
        return false;
    const CGEventFlags pair = devicePair(keycode);
    if (!pair || !(flags & pair))
        return true; // no device bits (Fn, some virtual keyboards): the generic flag says
    return (flags & deviceFlag(keycode)) != 0;
}

bool isMouseMove(CGEventType type)
{
    return type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDragged
           || type == kCGEventOtherMouseDragged;
}

class MacInputBackend : public InputBackend
{
public:
    explicit MacInputBackend(QObject *parent) : InputBackend(parent)
    {
        m_clock.start(); // monotonic: throttles and double clicks survive a change of the clock
        m_statusPoll.setInterval(kStatusPollMs);
        QObject::connect(&m_statusPoll, &QTimer::timeout, this, [this] { refreshStatus(); });
    }

    ~MacInputBackend() override
    {
        releaseAll();
        if (m_capturing)
            setCapture(false, QPoint(int(m_lastInjected.x), int(m_lastInjected.y)), -1);
        m_cursorHidden = false;
        updateCursorVisibility();
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
            qCWarning(lcMacInput) << "Accessibility permission missing: allow DeskFlower session in System Settings "
                                     "> Privacy & Security > Accessibility";

        allowCursorHidingInBackground();

        m_eventSource = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
        refreshDisplays();
        CGDisplayRegisterReconfigurationCallback(&MacInputBackend::displaysChanged, this);

        const bool tap = createTap();
        refreshStatus();
        m_statusPoll.start(); // a permission granted later takes effect without a restart
        return tap; // injection still works once Accessibility is granted
    }

    bool isCapturing() const override { return m_capturing; }
    // Local presses are seen through the event tap, once Accessibility allows it, and not while
    // Secure Event Input is on (the tap gets no keys then). Set by refreshStatus with the problem.
    bool reportsLocalPresses() const override { return m_localPresses; }
    void setReportEdges(bool report) override { m_reportEdges = report; }

    void setCapture(bool on, QPoint returnTo, int /*guardSide*/) override
    {
        if (on == m_capturing)
            return;
        if (on) {
            CGAssociateMouseAndMouseCursorPosition(false); // cursor frozen, deltas still reported
            m_capturing = true;
        } else {
            m_capturing = false;
            CGWarpMouseCursorPosition(CGPointMake(returnTo.x(), returnTo.y()));
            CGAssociateMouseAndMouseCursorPosition(true);
            m_cursorHidden = false; // back here: the cursor belongs to this computer again
        }
        updateCursorVisibility();
    }

    void setCursorHidden(bool hidden) override
    {
        if (hidden == m_cursorHidden)
            return;
        m_cursorHidden = hidden;
        updateCursorVisibility();
    }

    bool leftButtonHeld() const override { return m_localButtons.count(1) > 0; }
    void setReportReleases(bool on) override { m_reportReleases = on; }

    QString problem() const override { return m_problem; }
    QString problemLink() const override { return m_problemLink; }

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
            const qint64 now = m_clock.elapsed();
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
        if (!e)
            return;
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
                m_injectedFlags |= flag | deviceFlag(keycode);
            } else {
                // The generic flag stays while the other key of the kind is still held.
                m_injectedFlags &= ~deviceFlag(keycode);
                const CGEventFlags pair = devicePair(keycode);
                if (!pair || !(m_injectedFlags & pair))
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
    // A background process may hide the cursor (setCursorHidden, capture) only once its window
    // server connection says so. Private functions, looked up at run time: a system without them
    // only loses the hiding.
    static void allowCursorHidingInBackground()
    {
        using DefaultConnection = int (*)();
        using SetProperty = CGError (*)(int, int, CFStringRef, CFTypeRef);
        const auto defaultConnection = reinterpret_cast<DefaultConnection>(dlsym(RTLD_DEFAULT, "_CGSDefaultConnection"));
        const auto setProperty = reinterpret_cast<SetProperty>(dlsym(RTLD_DEFAULT, "CGSSetConnectionProperty"));
        if (!defaultConnection || !setProperty) {
            qCWarning(lcMacInput) << "Cannot hide the cursor from the background on this macOS";
            return;
        }
        const int connection = defaultConnection();
        setProperty(connection, connection, CFSTR("SetsCursorInBackground"), kCFBooleanTrue);
    }

    bool createTap()
    {
        if (m_tap)
            return true;
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
            return false;
        }
        m_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), m_source, kCFRunLoopCommonModes);
        CGEventTapEnable(m_tap, true);
        qCInfo(lcMacInput) << "Event tap installed";
        return true;
    }

    // Permissions and Secure Event Input, polled: macOS notifies neither.
    void refreshStatus()
    {
        const bool trusted = AXIsProcessTrusted();
        if (trusted && !m_tap)
            createTap(); // granted since the start
        QString problem, link;
        if (!trusted) {
            problem = tr("This Mac's keyboard and mouse cannot be shared: allow \"DeskFlower session\" in System "
                         "Settings › Privacy & Security › Accessibility.");
            link = QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility");
        } else if (!m_tap) {
            problem = tr("This Mac's keyboard and mouse cannot be shared: allow \"DeskFlower session\" in System "
                         "Settings › Privacy & Security › Input Monitoring.");
            link = QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent");
        } else if (IsSecureEventInputEnabled()) {
            // The tap gets no keys meanwhile: typed on the other computer, they would stay here.
            problem = tr("Secure keyboard entry is on (a password field, or Secure Keyboard Entry in Terminal): "
                         "keys typed while the cursor is on another computer stay on this Mac.");
        } else if (!CGPreflightScreenCaptureAccess()) {
            problem = tr("This Mac's screen cannot be viewed from other computers (thumbnails, remote view): allow "
                         "\"DeskFlower session\" in System Settings › Privacy & Security › Screen Recording.");
            link = QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture");
        }
        const bool localPresses = m_tap && !IsSecureEventInputEnabled();
        if (problem == m_problem && link == m_problemLink && localPresses == m_localPresses)
            return;
        m_localPresses = localPresses;
        if (problem != m_problem && !problem.isEmpty())
            qCWarning(lcMacInput).noquote() << problem;
        m_problem = problem;
        m_problemLink = link;
        emit problemChanged();
    }

    // Hidden while capturing (the cursor stays frozen here) or while another device has it.
    // CGDisplayHideCursor and CGDisplayShowCursor nest: called once each way.
    void updateCursorVisibility()
    {
        const bool hide = m_capturing || m_cursorHidden;
        if (hide == m_hiddenByUs)
            return;
        m_hiddenByUs = hide;
        if (hide)
            CGDisplayHideCursor(kCGDirectMainDisplay);
        else
            CGDisplayShowCursor(kCGDirectMainDisplay);
    }

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
        // Ours, or the agent's (a file drag shown here): neither is this computer's user.
        if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kInjectedMarker)
            return event;
        return self->m_capturing ? self->captured(type, event) : self->observed(type, event);
    }

    // Not capturing: track what is held locally and report pushes against outer edges.
    CGEventRef observed(CGEventType type, CGEventRef event)
    {
        // Someone uses this computer's own mouse: the cursor belongs here again.
        if (m_cursorHidden && (isMouseMove(type) || type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown
                               || type == kCGEventOtherMouseDown || type == kCGEventScrollWheel))
            setCursorHidden(false);
        bool down = false;
        if (const int b = buttonOf(type, event, &down)) {
            if (down) {
                m_localButtons.insert(b);
                emit localPasteGesture(); // paste evidence for file data
            } else {
                m_localButtons.erase(b);
                // A file drag that crossed and came back here ends with this button.
                if (b == 1 && m_reportReleases) {
                    const CGPoint p = CGEventGetLocation(event);
                    emit localRelease(QPoint(int(p.x), int(p.y)));
                }
            }
        } else if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
            const quint32 set1 = df::keymap::set1FromMac(int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)));
            if (type == kCGEventKeyDown) {
                // Command+V is this Mac's paste chord (Command maps to the Windows key's code).
                // Autorepeat (the key already held) is no new press.
                const bool gesture = !m_localKeys.count(set1) && df::keymap::isPasteEvidenceKey(m_localKeys, set1, true);
                m_localKeys.insert(set1);
                if (gesture)
                    emit localPasteGesture();
            } else
                m_localKeys.erase(set1);
        } else if (type == kCGEventFlagsChanged) {
            // Modifiers held here when a capture starts: their release then belongs to this Mac
            // (captured() lets it through) instead of reaching the other computer as a press.
            const int keycode = int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
            const quint32 set1 = df::keymap::set1FromMac(keycode);
            if (set1 && keycode != 0x39) { // Caps Lock is a state, not a held key
                if (modifierDown(keycode, CGEventGetFlags(event))) {
                    m_localKeys.insert(set1); // a modifier alone is no gesture
                } else {
                    m_localKeys.erase(set1);
                }
            }
        } else if (m_reportEdges && isMouseMove(type)) {
            const qint64 now = m_clock.elapsed();
            const CGPoint p = CGEventGetLocation(event);
            const int side = outerEdge(p);
            if (side >= 0 && now - m_lastEdge >= kEdgeThrottleMs) {
                m_lastEdge = now;
                emit edgeHit(side, QPoint(int(p.x), int(p.y)), false);
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
            const int keycode = int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
            const quint32 set1 = df::keymap::set1FromMac(keycode);
            if (!set1)
                return nullptr;
            if (keycode == 0x39) { // Caps Lock: one change per press
                emit keyEvent(set1, true);
                emit keyEvent(set1, false);
                return nullptr;
            }
            const bool pressed = modifierDown(keycode, CGEventGetFlags(event));
            if (!pressed && m_localKeys.erase(set1))
                return event; // held before the capture started: released here
            emit keyEvent(set1, pressed);
            return nullptr;
        }
        default:
            if (const int b = buttonOf(type, event, &down)) {
                if (!down && m_localButtons.erase(b)) {
                    // Pressed here before the capture: released here. Reported too, it ends a file
                    // drag that crossed to another device (the controller does not forward releases
                    // of buttons it never pressed there).
                    emit buttonEvent(b, false);
                    return event;
                }
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
    QElapsedTimer m_clock;
    QTimer m_statusPoll;
    QString m_problem;
    QString m_problemLink;
    bool m_localPresses = false; // reportsLocalPresses, kept with the problem by refreshStatus
    bool m_capturing = false;
    bool m_reportEdges = false;
    bool m_reportReleases = false;
    bool m_cursorHidden = false; // another device has the cursor (setCursorHidden)
    bool m_hiddenByUs = false;   // CGDisplayHideCursor in effect
    qint64 m_lastEdge = -kEdgeThrottleMs;
    std::set<quint32> m_localKeys;
    std::set<int> m_localButtons;

    CGPoint m_lastInjected = CGPointMake(0, 0);
    CGEventFlags m_injectedFlags = 0;
    std::set<int> m_injectedKeys;
    std::set<int> m_injectedButtons;
    int m_wheelX = 0;
    int m_wheelY = 0;
    int m_clickCount = 0;
    int m_lastClickButton = 0;
    qint64 m_lastClickTime = -kDoubleClickMs;
    QPoint m_lastClickPos;
};

} // namespace

InputBackend *InputBackend::create(QObject *parent) { return new MacInputBackend(parent); }
