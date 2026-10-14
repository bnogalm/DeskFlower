// X11 input backend.
//  - Edge detection and locally held keys/buttons: XInput2 raw events on the root window (they
//    arrive whatever window has the focus).
//  - Capture: pointer and keyboard grab on the root window with an invisible cursor; the pointer is
//    warped back to a parking point after every motion and the offset is the relative motion.
//  - Injection: XTest. Coordinates are root window pixels (the ones XRandR reports).
// NOTE: written without a Linux build environment; see the README.

#include "inputbackend.h"
#include "keymap.h"
#include "linuxbackends.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <set>
#include <vector>

// X11 headers last: they define macros (None, Bool, KeyPress...) that clash with Qt.
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>

Q_LOGGING_CATEGORY(lcX11Input, "deskflower.input.x11")

namespace {

constexpr qint64 kEdgeThrottleMs = 15;
constexpr int kWheelStep = 120;

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

class X11InputBackend : public InputBackend
{
public:
    explicit X11InputBackend(QObject *parent) : InputBackend(parent) {}

    ~X11InputBackend() override
    {
        if (!m_display)
            return;
        releaseAll();
        if (m_capturing) {
            XUngrabKeyboard(m_display, CurrentTime);
            XUngrabPointer(m_display, CurrentTime);
        }
        if (m_blankCursor)
            XFreeCursor(m_display, m_blankCursor);
        XCloseDisplay(m_display);
    }

    bool start() override
    {
        m_display = XOpenDisplay(nullptr);
        if (!m_display) {
            qCWarning(lcX11Input) << "Cannot open the X display";
            return false;
        }
        m_root = DefaultRootWindow(m_display);

        int event = 0, error = 0;
        if (!XQueryExtension(m_display, "XInputExtension", &m_xiOpcode, &event, &error)) {
            qCWarning(lcX11Input) << "XInput extension missing";
            return false;
        }
        int major = 2, minor = 2;
        if (XIQueryVersion(m_display, &major, &minor) != Success) {
            qCWarning(lcX11Input) << "XInput 2.2 is required";
            return false;
        }
        int xtestEvent = 0, xtestError = 0, xtestMajor = 0, xtestMinor = 0;
        if (!XTestQueryExtension(m_display, &xtestEvent, &xtestError, &xtestMajor, &xtestMinor)) {
            qCWarning(lcX11Input) << "XTest extension missing";
            return false;
        }
        if (!XRRQueryExtension(m_display, &m_randrEvent, &error)) {
            qCWarning(lcX11Input) << "XRandR extension missing";
            return false;
        }

        // Raw events from every master device, delivered to the root window.
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
        XISetMask(bits, XI_RawMotion);
        XISetMask(bits, XI_RawKeyPress);
        XISetMask(bits, XI_RawKeyRelease);
        XISetMask(bits, XI_RawButtonPress);
        XISetMask(bits, XI_RawButtonRelease);
        XIEventMask mask;
        mask.deviceid = XIAllMasterDevices;
        mask.mask_len = sizeof(bits);
        mask.mask = bits;
        XISelectEvents(m_display, m_root, &mask, 1);
        XRRSelectInput(m_display, m_root, RRScreenChangeNotifyMask);
        // Autorepeat as repeated presses only (no synthetic releases in between).
        XkbSetDetectableAutoRepeat(m_display, True, nullptr);

        // Invisible cursor for the capture grab.
        const Pixmap pixmap = XCreatePixmap(m_display, m_root, 1, 1, 1);
        XColor black = {};
        m_blankCursor = XCreatePixmapCursor(m_display, pixmap, pixmap, &black, &black, 0, 0);
        XFreePixmap(m_display, pixmap);

        findXTestDevices();
        refreshMonitors();
        XFlush(m_display);

        auto *notifier = new QSocketNotifier(ConnectionNumber(m_display), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] { processEvents(); });
        processEvents();
        return true;
    }

    bool isCapturing() const override { return m_capturing; }
    void setReportEdges(bool report) override { m_reportEdges = report; }

