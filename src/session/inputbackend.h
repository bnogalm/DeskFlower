#pragma once

// Platform input capture and injection, running inside the user's session.
// Coordinates are native desktop coordinates (Windows: physical pixels).
// Keys are PS/2 set-1 scancodes (0xE0xx for extended keys). Buttons: 1 left, 2 right, 3 middle,
// 4 back (X1), 5 forward (X2). Wheel deltas use 120 units per notch.

#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>

class InputBackend : public QObject
{
    Q_OBJECT
public:
    // A stretch of an outer edge of `screen` (side 0 left, 1 right, 2 top, 3 bottom; along-edge
    // coordinate between `from` and `to`, inclusive) behind which the service knows a device that
    // can take the input: see setEdgeMap.
    struct EdgeSpan {
        QRect screen;
        int side = 0;
        int from = 0;
        int to = 0;
    };

    static InputBackend *create(QObject *parent = nullptr);

    using QObject::QObject;
    ~InputBackend() override = default;

    virtual bool start() = 0;
    virtual bool isCapturing() const = 0;

    // Report the cursor pushing against an outer edge of the local desktop (master only).
    virtual void setReportEdges(bool report) = 0;
    // Master only: where a push past an edge is known to cross (the service keeps it current).
    // Within a span the backend may capture on its own as soon as the cursor touches the edge
    // (edgeHit with captured = true) instead of waiting for the service's answer, so that the
    // motion meanwhile is not lost: the service then confirms (setCapture(true), a no-op) or
    // declines (setCapture(false, edge point)). An empty map (the default) never captures early.
    virtual void setEdgeMap(const QList<EdgeSpan> &spans) { Q_UNUSED(spans) }
    // Capturing: local input is swallowed and reported as relative motion, buttons, wheel and keys.
    // Leaving capture warps the local cursor to `returnTo`; guardSide >= 0 is the edge the cursor
    // came back through, where an early capture is not attempted again for a moment.
    virtual void setCapture(bool on, QPoint returnTo, int guardSide = -1) = 0;
    // Hides the local cursor while another device has the pointer (devices other than the master).
    // Local mouse input shows it again. Platforms without support ignore it.
    virtual void setCursorHidden(bool hidden) { Q_UNUSED(hidden) }
    // File drag (master): whether this computer's own left button is held, and reporting its
    // release (localRelease) while not capturing. Platforms without support say no.
    virtual bool leftButtonHeld() const { return false; }
    virtual void setReportReleases(bool on) { Q_UNUSED(on) }
    // Why keyboard and mouse sharing (or the helper's screen capture) does not (fully) work here,
    // for the user: a refused portal, no display, a missing permission, a system state that hides
    // the keyboard from it... Empty when fine. problemLink: a system settings page that fixes it
    // (empty: none). problemChanged() whenever either changes.
    virtual QString problem() const { return {}; }
    virtual QString problemLink() const { return {}; }
    // Whether localPasteGesture() is reported here (sent to the service as localPressSupport).
    // Backends that see no local-only input (Wayland) or not yet (macOS without its event tap) say
    // no; a change comes with problemChanged().
    virtual bool reportsLocalPresses() const { return false; }

#ifdef DESKFLOWER_KEY_DIAGNOSTICS
    // Keyboard diagnostics (df::KeyDiagnostics, "keyDiag {on}" from the service): while on, the
    // backend describes what its keyboard hook sees of the keys that go to or come from another
    // device and each key it injects (diagnostic(), one line each), and where it runs once at the
    // start. Never anything typed for this device itself. Backends without it say so once.
    // Only built with DESKFLOWER_KEY_DIAGNOSTICS.
    virtual void setKeyDiagnostics(bool on)
    {
        if (on)
            emit diagnostic(QStringLiteral("this platform's input backend records no key details"));
    }
#endif

    virtual void moveTo(QPoint p) = 0;
    virtual void button(int button, bool down, QPoint at) = 0;
    virtual void wheel(int dx, int dy) = 0;
    virtual void key(quint32 scancode, bool down) = 0;
    // Releases every key and button injected so far.
    virtual void releaseAll() = 0;

signals:
    // The cursor pushes past an outer edge at `at`; captured: the backend already captures
    // (see setEdgeMap), and the motion that follows is reported as moved().
    void edgeHit(int side, QPoint at, bool captured);
    void moved(int dx, int dy);
    void buttonEvent(int button, bool down);
    void wheelEvent(int dx, int dy);
    void keyEvent(quint32 scancode, bool down);
    void localRelease(QPoint at);
    // A press on this device's own keyboard or mouse while not capturing that can be the user
    // asking for a paste: a paste chord or gesture key (keymap::isPasteEvidenceKey, judged against
    // the local keys already held) or any button press. Never motion or the wheel, never input
    // injected by the helper itself. Paste evidence for file data (the service's localPress).
    void localPasteGesture();
    // Capture ended on its own (the platform took the input hooks away and they could not be put
    // back): the keyboard and mouse are local again, the cursor at `at`.
    void captureLost(QPoint at);
    void problemChanged();
    void diagnostic(const QString &line);
};
