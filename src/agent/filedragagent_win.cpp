#include "filedragagent.h"

#include "virtualfiles.h"

#include <QCursor>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QDir>
#include <QLabel>
#include <QLoggingCategory>

#include <windows.h>
#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN leaves OLE out of windows.h
#include <exdisp.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <atomic>
#include <thread>

Q_LOGGING_CATEGORY(lcDragAgent, "deskflower.agent.drag")

namespace {

const wchar_t kCatcherClass[] = L"DeskflowerDragCatcher";

// Injected absolute move (normalised over the virtual desktop), as real mouse input: a drag loop
// only looks for a new drop target when the mouse moves.
void moveTo(POINT p)
{
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = LONG((qint64(p.x - vx) * 65535 + (vw - 1) / 2) / qMax(1, vw - 1));
    in.mi.dy = LONG((qint64(p.y - vy) * 65535 + (vh - 1) / 2) / qMax(1, vh - 1));
    SendInput(1, &in, sizeof(in));
}

// A one-pixel wiggle: a drag loop re-evaluates (target, end of drag) on mouse input.
void wiggle()
{
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    in[0].mi.dx = 1;
    in[1].mi.dx = -1;
    SendInput(2, in, sizeof(INPUT));
}

void releaseLeft()
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

// Accepts nothing; only reads which files are dragged over it.
class CatchTarget final : public IDropTarget
{
public:
    explicit CatchTarget(std::function<void(const QStringList &)> onFiles) : m_onFiles(std::move(onFiles)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&m_ref)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG ref = InterlockedDecrement(&m_ref);
        if (ref == 0)
            delete this;
        return ULONG(ref);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effect) override
    {
        *effect = DROPEFFECT_NONE;
        FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        if (!data || FAILED(data->GetData(&format, &medium)))
            return S_OK; // not files (text, a window...): ignored
        QStringList paths;
        if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 4] = {};
                if (DragQueryFileW(drop, i, path, UINT(std::size(path))))
                    paths << QDir::fromNativeSeparators(QString::fromWCharArray(path));
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        if (!paths.isEmpty() && m_onFiles)
            std::exchange(m_onFiles, nullptr)(paths);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    LONG m_ref = 1;
    std::function<void(const QStringList &)> m_onFiles;
};

// Ends when told, not with this device's button (it may not be held here at all).
class HoverSource final : public IDropSource
{
public:
    enum class State { Dragging, Drop, Cancel };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&m_ref)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG ref = InterlockedDecrement(&m_ref);
        if (ref == 0)
            delete this;
        return ULONG(ref);
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL, DWORD) override
    {
        // Escape is the master's to decide too: keys typed here go to the device with the cursor.
        switch (state.load()) {
        case State::Drop: return DRAGDROP_S_DROP;
        case State::Cancel: return DRAGDROP_S_CANCEL;
        case State::Dragging: break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

    std::atomic<State> state = State::Dragging; // set from the agent's main thread

private:
    LONG m_ref = 1;
};

// The originals as the shell's own data object (as Explorer drags them).
IDataObject *shellDataObject(const QStringList &paths)
{
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const QString &p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(p).utf16()),
                                         nullptr, &pidl, 0, nullptr)))
            pidls.push_back(pidl);
    }
    IDataObject *data = nullptr;
    IShellItemArray *items = nullptr;
    if (!pidls.empty()
        && SUCCEEDED(SHCreateShellItemArrayFromIDLists(UINT(pidls.size()),
                                                        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items))) {
        items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        items->Release();
    }
    for (PIDLIST_ABSOLUTE pidl : pidls)
        CoTaskMemFree(pidl);
    return data;
}

