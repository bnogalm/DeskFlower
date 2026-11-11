// X11 input backend.
//  - Edge detection and locally held keys/buttons: XInput2 raw events on the root window (they
//    arrive whatever window has the focus).
//  - Capture: pointer and keyboard grab on the root window with an invisible cursor; the pointer is
//    warped back to a parking point after every motion and the offset is the relative motion. Both
//    grabs or none: a grab another client holds (a menu, a drag in progress, a screen locker) is
//    retried for a moment, then the capture is given up (captureLost) instead of half-capturing,
//    which would send the keys to both computers.
//  - Injection: XTest. Coordinates are root window pixels (the ones XRandR reports).
//  - Cursor hiding while another device has the pointer: XFixes.

#include "inputbackend.h"
#include "clock.h"
#include "keymap.h"
#include "linuxbackends.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <QTimer>

#include <set>
#include <vector>

// X11 headers last: they define macros (None, Bool, KeyPress...) that clash with Qt.
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

Q_LOGGING_CATEGORY(lcX11Input, "deskflower.input.x11")

namespace {

constexpr qint64 kEdgeThrottleMs = 15;
constexpr int kWheelStep = 120;
// A grab held by another client is retried this often, this many times (a drag source lets go of
// the pointer as soon as the drag is cancelled; a menu closes...).
constexpr int kGrabRetryMs = 50;
constexpr int kGrabAttempts = 10;
// After coming back through an edge, it does not capture early again for this long.
constexpr qint64 kReturnGuardMs = 250;

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

class X11InputBackend : public InputBackend
{
public:
    X11InputBackend(QObject *parent, X11BackendOptions options) : InputBackend(parent), m_options(options)
    {
        m_grabRetry.setSingleShot(true);
        m_grabRetry.setInterval(kGrabRetryMs);
        connect(&m_grabRetry, &QTimer::timeout, this, [this] { tryGrab(); });
    }

    ~X11InputBackend() override
    {
        if (!m_display)
            return;
        releaseAll();
        if (m_capturing || m_grabPending) {
            XUngrabKeyboard(m_display, CurrentTime);
            XUngrabPointer(m_display, CurrentTime);
        }
        if (m_cursorHidden && m_hasXFixes)
            XFixesShowCursor(m_display, m_root);
        if (m_blankCursor)
            XFreeCursor(m_display, m_blankCursor);
        XCloseDisplay(m_display);
    }

    bool start() override
    {
        m_display = XOpenDisplay(nullptr);
        if (!m_display)
            return fail(tr("Cannot open the X display"));
        m_root = DefaultRootWindow(m_display);

        int event = 0, error = 0;
        if (!XQueryExtension(m_display, "XInputExtension", &m_xiOpcode, &event, &error))
            return fail(tr("The X server has no XInput extension"));
        int major = 2, minor = 2;
        if (XIQueryVersion(m_display, &major, &minor) != Success)
            return fail(tr("The X server does not support XInput 2.2"));
        int xtestEvent = 0, xtestError = 0, xtestMajor = 0, xtestMinor = 0;
        if (!XTestQueryExtension(m_display, &xtestEvent, &xtestError, &xtestMajor, &xtestMinor))
            return fail(tr("The X server has no XTest extension"));
        if (!XRRQueryExtension(m_display, &m_randrEvent, &error))
            return fail(tr("The X server has no XRandR extension"));
        int fixesEvent = 0, fixesError = 0;
        m_hasXFixes = XFixesQueryExtension(m_display, &fixesEvent, &fixesError);

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
        m_rawSelected = true;
        return true;
    }

    QString problem() const override { return m_problem; }
    bool isCapturing() const override { return m_capturing || m_grabPending; }
    // XInput2 raw events show local presses (XTest's own devices are told apart, see injected),
    // once start() selected them (it can fail with the display open, e.g. without XInput 2.2).
    bool reportsLocalPresses() const override { return m_rawSelected; }
    void setReportEdges(bool report) override { m_reportEdges = report; }
    void setEdgeMap(const QList<EdgeSpan> &spans) override { m_edgeMap = spans; }
    bool leftButtonHeld() const override { return m_localButtons.count(1) > 0; }
    void setReportReleases(bool on) override { m_reportReleases = on; }

