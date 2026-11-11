#include "inputbackend.h"
#include "keyinjection.h"
#include "keymap.h"

#include <QLoggingCategory>
#include <QString>
#include <QStringList>

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(lcWinInput, "deskflower.input.win")

namespace wi = df::wininput;
static_assert(wi::kVkRShift == VK_RSHIFT && wi::kVkLWin == VK_LWIN && wi::kVkRWin == VK_RWIN && wi::kVkApps == VK_APPS);
static_assert(wi::kExtendedKey == KEYEVENTF_EXTENDEDKEY && wi::kKeyUp == KEYEVENTF_KEYUP
              && wi::kScanCode == KEYEVENTF_SCANCODE);

namespace {

constexpr UINT kCommandMessage = WM_APP + 1;
constexpr UINT kWarpDoneMessage = WM_APP + 2;
constexpr ULONGLONG kWarpTimeoutMs = 100; // backstop in case the warp sentinel is lost
constexpr UINT_PTR kDesktopTimerMs = 250;
constexpr ULONGLONG kEdgeThrottleMs = 15;
// After the cursor came back through an edge, that edge does not capture early for this long
// (the service ignores edge hits there meanwhile too, see InputController).
constexpr ULONGLONG kReturnGuardMs = 250;
constexpr int kParkSlack = 2; // pixels the parked cursor may be off before the hooks are suspect
const wchar_t kHiderClass[] = L"DeskflowerCursorHider";
// System cursors replaced by a blank one while the pointer is on another device (OCR_* values).
constexpr DWORD kSystemCursors[] = {32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644,
                                    32645, 32646, 32648, 32649, 32650, 32651, 32671, 32672};
constexpr int kBlankSize = 32;

// Everything that touches hooks, windows or SendInput runs on one "desk thread", which follows
// the input desktop (Default, Winlogon for UAC / lock screen when running as SYSTEM).
class WinInputBackend : public InputBackend
{
public:
    explicit WinInputBackend(QObject *parent) : InputBackend(parent) { s_instance = this; }

    ~WinInputBackend() override
    {
        if (m_thread.joinable()) {
            post({Command::Quit});
            m_thread.join();
        }
        s_instance = nullptr;
    }

    bool start() override
    {
        std::promise<bool> ready;
        auto started = ready.get_future();
        m_thread = std::thread([this, &ready] { run(ready); });
        return started.get();
    }

    bool isCapturing() const override { return m_capturing; }
    // The low-level hooks see every local press (injected ones are told apart by their flag),
    // while both are installed (noteHooks).
    bool reportsLocalPresses() const override { return m_hooksUp; }
    void setReportEdges(bool report) override { m_reportEdges = report; }
    void setCapture(bool on, QPoint returnTo, int guardSide) override
    {
        post({Command::Capture, on, guardSide, 0, returnTo});
    }
    void setEdgeMap(const QList<EdgeSpan> &spans) override
    {
        Command c{Command::EdgeMap};
        c.spans = std::make_shared<const std::vector<EdgeSpan>>(spans.begin(), spans.end());
        post(c);
    }
    bool leftButtonHeld() const override { return m_localLeftDown; }
    void setReportReleases(bool on) override { m_reportReleases = on; }
    void setCursorHidden(bool hidden) override
    {
        if (m_cursorHidden.exchange(hidden) != hidden)
            post({Command::CursorVisibility});
    }
    void moveTo(QPoint p) override { post({Command::Move, false, 0, 0, p}); }
    void button(int b, bool down, QPoint at) override { post({Command::Button, down, b, 0, at}); }
    void wheel(int dx, int dy) override { post({Command::Wheel, false, dx, dy, {}}); }
    void key(quint32 sc, bool down) override { post({Command::Key, down, int(sc), 0, {}}); }
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
    void setKeyDiagnostics(bool on) override
    {
        m_keyDiag = on;
        if (on)
            post({Command::KeyDiag});
    }
#endif
    void releaseAll() override { post({Command::ReleaseAll}); }

private:
    struct Command {
        enum Type { Quit, Capture, Move, Button, Wheel, Key, ReleaseAll, CursorVisibility, EdgeMap, KeyDiag } type;
        bool flag = false;
        int a = 0; // Capture off: the guard side (-1 none)
        int b = 0;
        QPoint p;
        std::shared_ptr<const std::vector<EdgeSpan>> spans;
    };

    void post(const Command &c)
    {
        {
            std::lock_guard lock(m_mutex);
            // A move behind a move not injected yet replaces it: a desk thread that was away (in
            // hook callouts, replacing cursors, following the desktop) goes to the latest position
            // instead of walking through each one late. The order with buttons, the wheel and
            // keys is kept.
            if (c.type == Command::Move && !m_commands.empty() && m_commands.back().type == Command::Move)
                m_commands.back() = c;
            else
                m_commands.push_back(c);
        }
        PostThreadMessageW(m_threadId, kCommandMessage, 0, 0);
    }

    // --- Desk thread ---------------------------------------------------------------------------

    void run(std::promise<bool> &ready)
    {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the message queue
        m_threadId = GetCurrentThreadId();
        registerHiderClass();
        recoverBlankCursors();
        followInputDesktop();
        if (!m_mouseHook || !m_keyHook)
            setUp(); // could not open the input desktop: stay on the thread's current one
        const bool ok = m_mouseHook && m_keyHook;
        ready.set_value(ok);
        if (!ok)
            return;
        SetTimer(nullptr, 0, kDesktopTimerMs, nullptr);

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && msg.message == kWarpDoneMessage) {
                if (msg.wParam == m_warpSerial)
                    m_warpPending = false;
                continue;
            }
            if (msg.hwnd == nullptr && msg.message == kCommandMessage) {
                if (!drainCommands())
                    break;
                continue;
            }
            if (msg.hwnd == nullptr && msg.message == WM_TIMER) {
                followInputDesktop();
                checkCapture();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        releaseInjected();
        showSystemCursors();
        tearDown();
    }

