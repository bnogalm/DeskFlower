// Linux screen capture for the remote view, on the streamer's worker thread:
//  - X11: XShm (MIT-SHM) on a connection of its own, plain XGetImage without it (remote X);
//  - Wayland: the ScreenCast portal's PipeWire streams (PipeWireCapture), when built with
//    PipeWire; without it the streamer falls back to QScreen, which Wayland does not serve.

#include "screengrabber.h"
#include "linuxbackends.h"

#ifdef DESKFLOWER_HAVE_PIPEWIRE
#include "pipewirecapture.h"
#endif

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QThread>

#include <cstring>
#include <mutex>

#include <sys/ipc.h>
#include <sys/shm.h>

// X11 headers last: they define macros (None, Bool, Status...) that clash with Qt.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>

Q_LOGGING_CATEGORY(lcGrab, "deskflower.view.capture")

namespace {

constexpr int kPollMs = 20; // while waiting for a change

class X11Grabber final : public ScreenGrabber
{
public:
    ~X11Grabber() override
    {
        releaseImage();
        if (m_display)
            XCloseDisplay(m_display);
    }

    Result grab(QImage *image, int waitMs) override
    {
        if (!ensureDisplay())
            return Result::Failed;
        QElapsedTimer waited;
        waited.start();
        for (;;) {
            if (!capture())
                return Result::Failed;
            const qsizetype bytes = qsizetype(m_image->bytes_per_line) * m_image->height;
            const bool same = !m_full && m_previous.size() == bytes
                              && std::memcmp(m_previous.constData(), m_image->data, size_t(bytes)) == 0;
            if (!same) {
                m_previous = QByteArray(m_image->data, bytes);
                m_full = false;
                *image = QImage(reinterpret_cast<const uchar *>(m_previous.constData()), m_image->width,
                                m_image->height, m_image->bytes_per_line, QImage::Format_RGB32)
                             .copy();
                if (!m_shm)
                    releaseImage(); // XGetImage makes a new one every time
                return Result::Changed;
            }
            if (!m_shm)
                releaseImage();
            if (waited.elapsed() + kPollMs > waitMs)
                return Result::Unchanged;
            QThread::msleep(kPollMs);
        }
    }

    void reset() override { m_full = true; }

    void setScreen(const QRect &screen) override
    {
        m_screen = screen;
        m_full = true;
        releaseImage(); // another size
    }

private:
    bool ensureDisplay()
    {
        if (m_display)
            return true;
        if (m_failed)
            return false;
        // Xlib's default error handler ends the process: an error here (a monitor that went away
        // under a request) must only fail the grab.
        static std::once_flag handler;
        std::call_once(handler, [] { XSetErrorHandler([](Display *, XErrorEvent *) { return 0; }); });
        m_display = XOpenDisplay(nullptr);
        if (!m_display) {
            m_failed = true;
            qCWarning(lcGrab) << "Cannot open the X display for screen capture";
            return false;
        }
        int major = 0, minor = 0;
        Bool pixmaps = False;
        m_canShm = XShmQueryVersion(m_display, &major, &minor, &pixmaps);
        return true;
    }

    QRect target() const
    {
        const Window root = DefaultRootWindow(m_display);
        const int screen = DefaultScreen(m_display);
        const QRect desktop(0, 0, DisplayWidth(m_display, screen), DisplayHeight(m_display, screen));
        QRect r = m_screen;
        if (r.isEmpty()) {
            // The primary monitor (the whole desktop without XRandR monitors).
            int count = 0;
            XRRMonitorInfo *monitors = XRRGetMonitors(m_display, root, True, &count);
            for (int i = 0; i < count; ++i) {
                if (monitors[i].primary || r.isEmpty())
                    r = QRect(monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height);
            }
            if (monitors)
                XRRFreeMonitors(monitors);
        }
        return r.isEmpty() ? desktop : r.intersected(desktop);
    }

