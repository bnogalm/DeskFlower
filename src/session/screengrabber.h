#pragma once

#include <QImage>
#include <QRect>
#include <memory>

// Captures one screen (the primary one unless told otherwise) from a worker thread. Only Windows has a native implementation
// (DXGI Desktop Duplication, GDI fallback); elsewhere create() returns nullptr and the streamer
// grabs through QScreen on the GUI thread.
class ScreenGrabber
{
public:
    virtual ~ScreenGrabber() = default;

    enum class Result { Changed, Unchanged, Failed };

    // Fills `image` with the current content (Changed). Unchanged: nothing changed within the wait,
    // the previous image is still valid. Failed: no image (locked or protected desktop...).
    virtual Result grab(QImage *image, int waitMs) = 0;
    // The next grab returns a complete image, even if the screen did not change (a new stream).
    virtual void reset() = 0;
    // The screen to capture, in native desktop coordinates; empty: the primary screen. The next
    // grab returns a complete image of it.
    virtual void setScreen(const QRect &screen) = 0;

    static std::unique_ptr<ScreenGrabber> create();
};
