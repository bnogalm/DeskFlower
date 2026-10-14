#include "inputbackend.h"

#include <QLoggingCategory>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(lcWinInput, "deskflower.input.win")

namespace {

constexpr UINT kCommandMessage = WM_APP + 1;
constexpr UINT kWarpDoneMessage = WM_APP + 2;
constexpr ULONGLONG kWarpTimeoutMs = 100; // backstop in case the warp sentinel is lost
constexpr UINT_PTR kDesktopTimerMs = 250;
constexpr ULONGLONG kEdgeThrottleMs = 15;
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
    void setReportEdges(bool report) override { m_reportEdges = report; }
    void setCapture(bool on, QPoint returnTo) override { post({Command::Capture, on, 0, 0, returnTo}); }
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
    void releaseAll() override { post({Command::ReleaseAll}); }

private:
    struct Command {
        enum Type { Quit, Capture, Move, Button, Wheel, Key, ReleaseAll, CursorVisibility } type;
        bool flag = false;
        int a = 0;
        int b = 0;
        QPoint p;
    };

    void post(const Command &c)
    {
        {
            std::lock_guard lock(m_mutex);
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
                c.flag ? beginCapture() : endCapture(c.p);
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
            beginCapture();
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
        refreshMonitors();
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
    // a message posted later: each move whose position was computed before a cursor warp is seen
    // while m_warpPending is still set, and dropped. Without this, a move in flight while capture
    // starts reads as a huge delta (edge minus park point) and throws the remote cursor to the
    // middle of its screen, and one in flight while capture ends overrides the return position.
    void armWarpBarrier()
    {
        m_warpPending = true;
        m_warpTick = GetTickCount64();
        if (!PostThreadMessageW(GetCurrentThreadId(), kWarpDoneMessage, ++m_warpSerial, 0))
            m_warpPending = false;
    }

    void beginCapture()
    {
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
        armWarpBarrier();
        handOverModifiers();
        updateCursorVisibility();
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
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = WORD(sc & 0xFF);
            in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | ((sc & 0xFF00) ? KEYEVENTF_EXTENDEDKEY : 0);
            SendInput(1, &in, sizeof(in));
            QMetaObject::invokeMethod(this, [this, sc = sc] { emit keyEvent(sc, true); }, Qt::QueuedConnection);
        }
    }

    void endCapture(QPoint returnTo)
    {
        m_capturing = false;
        SetCursorPos(returnTo.x(), returnTo.y());
        armWarpBarrier();
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

    static LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp)
    {
        WinInputBackend *self = s_instance;
        const auto *info = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        if (code < 0 || !self || (info->flags & LLMHF_INJECTED))
            return CallNextHookEx(nullptr, code, wp, lp);

        if (wp == WM_MOUSEMOVE && self->m_warpPending) {
            if (GetTickCount64() - self->m_warpTick < kWarpTimeoutMs)
                return 1; // computed before the warp: neither a delta nor a position to keep
            self->m_warpPending = false; // sentinel lost: never freeze the mouse
        }

        bool down = false;
        const int button = buttonOf(wp, info->mouseData, &down);

        if (!self->m_capturing) {
            // Someone uses this computer's own mouse: the cursor belongs here again.
            if (self->m_cursorHidden
                && (wp != WM_MOUSEMOVE || info->pt.x != self->m_hiddenAt.x || info->pt.y != self->m_hiddenAt.y))
                self->setCursorHidden(false);
            if (button && down)
                self->m_localButtons.insert(button);
            else if (button)
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
                if (side >= 0 && now - self->m_lastEdgeTick >= kEdgeThrottleMs) {
                    self->m_lastEdgeTick = now;
                    const QPoint at(info->pt.x, info->pt.y);
                    QMetaObject::invokeMethod(self, [self, side, at] { emit self->edgeHit(side, at); },
                                              Qt::QueuedConnection);
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        if (wp == WM_MOUSEMOVE) {
            const int dx = info->pt.x - self->m_park.x;
            const int dy = info->pt.y - self->m_park.y;
            if (dx || dy)
                QMetaObject::invokeMethod(self, [self, dx, dy] { emit self->moved(dx, dy); }, Qt::QueuedConnection);
            return 1;
        }
        if (button) {
            // A release whose press happened locally belongs to the local desktop. It is reported
            // too: it ends a file drag that crossed to another device (the controller does not
            // forward releases of buttons it never pressed there).
            if (!down && self->m_localButtons.erase(button)) {
                if (button == 1)
                    self->m_localLeftDown = false;
                QMetaObject::invokeMethod(self, [self, button] { emit self->buttonEvent(button, false); },
                                          Qt::QueuedConnection);
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            QMetaObject::invokeMethod(self, [self, button, down] { emit self->buttonEvent(button, down); },
                                      Qt::QueuedConnection);
            return 1;
        }
        if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {
            const int delta = short(HIWORD(info->mouseData));
            const int dx = wp == WM_MOUSEHWHEEL ? delta : 0;
            const int dy = wp == WM_MOUSEWHEEL ? delta : 0;
            QMetaObject::invokeMethod(self, [self, dx, dy] { emit self->wheelEvent(dx, dy); }, Qt::QueuedConnection);
            return 1;
        }
        return 1;
    }

    static LRESULT CALLBACK keyProc(int code, WPARAM wp, LPARAM lp)
    {
        WinInputBackend *self = s_instance;
        const auto *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
        if (code < 0 || !self || (info->flags & LLKHF_INJECTED))
            return CallNextHookEx(nullptr, code, wp, lp);

        // The hook flags Right Shift as extended, but E0 36 is not Shift when injected.
        const quint32 sc = info->vkCode == VK_RSHIFT
                               ? 0x36
                               : (info->scanCode & 0xFF) | ((info->flags & LLKHF_EXTENDED) ? 0xE000 : 0);
        const bool down = !(info->flags & LLKHF_UP);

        if (!self->m_capturing) {
            if (down)
                self->m_localKeys.insert(sc);
            else
                self->m_localKeys.erase(sc);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        // Keys held down when capture started stay local, autorepeat included, until released.
        if (!down && self->m_localKeys.erase(sc))
            return CallNextHookEx(nullptr, code, wp, lp);
        if (down && self->m_localKeys.count(sc))
            return CallNextHookEx(nullptr, code, wp, lp);
        if ((sc & 0xFF) != 0)
            QMetaObject::invokeMethod(self, [self, sc, down] { emit self->keyEvent(sc, down); }, Qt::QueuedConnection);
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
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = LONG((qint64(p.x() - vx) * 65535 + (vw - 1) / 2) / qMax(1, vw - 1));
        in.mi.dy = LONG((qint64(p.y() - vy) * 65535 + (vh - 1) / 2) / qMax(1, vh - 1));
        SendInput(1, &in, sizeof(in));
        POINT now = {};
        if (!GetCursorPos(&now) || now.x != p.x() || now.y != p.y())
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
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = WORD(sc & 0xFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | ((sc & 0xFF00) ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
        if (down)
            m_injectedKeys.insert(sc);
        else
            m_injectedKeys.erase(sc);
        SendInput(1, &in, sizeof(in));
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

    static inline WinInputBackend *s_instance = nullptr;

    std::thread m_thread;
    std::atomic<DWORD> m_threadId = 0;
    std::mutex m_mutex;
    std::deque<Command> m_commands;
    std::atomic_bool m_capturing = false;
    std::atomic_bool m_reportEdges = false;
    std::atomic_bool m_cursorHidden = false; // another device has the pointer
    std::atomic_bool m_localLeftDown = false;
    std::atomic_bool m_reportReleases = false;

    // Desk-thread state.
    HDESK m_desktop = nullptr;
    std::wstring m_desktopName;
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
    ULONGLONG m_lastEdgeTick = 0;
    std::set<int> m_localButtons;
    std::set<quint32> m_localKeys;
    std::set<int> m_injectedButtons;
    std::set<quint32> m_injectedKeys;
};

} // namespace

InputBackend *InputBackend::create(QObject *parent) { return new WinInputBackend(parent); }