    bool drainCommands()
    {
        std::deque<Command> commands;
        {
            std::lock_guard lock(m_mutex);
            commands.swap(m_commands);
        }
        for (const Command &c : commands) {
            switch (c.type) {
            case Command::Quit:
                return false;
            case Command::Capture:
                if (c.flag) {
                    if (!m_capturing) {
                        beginCapture(false);
                    } else if (m_softPending) {
                        // The service confirms a capture begun at the edge: the input is away
                        // now, so the modifiers still held belong to the device it went to.
                        m_softPending = false;
                        handOverModifiers();
                    }
                } else {
                    endCapture(c.p, c.a);
                }
                break;
            case Command::EdgeMap:
                m_edgeMap = c.spans;
                break;
            case Command::Move:
                injectMove(c.p);
                break;
            case Command::Button:
                injectMove(c.p);
                injectButton(c.a, c.flag);
                break;
            case Command::Wheel:
                injectWheel(c.a, c.b);
                break;
            case Command::Key:
                injectKey(quint32(c.a), c.flag);
                break;
            case Command::ReleaseAll:
                releaseInjected();
                break;
            case Command::CursorVisibility:
                updateCursorVisibility();
                break;
            case Command::KeyDiag: {
                const QString hookDesktop = QString::fromStdWString(m_desktopName);
                const bool keyHook = m_keyHook != nullptr;
                QMetaObject::invokeMethod(this, [this, hookDesktop, keyHook] {
                    emit diagnostic(whereWeRun(hookDesktop, keyHook));
                }, Qt::QueuedConnection);
                break;
            }
            }
        }
        return true;
    }

    static std::wstring desktopName(HDESK desk)
    {
        wchar_t name[256] = {};
        DWORD needed = 0;
        GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed);
        return name;
    }

