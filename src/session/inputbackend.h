#pragma once

// Platform input capture and injection, running inside the user's session.
// Coordinates are native desktop coordinates (Windows: physical pixels).
// Keys are PS/2 set-1 scancodes (0xE0xx for extended keys). Buttons: 1 left, 2 right, 3 middle,
// 4 back (X1), 5 forward (X2). Wheel deltas use 120 units per notch.

#include <QObject>
#include <QPoint>

class InputBackend : public QObject
{
    Q_OBJECT
public:
    static InputBackend *create(QObject *parent = nullptr);

    using QObject::QObject;
    ~InputBackend() override = default;

    virtual bool start() = 0;
    virtual bool isCapturing() const = 0;

    // Report the cursor pushing against an outer edge of the local desktop (master only).
    virtual void setReportEdges(bool report) = 0;
    // Capturing: local input is swallowed and reported as relative motion, buttons, wheel and keys.
    // Leaving capture warps the local cursor to `returnTo`.
    virtual void setCapture(bool on, QPoint returnTo) = 0;
    // Hides the local cursor while another device has the pointer (devices other than the master).
    // Local mouse input shows it again. Platforms without support ignore it.
    virtual void setCursorHidden(bool hidden) { Q_UNUSED(hidden) }
    // File drag (master): whether this computer's own left button is held, and reporting its
    // release (localRelease) while not capturing. Platforms without support say no.
    virtual bool leftButtonHeld() const { return false; }
    virtual void setReportReleases(bool on) { Q_UNUSED(on) }

    virtual void moveTo(QPoint p) = 0;
    virtual void button(int button, bool down, QPoint at) = 0;
    virtual void wheel(int dx, int dy) = 0;
    virtual void key(quint32 scancode, bool down) = 0;
    // Releases every key and button injected so far.
    virtual void releaseAll() = 0;

signals:
    void edgeHit(int side, QPoint at);
    void moved(int dx, int dy);
    void buttonEvent(int button, bool down);
    void wheelEvent(int dx, int dy);
    void keyEvent(quint32 scancode, bool down);
    void localRelease(QPoint at);
};