    bool capture()
    {
        const QRect r = target();
        if (r.isEmpty())
            return false;
        const Window root = DefaultRootWindow(m_display);
        if (m_image && (m_image->width != r.width() || m_image->height != r.height()))
            releaseImage();
        if (!m_image && m_canShm)
            createShmImage(r.size());
        if (m_shm && !XShmGetImage(m_display, root, m_image, r.x(), r.y(), AllPlanes)) {
            // XShmAttach reports nothing when the server refuses the segment (a remote X server:
            // the error is asynchronous): the first XShmGetImage fails instead. If plain
            // XGetImage works where it failed, MIT-SHM is not usable here: never tried again.
            releaseImage();
            m_image = XGetImage(m_display, root, r.x(), r.y(), unsigned(r.width()), unsigned(r.height()), AllPlanes,
                                ZPixmap);
            if (!m_image)
                return false;
            qCInfo(lcGrab) << "MIT-SHM is not usable on this X display: plain XGetImage";
            m_canShm = false;
        } else if (!m_shm) {
            m_image = XGetImage(m_display, root, r.x(), r.y(), unsigned(r.width()), unsigned(r.height()), AllPlanes,
                                ZPixmap);
            if (!m_image)
                return false;
        }
        // 24/32-bit TrueColor with the usual masks: the bytes are Qt's RGB32 as they are.
        if (m_image->bits_per_pixel != 32 || m_image->red_mask != 0xff0000 || m_image->blue_mask != 0xff) {
            qCWarning(lcGrab) << "Unsupported X visual for screen capture:" << m_image->bits_per_pixel << "bpp";
            releaseImage();
            m_failed = true;
            return false;
        }
        return true;
    }

    void createShmImage(QSize size)
    {
        const int screen = DefaultScreen(m_display);
        m_image = XShmCreateImage(m_display, DefaultVisual(m_display, screen), unsigned(DefaultDepth(m_display, screen)),
                                  ZPixmap, nullptr, &m_shmInfo, unsigned(size.width()), unsigned(size.height()));
        if (!m_image)
            return;
        m_shmInfo.shmid = shmget(IPC_PRIVATE, size_t(m_image->bytes_per_line) * size_t(m_image->height), IPC_CREAT | 0600);
        if (m_shmInfo.shmid < 0) {
            XDestroyImage(m_image);
            m_image = nullptr;
            m_canShm = false;
            return;
        }
        m_shmInfo.shmaddr = m_image->data = static_cast<char *>(shmat(m_shmInfo.shmid, nullptr, 0));
        m_shmInfo.readOnly = False;
        const bool attached = m_shmInfo.shmaddr != reinterpret_cast<char *>(-1) && XShmAttach(m_display, &m_shmInfo);
        XSync(m_display, False);
        shmctl(m_shmInfo.shmid, IPC_RMID, nullptr); // freed once both sides detach
        if (!attached) {
            if (m_shmInfo.shmaddr != reinterpret_cast<char *>(-1))
                shmdt(m_shmInfo.shmaddr);
            m_image->data = nullptr;
            XDestroyImage(m_image);
            m_image = nullptr;
            m_canShm = false; // no shared memory here: plain XGetImage
            return;
        }
        m_shm = true;
    }

    void releaseImage()
    {
        if (!m_image)
            return;
        if (m_shm) {
            XShmDetach(m_display, &m_shmInfo);
            shmdt(m_shmInfo.shmaddr);
            m_image->data = nullptr;
            m_shm = false;
        }
        XDestroyImage(m_image);
        m_image = nullptr;
    }

    Display *m_display = nullptr;
    bool m_failed = false;
    bool m_canShm = false;
    bool m_shm = false;
    XShmSegmentInfo m_shmInfo{};
    XImage *m_image = nullptr;
    QRect m_screen;
    bool m_full = true;
    QByteArray m_previous;
};

#ifdef DESKFLOWER_HAVE_PIPEWIRE
class PipeWireGrabber final : public ScreenGrabber
{
public:
    Result grab(QImage *image, int waitMs) override
    {
        PipeWireCapture *capture = PipeWireCapture::instance();
        if (!capture)
            return Result::Failed; // the portal did not share the screens (yet)
        quint64 serial = m_full ? 0 : m_serial;
        if (capture->frame(m_screen, image, &serial, waitMs)) {
            m_serial = serial;
            m_full = false;
            return Result::Changed;
        }
        return m_serial ? Result::Unchanged : Result::Failed;
    }
    void reset() override { m_full = true; }
    void setScreen(const QRect &screen) override
    {
        m_screen = screen;
        m_full = true;
    }

private:
    QRect m_screen;
    quint64 m_serial = 0;
    bool m_full = true;
};
#endif

} // namespace

std::unique_ptr<ScreenGrabber> ScreenGrabber::create()
{
    if (isWaylandSession()) {
#ifdef DESKFLOWER_HAVE_PIPEWIRE
        return std::make_unique<PipeWireGrabber>();
#else
        return nullptr;
#endif
    }
    if (!qEnvironmentVariableIsEmpty("DISPLAY"))
        return std::make_unique<X11Grabber>();
    return nullptr;
}

std::unique_ptr<ScreenGrabber> createX11ScreenGrabber()
{
    return std::make_unique<X11Grabber>();
}
