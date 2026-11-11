#pragma once

#include <memory>

class InputBackend;
class QObject;
class ScreenGrabber;

// Linux display-server specific backends (selected in inputbackend_linux.cpp).

// A Wayland session (the helper then needs the portals: XWayland sees neither global input nor
// the native windows' clipboard and screens).
bool isWaylandSession();

struct X11BackendOptions {
    // Treat input from XTest devices as the user's own (tests: under Xvfb every event is XTest).
    // Normally XTest input is what this backend injects, and never counts as local activity.
    bool xtestIsLocal = false;
};
InputBackend *createX11InputBackend(QObject *parent, X11BackendOptions options = {});
#ifdef DESKFLOWER_HAVE_LIBEI
InputBackend *createWaylandInputBackend(QObject *parent);
#endif

// The X11 (XShm) screen grabber, whatever the session (tests).
std::unique_ptr<ScreenGrabber> createX11ScreenGrabber();