// The dragged files' icon (the first one's type), with their count, as the drag image.
void setDragImage(IDataObject *data, const QString &firstName, bool folder, int count)
{
    SHFILEINFOW info = {};
    SHGetFileInfoW(reinterpret_cast<const wchar_t *>(firstName.utf16()),
                   folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                   SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    QImage icon = info.hIcon ? QImage::fromHICON(info.hIcon) : QImage();
    if (info.hIcon)
        DestroyIcon(info.hIcon);
    const int size = 96;
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!icon.isNull())
            p.drawImage(QRect(16, 12, 64, 64), icon);
        if (count > 1) {
            const QRect badge(size - 38, 4, 32, 24);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x00, 0x89, 0x7b));
            p.drawRoundedRect(badge, 12, 12);
            p.setPen(Qt::white);
            QFont font = p.font();
            font.setBold(true);
            font.setPixelSize(14);
            p.setFont(font);
            p.drawText(badge, Qt::AlignCenter, QString::number(count));
        }
    }
    IDragSourceHelper *helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))))
        return;
    SHDRAGIMAGE drag = {};
    drag.sizeDragImage = {size, size};
    drag.ptOffset = {size / 2, size / 2};
    drag.hbmpDragImage = image.toHBITMAP();
    drag.crColorKey = CLR_NONE;
    if (FAILED(helper->InitializeFromBitmap(&drag, data)))
        DeleteObject(drag.hbmpDragImage); // otherwise the helper owns it
    helper->Release();
}

LRESULT CALLBACK catcherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Folder shown by an Explorer window (the active tab of it).
QString explorerFolder(HWND root)
{
    QString result;
    IShellWindows *windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
        return {};
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count && result.isEmpty(); ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        IDispatch *dispatch = nullptr;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch)
            continue;
        IWebBrowserApp *app = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&app)))) {
            SHANDLE_PTR hwnd = 0;
            app->get_HWND(&hwnd);
            IServiceProvider *provider = nullptr;
            if (HWND(hwnd) == root && SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&provider)))) {
                IShellBrowser *browser = nullptr;
                if (SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) {
                    HWND tab = nullptr;
                    browser->GetWindow(&tab); // tabs share the frame: only the visible one counts
                    IShellView *view = nullptr;
                    if ((!tab || IsWindowVisible(tab)) && SUCCEEDED(browser->QueryActiveShellView(&view))) {
                        IFolderView *folderView = nullptr;
                        if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folderView)))) {
                            IPersistFolder2 *folder = nullptr;
                            if (SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(&folder)))) {
                                PIDLIST_ABSOLUTE pidl = nullptr;
                                if (SUCCEEDED(folder->GetCurFolder(&pidl))) {
                                    wchar_t path[MAX_PATH * 4] = {};
                                    if (SHGetPathFromIDListEx(pidl, path, DWORD(std::size(path)), GPFIDL_DEFAULT))
                                        result = QString::fromWCharArray(path);
                                    CoTaskMemFree(pidl);
                                }
                                folder->Release();
                            }
                            folderView->Release();
                        }
                        view->Release();
                    }
                    browser->Release();
                }
                provider->Release();
            }
            app->Release();
        }
        dispatch->Release();
    }
    windows->Release();
    return result;
}

} // namespace

struct FileDragAgent::Hover {
    HoverSpec spec;
    ClipboardBridge::FetchFiles fetch;
    std::function<void(bool, bool)> done;
    HoverSource *source = nullptr; // while the drag runs (on its own thread)
    std::atomic_bool transferred = false;
    std::atomic<DWORD> thread = 0;
    POINT lastFed = {LONG_MIN, LONG_MIN};
    HoverSource::State requested = HoverSource::State::Dragging; // asked before the drag started
};

struct FileDragAgent::Catcher {
    HWND window = nullptr;
    CatchTarget *target = nullptr;
    POINT at = {};
    QStringList paths;
    std::function<void(const QStringList &)> done;
    QTimer timeout;
};

FileDragAgent::FileDragAgent(QObject *parent) : QObject(parent)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = catcherProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCatcherClass;
    RegisterClassW(&wc);

    m_feed.setInterval(15);
    connect(&m_feed, &QTimer::timeout, this, &FileDragAgent::feedDrag);

    m_follow.setInterval(30);
    connect(&m_follow, &QTimer::timeout, this, [this] {
        if (m_badge)
            m_badge->move(QCursor::pos() + QPoint(18, 22));
    });
}

FileDragAgent::~FileDragAgent()
{
    if (m_catcher)
        finishCatch();
}

void FileDragAgent::hover(const HoverSpec &spec, ClipboardBridge::FetchFiles fetch, std::function<void(bool, bool)> done)
{
    auto next = std::make_unique<Hover>();
    next->spec = spec;
    next->fetch = std::move(fetch);
    next->done = std::move(done);
    if (m_inDrag) {
        // One drag at a time: the current one is cancelled, this one starts once it has ended.
        if (m_nextHover && m_nextHover->done)
            m_nextHover->done(false, false);
        m_nextHover = std::move(next);
        endHover(false);
        return;
    }
    m_hover = std::move(next);
    QTimer::singleShot(0, this, &FileDragAgent::runHover);
}

