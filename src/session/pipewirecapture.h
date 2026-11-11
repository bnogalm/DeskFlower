#pragma once

// Frames of the monitors the portal's ScreenCast shares (PortalSession::streams), received from
// PipeWire on its own thread (pw_thread_loop), for the remote view (a ScreenGrabber) and the
// thumbnails. While nobody watches, the newest buffer of each monitor is only kept, and copied
// when asked for; while the remote view streams, every frame is copied as it arrives. Raw video
// only (BGRx/BGRA/RGBx/RGBA in shared memory buffers), which every portal implementation offers.

#include <QImage>
#include <QList>
#include <QRect>

#include <condition_variable>
#include <memory>
#include <mutex>

class PipeWireCapture
{
public:
    struct Source {
        quint32 node = 0;
        QRect geometry; // logical, as LocalScreens::nativeGeometry on Wayland
    };

    // The helper's capture, connected once the portal session started (null before or without).
    static PipeWireCapture *instance();
    static void setInstance(std::unique_ptr<PipeWireCapture> capture);

    // Connects to the PipeWire remote `fd` (taken over) and follows `sources`. False on failure.
    static std::unique_ptr<PipeWireCapture> connect(int fd, const QList<Source> &sources);
    ~PipeWireCapture();

    // The latest frame of the monitor at `screen` (empty: the first one) once it is newer than
    // `*serial`, waiting up to waitMs for one; updates *serial. False: nothing newer (or no such
    // monitor). waitMs > 0 (a stream of frames) also has the next ones copied as they arrive for
    // a moment.
    bool frame(const QRect &screen, QImage *image, quint64 *serial, int waitMs);
    QList<QRect> screens() const;

    struct Private;

private:
    PipeWireCapture() = default;
    std::unique_ptr<Private> d;
};