    void setCapture(bool on, QPoint returnTo) override
    {
        if (!m_display)
            return;
        if (on) {
            m_park = parkingPoint();
            if (XGrabPointer(m_display, m_root, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                             GrabModeAsync, GrabModeAsync, X11None, m_blankCursor, CurrentTime)
                != GrabSuccess)
                qCWarning(lcX11Input) << "Pointer grab failed (another application holds it)";
            if (XGrabKeyboard(m_display, m_root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
                qCWarning(lcX11Input) << "Keyboard grab failed (another application holds it)";
            XWarpPointer(m_display, X11None, m_root, 0, 0, 0, 0, m_park.x(), m_park.y());
            m_capturing = true;
        } else {
            m_capturing = false;
            XUngrabKeyboard(m_display, CurrentTime);
            XUngrabPointer(m_display, CurrentTime);
            XWarpPointer(m_display, X11None, m_root, 0, 0, 0, 0, returnTo.x(), returnTo.y());
        }
        flush();
    }

    void moveTo(QPoint p) override
    {
        if (!m_display)
            return;
        XTestFakeMotionEvent(m_display, -1, p.x(), p.y(), CurrentTime);
        flush();
    }

    void button(int button, bool down, QPoint at) override
    {
        const unsigned int xButton = toX11Button(button);
        if (!m_display || !xButton)
            return;
        XTestFakeMotionEvent(m_display, -1, at.x(), at.y(), CurrentTime);
        XTestFakeButtonEvent(m_display, xButton, down ? True : False, CurrentTime);
        if (down)
            m_injectedButtons.insert(xButton);
        else
            m_injectedButtons.erase(xButton);
        flush();
    }

    void wheel(int dx, int dy) override
    {
        if (!m_display)
            return;
        // X11 wheels are buttons: 4 up, 5 down, 6 left, 7 right; one click per 120 units.
        m_wheelX += dx;
        m_wheelY += dy;
        auto clicks = [this](int &acc, unsigned int positive, unsigned int negative) {
            while (acc >= kWheelStep || acc <= -kWheelStep) {
                const unsigned int b = acc > 0 ? positive : negative;
                XTestFakeButtonEvent(m_display, b, True, CurrentTime);
                XTestFakeButtonEvent(m_display, b, False, CurrentTime);
                acc += acc > 0 ? -kWheelStep : kWheelStep;
            }
        };
        clicks(m_wheelY, 4, 5);
        clicks(m_wheelX, 7, 6);
        flush();
    }

    void key(quint32 scancode, bool down) override
    {
        const int evdev = df::keymap::evdevFromSet1(scancode);
        if (!m_display || evdev < 0)
            return;
        const unsigned int keycode = unsigned(evdev + 8);
        XTestFakeKeyEvent(m_display, keycode, down ? True : False, CurrentTime);
        if (down)
            m_injectedKeys.insert(keycode);
        else
            m_injectedKeys.erase(keycode);
        flush();
    }

    void releaseAll() override
    {
        if (!m_display)
            return;
        for (unsigned int keycode : std::set<unsigned int>(m_injectedKeys))
            XTestFakeKeyEvent(m_display, keycode, False, CurrentTime);
        for (unsigned int b : std::set<unsigned int>(m_injectedButtons))
            XTestFakeButtonEvent(m_display, b, False, CurrentTime);
        m_injectedKeys.clear();
        m_injectedButtons.clear();
        flush();
    }

private:
    static constexpr unsigned long X11None = 0L; // Xlib's "None"

    static unsigned int toX11Button(int button)
    {
        switch (button) {
        case 1: return 1; // left
        case 2: return 3; // right
        case 3: return 2; // middle
        case 4: return 8; // back
        case 5: return 9; // forward
        default: return 0;
        }
    }

    static int fromX11Button(unsigned int button)
    {
        switch (button) {
        case 1: return 1;
        case 3: return 2;
        case 2: return 3;
        case 8: return 4;
        case 9: return 5;
        default: return 0;
        }
    }

    // Xlib buffers events while answering requests: drain them after each call.
    void flush()
    {
        XFlush(m_display);
        if (XPending(m_display) > 0)
            QMetaObject::invokeMethod(this, [this] { processEvents(); }, Qt::QueuedConnection);
    }

    void findXTestDevices()
    {
        int count = 0;
        XIDeviceInfo *devices = XIQueryDevice(m_display, XIAllDevices, &count);
        for (int i = 0; i < count; ++i) {
            if (QByteArray(devices[i].name).contains("XTEST"))
                m_xtestDevices.insert(devices[i].deviceid);
        }
        XIFreeDeviceInfo(devices);
    }

    void refreshMonitors()
    {
        m_monitors.clear();
        int count = 0;
        XRRMonitorInfo *monitors = XRRGetMonitors(m_display, m_root, True, &count);
        for (int i = 0; i < count; ++i) {
            m_monitors.push_back(Rect{monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height});
            if (monitors[i].primary)
                m_primary = int(m_monitors.size()) - 1;
        }
        if (monitors)
            XRRFreeMonitors(monitors);
    }

    QPoint parkingPoint() const
    {
        if (m_monitors.empty())
            return QPoint(100, 100);
        const Rect &r = m_monitors[size_t(qBound(0, m_primary, int(m_monitors.size()) - 1))];
        return QPoint(r.x + r.w / 2, r.y + r.h / 2);
    }

    bool onAnyMonitor(int x, int y) const
    {
        for (const Rect &r : m_monitors) {
            if (r.contains(x, y))
                return true;
        }
        return false;
    }

    int outerEdge(int x, int y) const
    {
        for (const Rect &r : m_monitors) {
            if (!r.contains(x, y))
                continue;
            if (x == r.x && !onAnyMonitor(x - 1, y))
                return 0;
            if (x == r.x + r.w - 1 && !onAnyMonitor(x + 1, y))
                return 1;
            if (y == r.y && !onAnyMonitor(x, y - 1))
                return 2;
            if (y == r.y + r.h - 1 && !onAnyMonitor(x, y + 1))
                return 3;
        }
        return -1;
    }

    void processEvents()
    {
        while (m_display && XPending(m_display) > 0) {
            XEvent ev;
            XNextEvent(m_display, &ev);
            if (ev.type == GenericEvent && ev.xcookie.extension == m_xiOpcode
                && XGetEventData(m_display, &ev.xcookie)) {
                handleRaw(static_cast<XIRawEvent *>(ev.xcookie.data), ev.xcookie.evtype);
                XFreeEventData(m_display, &ev.xcookie);
            } else if (ev.type == m_randrEvent + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
                refreshMonitors();
            } else if (m_capturing) {
                handleGrabbed(ev);
            }
        }
    }

    // Not capturing: remember what is held locally and report pushes against outer edges.
    void handleRaw(const XIRawEvent *raw, int type)
    {
        if (m_capturing || m_xtestDevices.count(raw->sourceid))
            return; // grabbed events are handled separately; injected ones are ours
        switch (type) {
        case XI_RawKeyPress:
            m_localKeys.insert(df::keymap::set1FromEvdev(raw->detail - 8));
            break;
        case XI_RawKeyRelease:
            m_localKeys.erase(df::keymap::set1FromEvdev(raw->detail - 8));
            break;
        case XI_RawButtonPress:
            if (const int b = fromX11Button(unsigned(raw->detail)))
                m_localButtons.insert(b);
            break;
        case XI_RawButtonRelease:
            if (const int b = fromX11Button(unsigned(raw->detail)))
                m_localButtons.erase(b);
            break;
        case XI_RawMotion: {
            if (!m_reportEdges)
                break;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastEdge < kEdgeThrottleMs)
                break;
            Window rootReturn, child;
            int rx = 0, ry = 0, wx = 0, wy = 0;
            unsigned int buttons = 0;
            if (!XQueryPointer(m_display, m_root, &rootReturn, &child, &rx, &ry, &wx, &wy, &buttons))
                break;
            const int side = outerEdge(rx, ry);
            if (side >= 0) {
                m_lastEdge = now;
                emit edgeHit(side, QPoint(rx, ry));
            }
            break;
        }
        default:
            break;
        }
    }

    // Capturing: everything goes to the remote device.
    void handleGrabbed(const XEvent &ev)
    {
        switch (ev.type) {
        case MotionNotify: {
            const int dx = ev.xmotion.x_root - m_park.x();
            const int dy = ev.xmotion.y_root - m_park.y();
            if (dx || dy) {
                emit moved(dx, dy);
                XWarpPointer(m_display, X11None, m_root, 0, 0, 0, 0, m_park.x(), m_park.y());
                XFlush(m_display);
            }
            break;
        }
        case ButtonPress:
        case ButtonRelease: {
            const bool down = ev.type == ButtonPress;
            const unsigned int b = ev.xbutton.button;
            if (b >= 4 && b <= 7) {
                if (down) { // wheel "buttons" press and release at once
                    const int amount = (b == 4 || b == 7) ? kWheelStep : -kWheelStep;
                    emit wheelEvent(b >= 6 ? amount : 0, b <= 5 ? amount : 0);
                }
                break;
            }
            const int mapped = fromX11Button(b);
            if (!mapped)
                break;
            if (!down && m_localButtons.erase(mapped))
                break; // pressed before the capture started: stays local
            emit buttonEvent(mapped, down);
            break;
        }
        case KeyPress:
        case KeyRelease: {
            const bool down = ev.type == KeyPress;
            const quint32 set1 = df::keymap::set1FromEvdev(int(ev.xkey.keycode) - 8);
            if (!set1)
                break;
            if (!down && m_localKeys.erase(set1))
                break;
            emit keyEvent(set1, down);
            break;
        }
        default:
            break;
        }
    }

    Display *m_display = nullptr;
    Window m_root = 0;
    int m_xiOpcode = 0;
    int m_randrEvent = 0;
    Cursor m_blankCursor = 0;
    std::set<int> m_xtestDevices;
    std::vector<Rect> m_monitors;
    int m_primary = 0;
    bool m_capturing = false;
    bool m_reportEdges = false;
    QPoint m_park;
    qint64 m_lastEdge = 0;
    int m_wheelX = 0;
    int m_wheelY = 0;
    std::set<quint32> m_localKeys;
    std::set<int> m_localButtons;
    std::set<unsigned int> m_injectedKeys;
    std::set<unsigned int> m_injectedButtons;
};

} // namespace

InputBackend *createX11InputBackend(QObject *parent) { return new X11InputBackend(parent); }
