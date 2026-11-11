#pragma once

#include <QImage>
#include <QRect>
#include <memory>

// Captures one screen (the primary one unless told otherwise) from a worker thread. Native
// implementations: Windows (DXGI Desktop Duplication, GDI fallback) and macOS 12.3+
// (ScreenCaptureKit); elsewhere create() returns nullptr and the streamer grabs through QScreen on
// the GUI thread.
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

#ifdef Q_OS_MACOS
// One image of the screen with these bounds (global points), through ScreenCaptureKit (macOS 14+):
// null elsewhere or on failure (the caller falls back to QScreen::grabWindow).
QImage captureScreenImage(const QRect &screen);
#endif