    // Moves the desk thread to the current input desktop when it changes (needs no windows or
    // hooks on the thread while switching).
    void followInputDesktop()
    {
        HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!input)
            return; // e.g. Winlogon while running as a normal user
        const std::wstring name = desktopName(input);
        if (name == m_desktopName && m_mouseHook) {
            CloseDesktop(input);
            return;
        }
        const bool wasCapturing = m_capturing;
        tearDown();
        if (SetThreadDesktop(input)) {
            if (m_desktop)
                CloseDesktop(m_desktop);
            m_desktop = input;
            m_desktopName = name;
            qCInfo(lcWinInput) << "Input desktop:" << QString::fromStdWString(name);
        } else {
            CloseDesktop(input);
        }
        setUp();
        if (wasCapturing)
            beginCapture(false);
    }

    void setUp()
    {
        const HINSTANCE module = GetModuleHandleW(nullptr);
        m_hider = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kHiderClass,
                                  L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, module, nullptr);
        if (m_hider)
            SetLayeredWindowAttributes(m_hider, 0, 1, LWA_ALPHA); // invisible but hit-testable
        m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &WinInputBackend::mouseProc, module, 0);
        m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &WinInputBackend::keyProc, module, 0);
        if (!m_mouseHook || !m_keyHook)
            qCWarning(lcWinInput) << "Cannot install input hooks, error" << GetLastError();
        noteHooks();
        refreshMonitors();
    }

    // Desk thread, after the hooks changed: whether both are in (reportsLocalPresses), told with
    // problemChanged so the service hears of it (localPressSupport).
    void noteHooks()
    {
        const bool up = m_mouseHook && m_keyHook;
        if (m_hooksUp.exchange(up) != up)
            QMetaObject::invokeMethod(this, [this] { emit problemChanged(); }, Qt::QueuedConnection);
    }

    // While capturing, the mouse hook swallows every move, so the cursor stays parked. Windows
    // silently removes low-level hooks that once took too long to answer (LowLevelHooksTimeout):
    // then the hidden cursor moves, nothing reaches the service, and the user has neither this
    // mouse nor the remote one. A cursor off its park point means that (or a program moved it):
    // the hooks are put in again and the cursor parked. If they cannot be, capture ends here.
    void checkCapture()
    {
        if (!m_capturing || m_warpPending)
            return;
        POINT p = {};
        if (!GetCursorPos(&p) || (std::abs(p.x - m_park.x) <= kParkSlack && std::abs(p.y - m_park.y) <= kParkSlack))
            return;
        qCWarning(lcWinInput) << "The cursor left its park point while captured: putting the input hooks in again";
        if (reinstallHooks()) {
            SetCursorPos(m_park.x, m_park.y);
            armWarpBarrier(p, m_park);
            return;
        }
        qCWarning(lcWinInput) << "Cannot put the input hooks in again, error" << GetLastError() << ": capture ends";
        const QPoint at(p.x, p.y);
        endCapture(at, -1);
        QMetaObject::invokeMethod(this, [this, at] { emit captureLost(at); }, Qt::QueuedConnection);
    }

    // New hooks first, then the old ones out (a removed hook's handle is simply no longer valid).
    bool reinstallHooks()
    {
        const HINSTANCE module = GetModuleHandleW(nullptr);
        HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, &WinInputBackend::mouseProc, module, 0);
        HHOOK key = SetWindowsHookExW(WH_KEYBOARD_LL, &WinInputBackend::keyProc, module, 0);
        if (!mouse || !key) {
            if (mouse)
                UnhookWindowsHookEx(mouse);
            if (key)
                UnhookWindowsHookEx(key);
            return false;
        }
        if (m_mouseHook)
            UnhookWindowsHookEx(m_mouseHook);
        if (m_keyHook)
            UnhookWindowsHookEx(m_keyHook);
        m_mouseHook = mouse;
        m_keyHook = key;
        noteHooks();
        return true;
    }

    void tearDown()
    {
        if (m_mouseHook)
            UnhookWindowsHookEx(m_mouseHook);
        if (m_keyHook)
            UnhookWindowsHookEx(m_keyHook);
        if (m_hider)
            DestroyWindow(m_hider);
        m_mouseHook = nullptr;
        m_keyHook = nullptr;
        m_hider = nullptr;
        noteHooks();
    }

    void registerHiderClass()
    {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &WinInputBackend::hiderProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kHiderClass;
        wc.hCursor = nullptr; // no cursor over the window
        RegisterClassW(&wc);
    }

    static LRESULT CALLBACK hiderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;
        case WM_DISPLAYCHANGE:
            if (s_instance)
                s_instance->refreshMonitors();
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void refreshMonitors()
    {
        m_monitors.clear();
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                MONITORINFO info = {};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info))
                    reinterpret_cast<std::vector<RECT> *>(data)->push_back(info.rcMonitor);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&m_monitors));
    }

    bool onAnyMonitor(LONG x, LONG y) const
    {
        for (const RECT &r : m_monitors) {
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
                return true;
        }
        return false;
    }

    // Side (0 left, 1 right, 2 top, 3 bottom) if pt is on an edge with no local monitor beyond.
    int outerEdge(POINT pt) const
    {
        for (const RECT &r : m_monitors) {
            if (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)
                continue;
            if (pt.x == r.left && !onAnyMonitor(pt.x - 1, pt.y))
                return 0;
            if (pt.x == r.right - 1 && !onAnyMonitor(pt.x + 1, pt.y))
                return 1;
            if (pt.y == r.top && !onAnyMonitor(pt.x, pt.y - 1))
                return 2;
            if (pt.y == r.bottom - 1 && !onAnyMonitor(pt.x, pt.y + 1))
                return 3;
        }
        return -1;
    }

    // Hook callouts are sent messages, so GetMessage dispatches every pending one before it returns
    // a message posted later: a move whose position was computed before a cursor warp (one in
    // flight while the warp happened) is seen while m_warpPending is still set. Without this, a
    // move in flight while capture starts reads as a huge delta (edge minus park point) and throws
    // the remote cursor to the middle of its screen, and one in flight while capture ends
    // overrides the return position. Such a move is near where the cursor was before the warp
    // (usually far from the warp point: capture starts with the cursor on an edge and parks it at
    // the monitor's centre; it ends at the park point and returns near an edge): a move nearer to
    // the warp point than to the old position, and within a quarter of the monitor of it, was
    // computed after the warp and is kept, since the sentinel, posted, is retrieved only after
    // every sent callout queued meanwhile (there are several while the system cursors are
    // replaced), and the timeout alone would also drop good moves on a loaded machine. When the
    // cursor was already near the warp point (a crossing held back while a drag was asked about,
    // the cursor pulled back meanwhile) only a move nearer to the old position is dropped; one
    // from the very same point is kept, as its delta is right.
    void armWarpBarrier(POINT from, POINT to)
    {
        m_warpPending = true;
        m_warpTick = GetTickCount64();
        m_warpFrom = from;
        m_warpTo = to;
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromPoint(to, MONITOR_DEFAULTTONEAREST), &info);
        m_warpNear = {(info.rcMonitor.right - info.rcMonitor.left) / 4, (info.rcMonitor.bottom - info.rcMonitor.top) / 4};
        if (!PostThreadMessageW(GetCurrentThreadId(), kWarpDoneMessage, ++m_warpSerial, 0))
            m_warpPending = false;
    }

    bool nearWarpPoint(POINT pt) const
    {
        const auto distance2 = [pt](POINT p) {
            return qint64(pt.x - p.x) * (pt.x - p.x) + qint64(pt.y - p.y) * (pt.y - p.y);
        };
        return std::abs(pt.x - m_warpTo.x) < m_warpNear.x && std::abs(pt.y - m_warpTo.y) < m_warpNear.y
               && distance2(m_warpTo) <= distance2(m_warpFrom);
    }

    // Parks the cursor and starts swallowing input. Soft: called from inside the mouse hook
    // callout for a move that touches an edge of the edge map (edgeHit tells the service, which
    // confirms or declines). The thread that feeds the hook waits for the callout to return, so
    // no move is in flight and no warp barrier is needed, and anything slow (replacing the system
    // cursors) is deferred to a command, so the callout returns at once. The hider window under
    // the park point already blanks the cursor meanwhile.
    void beginCapture(bool soft)
    {
        m_diagHeld.clear();
        // Park on the monitor under the cursor, well away from any edge.
        POINT cursor = {};
        GetCursorPos(&cursor);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info);
        m_park = {(info.rcMonitor.left + info.rcMonitor.right) / 2, (info.rcMonitor.top + info.rcMonitor.bottom) / 2};
        if (m_hider)
            SetWindowPos(m_hider, HWND_TOPMOST, m_park.x - 32, m_park.y - 32, 64, 64,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetCursorPos(m_park.x, m_park.y);
        m_capturing = true;
        if (!soft)
            armWarpBarrier(cursor, m_park);
        // A soft capture may still be declined: the modifiers are handed over only once the
        // service confirms it (drainCommands), so a declined one leaves the local desktop's Shift
        // or Ctrl as the user holds it. Meanwhile the keys held stay local (keyProc).
        m_softPending = soft;
        if (soft)
            post({Command::CursorVisibility});
        else {
            handOverModifiers();
            updateCursorVisibility();
        }
    }

    // Modifiers held while crossing (Shift, Ctrl, Alt, Win) belong to the device we move to: send
    // their presses there and release them here, so that Shift + key typed right after crossing
    // is shifted on the remote and the local desktop is not left with a stuck modifier.
    void handOverModifiers()
    {
        static const std::pair<quint32, int> kModifiers[] = {
            {0x2A, VK_LSHIFT},   {0x36, VK_RSHIFT}, {0x1D, VK_LCONTROL}, {0xE01D, VK_RCONTROL},
            {0x38, VK_LMENU},    {0xE038, VK_RMENU}, {0xE05B, VK_LWIN},   {0xE05C, VK_RWIN},
        };
        for (const auto &[sc, vk] : kModifiers) {
            if (!m_localKeys.erase(sc) || !(GetAsyncKeyState(vk) & 0x8000))
                continue;
            // Released here the way keys are injected (the Windows keys by virtual key).
            sendKey(sc, false, "hand over: local release of");
            QMetaObject::invokeMethod(this, [this, sc = sc] { emit keyEvent(sc, true); }, Qt::QueuedConnection);
        }
    }

    // guardSide >= 0: the cursor came back through that edge, which does not capture early for
    // a moment (the service ignores hits there meanwhile too); see softCaptureAllowed.
    void endCapture(QPoint returnTo, int guardSide)
    {
        m_capturing = false;
        m_softPending = false; // never confirmed: nothing was handed over, nothing to put back
        POINT was = m_park;
        GetCursorPos(&was);
        SetCursorPos(returnTo.x(), returnTo.y());
        armWarpBarrier(was, {returnTo.x(), returnTo.y()});
        if (guardSide >= 0) {
            m_guardSide = guardSide;
            m_guardTick = GetTickCount64();
        }
        if (m_hider)
            ShowWindow(m_hider, SW_HIDE);
        refreshMonitors(); // only read while not capturing
        updateCursorVisibility();
    }

    // --- Cursor visibility (desk thread) -------------------------------------------------------

    // The hider window only blanks the cursor while it is over the window and the window gets
    // WM_SETCURSOR; other applications (mouse capture, their own cursors) can still show it, and a
    // device that is not the master has no hider at all. Replacing the system cursors hides it
    // everywhere in the session. Copies of the current cursors (the user's scheme and size, which
    // SPI_SETCURSORS would not restore when running as SYSTEM) are put back afterwards.
    static HCURSOR createBlankCursor()
    {
        std::vector<BYTE> andMask(kBlankSize * kBlankSize / 8, 0xFF);
        std::vector<BYTE> xorMask(kBlankSize * kBlankSize / 8, 0x00);
        return CreateCursor(GetModuleHandleW(nullptr), 0, 0, kBlankSize, kBlankSize, andMask.data(), xorMask.data());
    }

    static bool isBlankCursor(HCURSOR cursor)
    {
        ICONINFO info = {};
        if (!cursor || !GetIconInfo(cursor, &info))
            return false;
        bool blank = false;
        BITMAP bm = {};
        if (!info.hbmColor && info.hbmMask && GetObjectW(info.hbmMask, sizeof(bm), &bm) && bm.bmWidth == kBlankSize
            && bm.bmHeight == 2 * kBlankSize) {
            std::vector<BYTE> bits(size_t(bm.bmWidthBytes) * bm.bmHeight);
            if (GetBitmapBits(info.hbmMask, LONG(bits.size()), bits.data()) == LONG(bits.size())) {
                const size_t half = bits.size() / 2;
                blank = std::all_of(bits.begin(), bits.begin() + half, [](BYTE b) { return b == 0xFF; })
                        && std::all_of(bits.begin() + half, bits.end(), [](BYTE b) { return b == 0x00; });
            }
        }
        if (info.hbmMask)
            DeleteObject(info.hbmMask);
        if (info.hbmColor)
            DeleteObject(info.hbmColor);
        return blank;
    }

    // A helper that ended without restoring them (crash, killed) leaves blank system cursors behind.
    static void recoverBlankCursors()
    {
        if (isBlankCursor(LoadCursorW(nullptr, IDC_ARROW))) {
            qCWarning(lcWinInput) << "Restoring system cursors left hidden by a previous run";
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
        }
    }

    void updateCursorVisibility()
    {
        if (m_capturing || m_cursorHidden) {
            if (!m_capturing)
                GetCursorPos(&m_hiddenAt); // where it rests; moving away from it is local input
            hideSystemCursors();
        } else {
            showSystemCursors();
        }
    }

    void hideSystemCursors()
    {
        if (!m_savedCursors.empty())
            return;
        for (DWORD id : kSystemCursors) {
            const HCURSOR current = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
            const HCURSOR copy = current ? HCURSOR(CopyIcon(current)) : nullptr;
            if (!copy)
                continue;
            if (SetSystemCursor(createBlankCursor(), id))
                m_savedCursors.emplace_back(id, copy);
            else
                DestroyCursor(copy);
        }
    }

    void showSystemCursors()
    {
        // SetSystemCursor takes ownership of the copies.
        for (const auto &[id, cursor] : m_savedCursors)
            SetSystemCursor(cursor, id);
        m_savedCursors.clear();
    }

    // --- Hooks (desk thread) -------------------------------------------------------------------

    static int buttonOf(WPARAM msg, DWORD mouseData, bool *down)
    {
        switch (msg) {
        case WM_LBUTTONDOWN: *down = true; return 1;
        case WM_LBUTTONUP: *down = false; return 1;
        case WM_RBUTTONDOWN: *down = true; return 2;
        case WM_RBUTTONUP: *down = false; return 2;
        case WM_MBUTTONDOWN: *down = true; return 3;
        case WM_MBUTTONUP: *down = false; return 3;
        case WM_XBUTTONDOWN: *down = true; return HIWORD(mouseData) == XBUTTON1 ? 4 : 5;
        case WM_XBUTTONUP: *down = false; return HIWORD(mouseData) == XBUTTON1 ? 4 : 5;
        default: return 0;
        }
    }

    // Whether a push past `side` at pt captures at once (see setEdgeMap): the edge map says a
    // device is there, no local button is held (a drag crossing waits for the service, which asks
    // what is dragged, with the cursor in sight on the edge) and the cursor did not just come back
    // through that edge.
    bool softCaptureAllowed(int side, POINT pt, ULONGLONG now) const
    {
        if (!m_edgeMap || m_localLeftDown || !m_localButtons.empty())
            return false;
        if (m_guardSide == side && now - m_guardTick < kReturnGuardMs)
            return false;
        const int along = side < 2 ? pt.y : pt.x;
        for (const EdgeSpan &s : *m_edgeMap) {
            if (s.side == side && s.screen.contains(pt.x, pt.y) && along >= s.from && along <= s.to)
                return true;
        }
        return false;
    }

    static LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp)
    {
        WinInputBackend *self = s_instance;
        const auto *info = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        if (code < 0 || !self)
            return CallNextHookEx(nullptr, code, wp, lp);
        if (info->flags & LLMHF_INJECTED) {
            if (wp == WM_MOUSEMOVE) {
                self->m_injectedPt = info->pt; // where the move injectMove just sent lands
                self->m_injectedSeen = true;
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        if (wp == WM_MOUSEMOVE && self->m_warpPending) {
            if (self->nearWarpPoint(info->pt))
                self->m_warpPending = false; // computed after the warp: a delta or position to keep
            else if (GetTickCount64() - self->m_warpTick < kWarpTimeoutMs)
                return 1; // computed before the warp: neither a delta nor a position to keep
            else
                self->m_warpPending = false; // sentinel lost: never freeze the mouse
        }

        bool down = false;
        const int button = buttonOf(wp, info->mouseData, &down);

        if (!self->m_capturing) {
            // Someone uses this computer's own mouse: the cursor belongs here again.
            if (self->m_cursorHidden
                && (wp != WM_MOUSEMOVE || info->pt.x != self->m_hiddenAt.x || info->pt.y != self->m_hiddenAt.y))
                self->setCursorHidden(false);
            if (button && down) {
                self->m_localButtons.insert(button);
                QMetaObject::invokeMethod(self, [self] { emit self->localPasteGesture(); }, Qt::QueuedConnection);
            } else if (button)
                self->m_localButtons.erase(button);
            if (button == 1)
                self->m_localLeftDown = down;
            if (button == 1 && !down && self->m_reportReleases) {
                const QPoint at(info->pt.x, info->pt.y);
                QMetaObject::invokeMethod(self, [self, at] { emit self->localRelease(at); }, Qt::QueuedConnection);
            }
            if (wp == WM_MOUSEMOVE && self->m_reportEdges) {
                const int side = self->outerEdge(info->pt);
                const ULONGLONG now = GetTickCount64();
                if (side >= 0 && self->softCaptureAllowed(side, info->pt, now)) {
                    // Known to cross: capture here and now, so that nothing moved while the service
                    // answers is lost (the system would hold the cursor on the edge meanwhile, and
                    // the push past it is nowhere in pt). The edge move itself is swallowed; the
                    // next one is already a delta from the park point. The edge is queued before
                    // capturing: the presses of the modifiers handed over then follow it to the
                    // service, which only forwards them once the input is away.
                    const QPoint at(info->pt.x, info->pt.y);
                    QMetaObject::invokeMethod(self, [self, side, at] { emit self->edgeHit(side, at, true); },
                                              Qt::QueuedConnection);
                    self->beginCapture(true);
                    self->m_lastEdgeTick = now;
                    return 1;
                }
                if (side >= 0 && now - self->m_lastEdgeTick >= kEdgeThrottleMs) {
                    self->m_lastEdgeTick = now;
                    const QPoint at(info->pt.x, info->pt.y);
                    QMetaObject::invokeMethod(self, [self, side, at] { emit self->edgeHit(side, at, false); },
                                              Qt::QueuedConnection);
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        if (wp == WM_MOUSEMOVE) {
            const int dx = info->pt.x - self->m_park.x;
            const int dy = info->pt.y - self->m_park.y;
            if (dx || dy)
                QMetaObject::invokeMethod(self, [self, dx, dy] { self->addMove(dx, dy); }, Qt::QueuedConnection);
            return 1;
        }
        if (button) {
            // A release whose press happened locally belongs to the local desktop. It is reported
            // too: it ends a file drag that crossed to another device (the controller does not
            // forward releases of buttons it never pressed there).
            if (!down && self->m_localButtons.erase(button)) {
                if (button == 1)
                    self->m_localLeftDown = false;
                QMetaObject::invokeMethod(self, [self, button] {
                    self->flushMoves();
                    emit self->buttonEvent(button, false);
                },
                                          Qt::QueuedConnection);
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            QMetaObject::invokeMethod(self, [self, button, down] {
                self->flushMoves();
                emit self->buttonEvent(button, down);
            },
                                      Qt::QueuedConnection);
            return 1;
        }
        if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {
            const int delta = short(HIWORD(info->mouseData));
            const int dx = wp == WM_MOUSEHWHEEL ? delta : 0;
            const int dy = wp == WM_MOUSEWHEEL ? delta : 0;
            QMetaObject::invokeMethod(self, [self, dx, dy] {
                self->flushMoves();
                emit self->wheelEvent(dx, dy);
            }, Qt::QueuedConnection);
            return 1;
        }
        return 1;
    }

    static LRESULT CALLBACK keyProc(int code, WPARAM wp, LPARAM lp)
    {
        WinInputBackend *self = s_instance;
        const auto *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
        if (code < 0 || !self)
            return CallNextHookEx(nullptr, code, wp, lp);
        if (info->flags & LLKHF_INJECTED) {
            // Diagnostics: what the system made of our own injection, told by the mark sendKey
            // puts on it (other programs' injected keys are not recorded).
            if (self->keyDiag() && info->dwExtraInfo == kInjectMark) {
                if (self->m_injecting)
                    self->m_injectSeen << describeHookEvent(info);
                else
                    self->diag(QStringLiteral("hook: after the injection: ") + describeHookEvent(info));
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // Right Shift travels as 0x36 and the Windows and Menu keys by their virtual key (see
        // keyinjection.h); a key without a scan code gets the one its virtual key maps to.
        quint32 sc = wi::hookScancode(info->vkCode, info->scanCode, info->flags & LLKHF_EXTENDED);
        if (!sc) {
            const UINT mapped = MapVirtualKeyW(info->vkCode, MAPVK_VK_TO_VSC_EX);
            sc = (mapped & 0xFF) ? ((mapped & 0xFF) | ((mapped & 0xFF00) ? 0xE000 : 0)) : 0;
        }
        const bool down = !(info->flags & LLKHF_UP);

        if (!self->m_capturing) {
            if (down) {
                // Autorepeat (the key already held) is no new press.
                const bool gesture = !self->m_localKeys.count(sc)
                                     && df::keymap::isPasteEvidenceKey(self->m_localKeys, sc, false);
                self->m_localKeys.insert(sc);
                if (gesture)
                    QMetaObject::invokeMethod(self, [self] { emit self->localPasteGesture(); }, Qt::QueuedConnection);
            } else {
                self->m_localKeys.erase(sc);
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        // Keys held down when capture started stay local, autorepeat included, until released.
        if ((!down && self->m_localKeys.erase(sc)) || (down && self->m_localKeys.count(sc))) {
            // A key typed for this device: not recorded, only that a modifier stayed here (its
            // release), which matters for a combination.
            if (self->keyDiag() && !down) {
                const QString modifier = modifierName(info->vkCode);
                if (!modifier.isEmpty())
                    self->diag(QStringLiteral("hook: %1 held since before the capture, released here (kept local)").arg(modifier));
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        // Autorepeat is not recorded here (the service counts it): a key once down, once up.
        if (self->keyDiag() && (!down || self->m_diagHeld.insert(sc).second)) {
            if (!down)
                self->m_diagHeld.erase(sc);
            self->diag(QStringLiteral("hook: %1 -> %2")
                           .arg(describeHookEvent(info),
                                (sc & 0xFF) ? QStringLiteral("swallowed, forwarded as sc=%1").arg(hex(sc))
                                            : QStringLiteral("swallowed, no scan code: not forwarded")));
        }
        if ((sc & 0xFF) != 0)
            QMetaObject::invokeMethod(self, [self, sc, down] {
                self->flushMoves();
                emit self->keyEvent(sc, down);
            }, Qt::QueuedConnection);
        return 1;
    }

    // --- Injection (desk thread) ---------------------------------------------------------------

    void injectMove(QPoint p)
    {
        // A real (absolute) move through SendInput: applications only see mouse input, and drags
        // (text and rubber-band selection, drag and drop) follow, with a button held, moves that go
        // through the input system. SetCursorPos plus an empty relative move did not carry the
        // motion there, and drags on the remote screen did not work.
        // Absolute coordinates are normalised to 0..65535 over the virtual desktop and may round by
        // a pixel (and a move to its left/top edge can be ignored): SetCursorPos, exact in physical
        // pixels for a per-monitor-aware process, corrects the position afterwards.
        // The mouse hook sees the injected move on this very thread, inside SendInput, and the
        // system applies it before SendInput returns (measured), so where it landed is known here
        // from the callout: only a move that landed elsewhere is corrected, and local motion on
        // this device meanwhile is not overwritten. Without the callout (the hooks were taken
        // away) the cursor position says.
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = LONG((qint64(p.x() - vx) * 65535 + (vw - 1) / 2) / qMax(1, vw - 1));
        in.mi.dy = LONG((qint64(p.y() - vy) * 65535 + (vh - 1) / 2) / qMax(1, vh - 1));
        m_injectedSeen = false;
        SendInput(1, &in, sizeof(in));
        POINT landed = m_injectedPt;
        if (!m_injectedSeen && !GetCursorPos(&landed))
            return;
        if (landed.x != p.x() || landed.y != p.y())
            SetCursorPos(p.x(), p.y());
    }

    void injectButton(int button, bool down)
    {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        switch (button) {
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 4:
        case 5:
            in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            in.mi.mouseData = button == 4 ? XBUTTON1 : XBUTTON2;
            break;
        default: return;
        }
        if (down)
            m_injectedButtons.insert(button);
        else
            m_injectedButtons.erase(button);
        SendInput(1, &in, sizeof(in));
    }

    void injectWheel(int dx, int dy)
    {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        if (dy) {
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = DWORD(dy);
            SendInput(1, &in, sizeof(in));
        }
        if (dx) {
            in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in.mi.mouseData = DWORD(dx);
            SendInput(1, &in, sizeof(in));
        }
    }

    void injectKey(quint32 sc, bool down)
    {
        if (sc == 0xE036)
            sc = 0x36; // Right Shift as sent by older versions
        bool repeat = false;
        if (down)
            repeat = !m_injectedKeys.insert(sc).second;
        else
            m_injectedKeys.erase(sc);
        sendKey(sc, down, "inject", repeat);
    }

    // One key through SendInput as keyinjection.h says (by scan code; the Windows and Menu keys by
    // virtual key), described for the diagnostics with what the hook saw meanwhile (not for an
    // autorepeat: the service counts those). The event carries kInjectMark, which tells the hook
    // our own injection from other programs'.
    void sendKey(quint32 sc, bool down, const char *what, bool repeat = false)
    {
        const wi::KeyInput k = wi::keyInput(sc, down);
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = k.vk;
        in.ki.wScan = k.scan;
        in.ki.dwFlags = k.flags;
        in.ki.dwExtraInfo = kInjectMark;
        if (!keyDiag()) {
            SendInput(1, &in, sizeof(in));
            return;
        }
        m_injectSeen.clear();
        m_injecting = true;
        SetLastError(0);
        const UINT sent = SendInput(1, &in, sizeof(in));
        const DWORD error = GetLastError();
        m_injecting = false;
        if (repeat)
            return;
        QString line = QStringLiteral("%1 sc=%2 %3: INPUT_KEYBOARD wVk=%4 wScan=%5 dwFlags=%6")
                           .arg(QLatin1String(what), hex(sc), down ? QStringLiteral("down") : QStringLiteral("up"),
                                hex(k.vk), hex(k.scan), describeInjectFlags(k.flags));
        line += QStringLiteral(" -> SendInput=%1 error=%2").arg(sent).arg(error);
        line += m_injectSeen.isEmpty() ? QStringLiteral("; hook saw nothing during SendInput")
                                       : QStringLiteral("; hook saw: ") + m_injectSeen.join(QStringLiteral(", "));
        diag(line, down && k.vk); // the foreground program, for the Windows and Menu keys
    }

    // --- Keyboard diagnostics ------------------------------------------------------------------

    // Ours (dwExtraInfo of the keys sendKey injects): what the hook sees with it is our own
    // injection. Clear of the bits Windows uses to mark pen and touch input (0xFFFFFF00 ==
    // 0xFF515700).
    static constexpr ULONG_PTR kInjectMark = 0x44464B49; // "DFKI"

    // Any thread: one line for the service (main thread), with the foreground program found
    // there (not on the desk thread, which must answer the hooks quickly).
    void diag(const QString &line, bool withForeground = false)
    {
        QMetaObject::invokeMethod(this, [this, line, withForeground] {
            emit diagnostic(withForeground ? line + QStringLiteral("; foreground: ") + foregroundProgram() : line);
        }, Qt::QueuedConnection);
    }

    static QString modifierName(DWORD vk)
    {
        switch (vk) {
        case VK_LWIN: return QStringLiteral("LWin");
        case VK_RWIN: return QStringLiteral("RWin");
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return QStringLiteral("Shift");
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return QStringLiteral("Ctrl");
        case VK_LMENU: case VK_RMENU: case VK_MENU: return QStringLiteral("Alt");
        default: return {};
        }
    }

    static QString hex(quint32 v)
    {
        return QStringLiteral("0x") + QString::number(v, 16).toUpper().rightJustified(v > 0xFF ? 4 : 2, QLatin1Char('0'));
    }

    static QString describeHookEvent(const KBDLLHOOKSTRUCT *info)
    {
        QStringList flags;
        if (info->flags & LLKHF_EXTENDED)
            flags << QStringLiteral("EXTENDED");
        if (info->flags & LLKHF_INJECTED)
            flags << QStringLiteral("INJECTED");
        if (info->flags & LLKHF_LOWER_IL_INJECTED)
            flags << QStringLiteral("LOWER_IL_INJECTED");
        if (info->flags & LLKHF_ALTDOWN)
            flags << QStringLiteral("ALTDOWN");
        return QStringLiteral("%1 vk=%2 scan=%3 flags=%4(%5)")
            .arg((info->flags & LLKHF_UP) ? QStringLiteral("up") : QStringLiteral("down"), hex(info->vkCode),
                 hex(info->scanCode), hex(info->flags), flags.join(QLatin1Char('|')));
    }

    static QString describeInjectFlags(quint32 flags)
    {
        QStringList names;
        if (flags & KEYEVENTF_EXTENDEDKEY)
            names << QStringLiteral("EXTENDEDKEY");
        if (flags & KEYEVENTF_KEYUP)
            names << QStringLiteral("KEYUP");
        if (flags & KEYEVENTF_SCANCODE)
            names << QStringLiteral("SCANCODE");
        return QStringLiteral("%1(%2)").arg(hex(flags), names.join(QLatin1Char('|')));
    }

    // The executable of the foreground window (file name only) and its keyboard layout.
    static QString foregroundProgram()
    {
        const HWND window = GetForegroundWindow();
        if (!window)
            return QStringLiteral("none");
        DWORD pid = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &pid);
        QString name = QStringLiteral("pid %1").arg(pid);
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                const QString full = QString::fromWCharArray(path, int(size));
                name = full.mid(full.lastIndexOf(QLatin1Char('\\')) + 1) + QStringLiteral(" (pid %1)").arg(pid);
            }
            CloseHandle(process);
        }
        const HKL layout = GetKeyboardLayout(thread);
        return name + QStringLiteral(", layout %1").arg(hex(quint32(reinterpret_cast<quintptr>(layout) & 0xFFFFFFFF)));
    }

    // Main thread (the desk thread, which answers the hooks, only says where its hooks are):
    // where the helper runs (who, integrity, session, desktop, foreground program).
    QString whereWeRun(const QString &hookDesktop, bool keyHook) const
    {
        QString user = QStringLiteral("?");
        QString integrity = QStringLiteral("?");
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            BYTE buffer[512] = {};
            DWORD size = 0;
            if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
                wchar_t name[256] = {}, domain[256] = {};
                DWORD nameSize = 256, domainSize = 256;
                SID_NAME_USE use;
                if (LookupAccountSidW(nullptr, reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid, name, &nameSize,
                                      domain, &domainSize, &use))
                    user = QString::fromWCharArray(domain) + QLatin1Char('\\') + QString::fromWCharArray(name);
            }
            if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
                PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer)->Label.Sid;
                const DWORD rid = *GetSidSubAuthority(sid, DWORD(*GetSidSubAuthorityCount(sid) - 1));
                integrity = rid >= SECURITY_MANDATORY_SYSTEM_RID   ? QStringLiteral("system")
                            : rid >= SECURITY_MANDATORY_HIGH_RID   ? QStringLiteral("high")
                            : rid >= SECURITY_MANDATORY_MEDIUM_RID ? QStringLiteral("medium")
                                                                   : QStringLiteral("low");
                integrity += QStringLiteral(" (%1)").arg(hex(rid));
            }
            CloseHandle(token);
        }
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        QString input = QStringLiteral("? (cannot open it)");
        if (HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)) {
            input = QString::fromStdWString(desktopName(desk));
            CloseDesktop(desk);
        }
        return QStringLiteral("where: user %1, integrity %2, session %3, input desktop %4, hooks on %5 (%6), "
                              "capturing %7, foreground %8, LWin maps to scan %9")
            .arg(user, integrity)
            .arg(session)
            .arg(input, hookDesktop, keyHook ? QStringLiteral("installed") : QStringLiteral("missing"),
                 m_capturing ? QStringLiteral("yes") : QStringLiteral("no"), foregroundProgram(),
                 hex(MapVirtualKeyW(VK_LWIN, MAPVK_VK_TO_VSC_EX)));
    }

    void releaseInjected()
    {
        const auto keys = m_injectedKeys;
        for (quint32 sc : keys)
            injectKey(sc, false);
        const auto buttons = m_injectedButtons;
        for (int b : buttons)
            injectButton(b, false);
    }

    // Main thread. Captured moves that reach it together (it was busy) go out as one: the sum,
    // after the moves queued so far. Buttons, the wheel and keys send what is pending first.
    void addMove(int dx, int dy)
    {
        m_moveDx += dx;
        m_moveDy += dy;
        if (!m_moveFlushQueued) {
            m_moveFlushQueued = true;
            QMetaObject::invokeMethod(this, [this] { flushMoves(); }, Qt::QueuedConnection);
        }
    }
    void flushMoves()
    {
        m_moveFlushQueued = false;
        if (!m_moveDx && !m_moveDy)
            return;
        const int dx = std::exchange(m_moveDx, 0), dy = std::exchange(m_moveDy, 0);
        emit moved(dx, dy);
    }

    static inline WinInputBackend *s_instance = nullptr;
    int m_moveDx = 0, m_moveDy = 0; // main thread
    bool m_moveFlushQueued = false;

    std::thread m_thread;
    std::atomic<DWORD> m_threadId = 0;
    std::mutex m_mutex;
    std::deque<Command> m_commands;
    std::atomic_bool m_capturing = false;
    bool m_softPending = false; // desk thread: a capture begun at the edge, not confirmed yet
    std::atomic_bool m_reportEdges = false;
    std::atomic_bool m_cursorHidden = false; // another device has the pointer
    std::atomic_bool m_localLeftDown = false;
    std::atomic_bool m_reportReleases = false;
    std::atomic_bool m_keyDiag = false; // keyboard diagnostics (setKeyDiagnostics)
    // Whether to record: never without keyboard diagnostics built (the recording code compiles away).
    bool keyDiag() const
    {
#ifdef DESKFLOWER_KEY_DIAGNOSTICS
        return m_keyDiag;
#else
        return false;
#endif
    }
    std::atomic_bool m_hooksUp = false; // both low-level hooks installed (noteHooks)

    // Desk-thread state.
    HDESK m_desktop = nullptr;
    std::wstring m_desktopName;
    bool m_injecting = false;       // inside sendKey's SendInput (diagnostics)
    QStringList m_injectSeen;       // what the hook saw meanwhile
    std::set<quint32> m_diagHeld;   // keys the hook recorded down while capturing (autorepeat)
    HHOOK m_mouseHook = nullptr;
    HHOOK m_keyHook = nullptr;
    HWND m_hider = nullptr;
    std::vector<RECT> m_monitors;
    POINT m_park = {};
    POINT m_hiddenAt = {};
    std::vector<std::pair<DWORD, HCURSOR>> m_savedCursors;
    bool m_warpPending = false;
    ULONGLONG m_warpTick = 0;
    WPARAM m_warpSerial = 0;
    POINT m_warpFrom = {}; // where the cursor was before the warp
    POINT m_warpTo = {};   // where the cursor was warped to
    POINT m_warpNear = {}; // a move within this of m_warpTo (and nearer to it than to m_warpFrom)
                           // was computed after the warp
    ULONGLONG m_lastEdgeTick = 0;
    std::shared_ptr<const std::vector<EdgeSpan>> m_edgeMap; // from the service, see setEdgeMap
    int m_guardSide = -1; // edge the cursor came back through (no early capture for a moment)
    ULONGLONG m_guardTick = 0;
    POINT m_injectedPt = {}; // the injected move the hook last saw (see injectMove)
    bool m_injectedSeen = false;
    std::set<int> m_localButtons;
    std::set<quint32> m_localKeys;
    std::set<int> m_injectedButtons;
    std::set<quint32> m_injectedKeys;
};

} // namespace

InputBackend *InputBackend::create(QObject *parent) { return new WinInputBackend(parent); }