    void setCursorHidden(bool hidden) override
    {
        if (!m_display || !m_hasXFixes || hidden == m_cursorHidden)
            return;
        // XFixes counts hides per client: exactly one hide is outstanding while hidden.
        if (hidden)
            XFixesHideCursor(m_display, m_root);
        else
            XFixesShowCursor(m_display, m_root);
        m_cursorHidden = hidden;
        flush();
    }

    void setCapture(bool on, QPoint returnTo, int guardSide) override
    {
        if (!m_display)
            return;
        if (on) {
            if (m_capturing || m_grabPending)
                return; // already capturing (an early capture the service confirms)
            m_grabPending = true;
            m_grabAttempts = 0;
            m_haveKeyboard = m_havePointer = false;
            tryGrab();
            return;
        }
        const bool wasGrabbing = m_capturing || m_grabPending;
        m_capturing = false;
        m_grabPending = false;
        m_grabRetry.stop();
        if (wasGrabbing) {
            XUngrabKeyboard(m_display, CurrentTime);
            XUngrabPointer(m_display, CurrentTime);
        }
        m_haveKeyboard = m_havePointer = false;
        XWarpPointer(m_display, X11None, m_root, 0, 0, 0, 0, returnTo.x(), returnTo.y());
        if (guardSide >= 0) {
            m_guardSide = guardSide;
            m_guardSince = df::nowMs();
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

    bool fail(const QString &problem)
    {
        qCWarning(lcX11Input).noquote() << problem;
        m_problem = problem;
        emit problemChanged();
        return false;
    }

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

    // One attempt at the capture grabs. Both are needed: with only the pointer the keys would go to
    // the local focus and to the remote at once, with only the keyboard the local cursor would move.
    void tryGrab()
    {
        if (!m_grabPending)
            return;
        ++m_grabAttempts;
        if (!m_havePointer)
            m_havePointer = XGrabPointer(m_display, m_root, False,
                                         PointerMotionMask | ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                                         GrabModeAsync, X11None, m_blankCursor, CurrentTime)
                            == GrabSuccess;
        if (!m_haveKeyboard)
            m_haveKeyboard =
                XGrabKeyboard(m_display, m_root, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
        if (m_havePointer && m_haveKeyboard) {
            m_grabPending = false;
            m_capturing = true;
            m_park = parkingPoint();
            XWarpPointer(m_display, X11None, m_root, 0, 0, 0, 0, m_park.x(), m_park.y());
            flush();
            return;
        }
        if (m_grabAttempts < kGrabAttempts) {
            m_grabRetry.start();
            XFlush(m_display);
            return;
        }
        // Another application keeps a grab: no half capture. Undo what we got and give the
        // input back (the service brings the cursor home on captureLost).
        qCWarning(lcX11Input) << (m_havePointer ? "Keyboard" : "Pointer")
                              << "grab failed: another application holds it; capture abandoned";
        if (m_havePointer)
            XUngrabPointer(m_display, CurrentTime);
        if (m_haveKeyboard)
            XUngrabKeyboard(m_display, CurrentTime);
        m_havePointer = m_haveKeyboard = false;
        m_grabPending = false;
        flush();
        emit captureLost(cursorPosition());
    }

    QPoint cursorPosition() const
    {
        Window rootReturn, child;
        int rx = 0, ry = 0, wx = 0, wy = 0;
        unsigned int buttons = 0;
        if (!XQueryPointer(m_display, m_root, &rootReturn, &child, &rx, &ry, &wx, &wy, &buttons))
            return parkingPoint();
        return QPoint(rx, ry);
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

    // Within a span of the edge map the push is known to cross: see InputBackend::setEdgeMap.
    bool earlyCaptureAllowed(int side, int x, int y, qint64 now) const
    {
        if (m_edgeMap.isEmpty() || !m_localButtons.empty())
            return false; // a held button may be a drag: the service decides (catch first)
        if (m_guardSide == side && now - m_guardSince < kReturnGuardMs)
            return false;
        const int along = side < 2 ? y : x;
        for (const EdgeSpan &s : m_edgeMap) {
            if (s.side == side && s.screen.contains(x, y) && along >= s.from && along <= s.to)
                return true;
        }
        return false;
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

    bool injected(const XIRawEvent *raw) const
    {
        return !m_options.xtestIsLocal && m_xtestDevices.count(raw->sourceid);
    }

    // Not capturing: remember what is held locally and report pushes against outer edges.
    void handleRaw(const XIRawEvent *raw, int type)
    {
        if (m_capturing || injected(raw))
            return; // grabbed events are handled separately; injected ones are ours
        switch (type) {
        case XI_RawKeyPress: {
            const quint32 set1 = df::keymap::set1FromEvdev(raw->detail - 8);
            // Autorepeat (the key already held, XkbSetDetectableAutoRepeat) is no new press.
            const bool gesture = !m_localKeys.count(set1) && df::keymap::isPasteEvidenceKey(m_localKeys, set1, false);
            m_localKeys.insert(set1);
            if (gesture)
                emit localPasteGesture(); // paste evidence for file data
            break;
        }
        case XI_RawKeyRelease:
            m_localKeys.erase(df::keymap::set1FromEvdev(raw->detail - 8));
            break;
        case XI_RawButtonPress:
            setCursorHidden(false); // this computer's own mouse: the cursor belongs here again
            if (const int b = fromX11Button(unsigned(raw->detail))) {
                m_localButtons.insert(b);
                emit localPasteGesture(); // a button, not the wheel (X11 buttons 4 to 7)
            }
            break;
        case XI_RawButtonRelease: {
            const int b = fromX11Button(unsigned(raw->detail));
            if (b)
                m_localButtons.erase(b);
            if (b == 1 && m_reportReleases && !m_grabPending)
                emit localRelease(cursorPosition()); // a file drag may end here (dragWatch)
            break;
        }
        case XI_RawMotion: {
            setCursorHidden(false);
            if (!m_reportEdges || m_grabPending)
                break;
            const qint64 now = df::nowMs();
            const QPoint at = cursorPosition();
            const int side = outerEdge(at.x(), at.y());
            if (side < 0)
                break;
            if (earlyCaptureAllowed(side, at.x(), at.y(), now)) {
                // Known to cross: capture now so that the motion while the service answers is not
                // lost. The edge goes first; the service confirms or declines (setCapture).
                m_lastEdge = now;
                emit edgeHit(side, at, true);
                setCapture(true, at, -1);
                break;
            }
            if (now - m_lastEdge < kEdgeThrottleMs)
                break;
            m_lastEdge = now;
            emit edgeHit(side, at, false);
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

    const X11BackendOptions m_options;
    Display *m_display = nullptr;
    Window m_root = 0;
    int m_xiOpcode = 0;
    int m_randrEvent = 0;
    bool m_hasXFixes = false;
    bool m_rawSelected = false; // start() got as far as selecting the raw events
    Cursor m_blankCursor = 0;
    std::set<int> m_xtestDevices;
    std::vector<Rect> m_monitors;
    int m_primary = 0;
    QString m_problem;
    bool m_capturing = false;
    bool m_grabPending = false; // capture asked for, grabs not complete yet (retrying)
    bool m_havePointer = false;
    bool m_haveKeyboard = false;
    int m_grabAttempts = 0;
    QTimer m_grabRetry;
    bool m_reportEdges = false;
    bool m_reportReleases = false;
    bool m_cursorHidden = false;
    QList<EdgeSpan> m_edgeMap;
    int m_guardSide = -1;
    qint64 m_guardSince = 0;
    QPoint m_park;
    qint64 m_lastEdge = -kEdgeThrottleMs;
    int m_wheelX = 0;
    int m_wheelY = 0;
    std::set<quint32> m_localKeys;
    std::set<int> m_localButtons;
    std::set<unsigned int> m_injectedKeys;
    std::set<unsigned int> m_injectedButtons;
};

} // namespace

InputBackend *createX11InputBackend(QObject *parent, X11BackendOptions options)
{
    return new X11InputBackend(parent, options);
}
