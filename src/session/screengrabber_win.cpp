// Screen capture on Windows (the primary monitor, or the one chosen with setScreen).
//  - DXGI Desktop Duplication: the GPU hands over a new frame only when something changed; frames
//    are copied to a CPU-readable staging texture. Not available on the secure desktop (UAC, lock
//    screen) or with some drivers.
//  - GDI BitBlt as a fallback; DXGI is tried again every few seconds (e.g. after the lock screen).
// The mouse cursor is not part of the capture: the viewer draws it.

#include "screengrabber.h"

#include <QLoggingCategory>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(lcGrab, "deskflower.grab")

namespace {

// The monitor showing `screen` (native coordinates), or the primary one.
HMONITOR monitorFor(const QRect &screen)
{
    if (screen.isEmpty())
        return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const QPoint c = screen.center();
    return MonitorFromPoint({c.x(), c.y()}, MONITOR_DEFAULTTOPRIMARY);
}

RECT monitorRect(HMONITOR monitor)
{
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    return info.rcMonitor;
}

class GdiGrabber
{
public:
    bool grab(QImage *image, HMONITOR monitor)
    {
        const RECT r = monitorRect(monitor);
        const int w = r.right - r.left, h = r.bottom - r.top;
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = false;
        if (bitmap) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            ok = BitBlt(memory, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
            SelectObject(memory, old);
            if (ok) {
                QImage img(w, h, QImage::Format_RGB32);
                for (int y = 0; y < h; ++y)
                    memcpy(img.scanLine(y), static_cast<const uchar *>(bits) + size_t(y) * size_t(w) * 4, size_t(w) * 4);
                *image = img;
            }
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return ok;
    }
};

class DxgiGrabber : public ScreenGrabber
{
public:
    Result grab(QImage *image, int waitMs) override
    {
        if (!m_duplication) {
            const ULONGLONG now = GetTickCount64();
            if (now < m_retryDxgiAt)
                return gdiGrab(image, waitMs);
            if (!init()) {
                if (!m_dxgiFailed)
                    qCInfo(lcGrab) << "DXGI Desktop Duplication not available, using GDI";
                m_dxgiFailed = true;
                m_retryDxgiAt = now + kDxgiRetryMs;
                return gdiGrab(image, waitMs);
            }
            m_dxgiFailed = false;
        }

        DXGI_OUTDUPL_FRAME_INFO info = {};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = m_duplication->AcquireNextFrame(UINT(waitMs), &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return m_first ? firstGrab(image) : Result::Unchanged;
        if (hr == DXGI_ERROR_ACCESS_LOST || FAILED(hr)) {
            // Desktop switch (UAC, lock screen), mode change...: start again next time.
            m_duplication.Reset();
            return gdiGrab(image, 0);
        }
        if (m_first && info.LastPresentTime.QuadPart == 0) {
            // The first duplicated frame is often empty (black) until something is presented.
            m_duplication->ReleaseFrame();
            return firstGrab(image);
        }
        if (info.LastPresentTime.QuadPart == 0) { // only the pointer moved
            m_duplication->ReleaseFrame();
            return Result::Unchanged;
        }
        bool updated = false;
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture)))
            updated = copy(texture.Get(), image);
        m_duplication->ReleaseFrame();
        if (updated) {
            qCDebug(lcGrab) << "DXGI frame" << image->size() << "first:" << m_first << "present:"
                            << info.LastPresentTime.QuadPart << "sample:" << Qt::hex << image->pixel(image->width() / 2, image->height() / 2);
            m_first = false;
        }
        return updated ? Result::Changed : Result::Failed;
    }

    void reset() override { m_first = true; }

    void setScreen(const QRect &screen) override
    {
        if (screen == m_screen)
            return;
        m_screen = screen;
        m_duplication.Reset(); // duplicates one output: set up again for the new one
        m_first = true;
    }

private:
    // The initial image of a stream, with GDI; DXGI then reports only real changes.
    Result firstGrab(QImage *image)
    {
        const Result r = gdiGrab(image, 0);
        if (r == Result::Changed)
            m_first = false;
        return r;
    }

    Result gdiGrab(QImage *image, int waitMs)
    {
        if (waitMs > 0)
            Sleep(DWORD(qMin(waitMs, 50))); // GDI cannot wait for changes: pace a little
        const bool ok = m_gdi.grab(image, monitorFor(m_screen));
        qCDebug(lcGrab) << "GDI frame" << ok << image->size() << "sample:" << Qt::hex
                        << (ok ? image->pixel(image->width() / 2, image->height() / 2) : 0u);
        return ok ? Result::Changed : Result::Failed;
    }

    bool init()
    {
        const HMONITOR target = monitorFor(m_screen);
        // A new device below: the staging texture of the previous one cannot be reused.
        m_staging.Reset();
        m_stagingSize = QSize();
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        // The monitor may hang off any adapter (hybrid graphics): find it.
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc = {};
                output->GetDesc(&desc);
                if (desc.Monitor != target)
                    continue;
                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output.As(&output1)))
                    return false;
                if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &m_device, nullptr, &m_context)))
                    return false;
                if (FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication)))
                    return false;
                m_first = true;
                return true;
            }
        }
        return false;
    }

    bool copy(ID3D11Texture2D *texture, QImage *image)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            return false;
        if (!m_staging || m_stagingSize != QSize(int(desc.Width), int(desc.Height))) {
            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.BindFlags = 0;
            staging.MiscFlags = 0;
            staging.MipLevels = 1;
            staging.ArraySize = 1;
            m_staging.Reset();
            if (FAILED(m_device->CreateTexture2D(&staging, nullptr, &m_staging)))
                return false;
            m_stagingSize = QSize(int(desc.Width), int(desc.Height));
        }
        m_context->CopyResource(m_staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        QImage img(m_stagingSize, QImage::Format_RGB32);
        for (int y = 0; y < img.height(); ++y)
            memcpy(img.scanLine(y), static_cast<const uchar *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                   size_t(img.width()) * 4);
        m_context->Unmap(m_staging.Get(), 0);
        *image = img;
        return true;
    }

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D> m_staging;
    QSize m_stagingSize;
    GdiGrabber m_gdi;
    QRect m_screen; // empty: the primary monitor
    static constexpr ULONGLONG kDxgiRetryMs = 5000;
    ULONGLONG m_retryDxgiAt = 0;
    bool m_dxgiFailed = false;
    bool m_first = true;
};

} // namespace

std::unique_ptr<ScreenGrabber> ScreenGrabber::create() { return std::make_unique<DxgiGrabber>(); }