void FileDragAgent::endHover(bool drop)
{
    const auto state = drop ? HoverSource::State::Drop : HoverSource::State::Cancel;
    if (m_hover && m_hover->source) {
        m_hover->source->state = state;
        m_hover->lastFed = {LONG_MIN, LONG_MIN};
        feedDrag(); // the drag loop asks the source again on mouse input
    } else if (m_hover) {
        m_hover->requested = state;
    }
}

// The drag loop captures the mouse with a window of its thread, but only the foreground window
// gets mouse input that way: the loop would neither follow the cursor nor ask the source whether
// to drop. Mouse moves are therefore handed to that window.
void FileDragAgent::feedDrag()
{
    if (!m_hover || !m_hover->thread)
        return;
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(m_hover->thread, &info) || !info.hwndCapture)
        return;
    POINT p = {};
    GetCursorPos(&p);
    if (p.x == m_hover->lastFed.x && p.y == m_hover->lastFed.y)
        return;
    m_hover->lastFed = p;
    POINT client = p;
    ScreenToClient(info.hwndCapture, &client);
    PostMessageW(info.hwndCapture, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(client.x, client.y));
}

void FileDragAgent::runHover()
{
    if (!m_hover || m_inDrag)
        return;
    Hover *h = m_hover.get();
    if (h->requested != HoverSource::State::Dragging) { // ended before it began
        auto done = std::move(h->done);
        m_hover.reset();
        if (done)
            done(false, false);
        return;
    }
    // The drag loop blocks its thread and dispatches only what OLE needs: on the agent's main
    // thread the service could no longer say "drop" or "cancel". So it gets a thread of its own
    // (an OLE apartment, which the drop target calls into for the files).
    h->source = new HoverSource;
    m_inDrag = true;
    QTimer::singleShot(80, this, [] { wiggle(); }); // the drag loop then finds the target under the cursor
    m_feed.start();
    std::thread([this, h] {
        h->thread = GetCurrentThreadId();
        OleInitialize(nullptr);
        IDataObject *data = nullptr;
        if (h->spec.own) {
            data = shellDataObject(h->spec.paths);
        } else {
            data = createVirtualFiles(h->spec.files, [h](QString *error) {
                QStringList items = h->fetch ? h->fetch(error) : QStringList(); // blocking: the drop waits
                h->transferred = !items.isEmpty();
                return items;
            });
        }
        bool dropped = false;
        if (data) {
            const QString first = h->spec.own ? QFileInfo(h->spec.paths.value(0)).fileName()
                                              : h->spec.files.value(0).path.section(QLatin1Char('/'), 0, 0);
            const bool folder = h->spec.own ? QFileInfo(h->spec.paths.value(0)).isDir()
                                            : h->spec.files.value(0).path.contains(QLatin1Char('/'));
            setDragImage(data, first, folder, h->spec.own ? int(h->spec.paths.size()) : h->spec.count);
            DWORD effect = DROPEFFECT_NONE;
            const HRESULT hr = SHDoDragDrop(nullptr, data, h->source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
            // An optimised move (Explorer moving our own files itself) reports DROPEFFECT_NONE, so
            // the drop counts, not the effect; virtual files must also have been transferred.
            dropped = hr == DRAGDROP_S_DROP;
            qCInfo(lcDragAgent) << "Drag here ended:" << (dropped ? "dropped" : "not dropped") << "effect" << effect;
            data->Release();
        }
        OleUninitialize();
        QMetaObject::invokeMethod(this, [this, h, dropped] {
            m_inDrag = false;
            m_feed.stop();
            if (m_hover.get() != h)
                return;
            h->source->Release();
            h->source = nullptr;
            auto done = std::move(h->done);
            const bool transferred = h->transferred;
            m_hover.reset();
            if (done)
                done(dropped, transferred);
            if (m_nextHover) {
                m_hover = std::move(m_nextHover);
                QTimer::singleShot(0, this, &FileDragAgent::runHover);
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void FileDragAgent::catchFiles(QPoint at, std::function<void(const QStringList &)> done)
{
    if (m_catcher)
        finishCatch(); // an older one answers "nothing"
    m_catcher = std::make_unique<Catcher>();
    m_catcher->at = {at.x(), at.y()};
    m_catcher->done = std::move(done);

    // A small invisible (but hit-testable) window under the cursor, inside its monitor.
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromPoint(m_catcher->at, MONITOR_DEFAULTTONEAREST), &info);
    const int size = 48;
    const int x = qBound(int(info.rcMonitor.left), at.x() - size / 2, int(info.rcMonitor.right) - size);
    const int y = qBound(int(info.rcMonitor.top), at.y() - size / 2, int(info.rcMonitor.bottom) - size);
    m_catcher->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                                        kCatcherClass, L"", WS_POPUP, x, y, size, size, nullptr, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
    if (!m_catcher->window) {
        finishCatch();
        return;
    }
    SetLayeredWindowAttributes(m_catcher->window, 0, 1, LWA_ALPHA);
    Catcher *catcher = m_catcher.get();
    catcher->target = new CatchTarget([this, catcher](const QStringList &paths) {
        // Called from the drag loop: answer after it returns.
        catcher->paths = paths;
        QTimer::singleShot(0, this, [this, catcher] {
            if (m_catcher.get() != catcher)
                return;
            // End the drag here without a drop (Deskflower moves the files), then answer.
            releaseLeft();
            QTimer::singleShot(80, this, [this, catcher] {
                if (m_catcher.get() == catcher)
                    finishCatch();
            });
        });
    });
    if (FAILED(RegisterDragDrop(m_catcher->window, catcher->target))) {
        qCWarning(lcDragAgent) << "RegisterDragDrop failed";
        finishCatch();
        return;
    }
    ShowWindow(m_catcher->window, SW_SHOWNOACTIVATE);
    // Wiggle onto the window and back: the drag loop then asks it what it would accept.
    const POINT inside = {x + size / 2, y + size / 2};
    moveTo(inside);
    moveTo(m_catcher->at);
    m_catcher->timeout.setSingleShot(true);
    connect(&m_catcher->timeout, &QTimer::timeout, this, [this, catcher] {
        if (m_catcher.get() == catcher)
            finishCatch(); // nothing (not files): the crossing goes on as usual
    });
    m_catcher->timeout.start(kCatchTimeoutMs);
}

void FileDragAgent::finishCatch()
{
    std::unique_ptr<Catcher> catcher = std::move(m_catcher);
    if (catcher->window) {
        RevokeDragDrop(catcher->window);
        DestroyWindow(catcher->window);
    }
    if (catcher->target)
        catcher->target->Release();
    if (!catcher->paths.isEmpty())
        qCInfo(lcDragAgent) << "Caught a drag of" << catcher->paths.size() << "items";
    if (catcher->done)
        catcher->done(catcher->paths);
}

QString FileDragAgent::folderAt(QPoint at) const
{
    const POINT p = {at.x(), at.y()};
    HWND root = GetAncestor(WindowFromPoint(p), GA_ROOT);
    if (!root)
        return {};
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, 64);
    const QString name = QString::fromWCharArray(cls);
    if (name == QLatin1String("Progman") || name == QLatin1String("WorkerW")) {
        PWSTR desktop = nullptr;
        QString path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
            path = QString::fromWCharArray(desktop);
        CoTaskMemFree(desktop);
        return path;
    }
    if (name == QLatin1String("CabinetWClass"))
        return explorerFolder(root);
    return {};
}

bool FileDragAgent::recycle(const QStringList &paths, QString *error)
{
    std::wstring from;
    for (const QString &p : paths)
        from += QDir::toNativeSeparators(p).toStdWString() + L'\0';
    from += L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        *error = tr("error %1").arg(rc);
        return false;
    }
    return true;
}

void FileDragAgent::setBadge(const QString &text)
{
    if (text.isEmpty()) {
        m_follow.stop();
        m_badge.reset();
        return;
    }
    if (!m_badge) {
        m_badge = std::make_unique<QLabel>();
        m_badge->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        m_badge->setAttribute(Qt::WA_ShowWithoutActivating);
        m_badge->setStyleSheet(QStringLiteral(
            "QLabel { background: #263238; color: white; border: 1px solid #4db6ac; border-radius: 6px; padding: 6px 10px; }"));
    }
    m_badge->setText(QStringLiteral("📄 ") + text);
    m_badge->adjustSize();
    m_badge->move(QCursor::pos() + QPoint(18, 22));
    m_badge->show();
    m_follow.start();
}
