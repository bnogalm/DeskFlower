#include "inputbackend.h"
#include "linuxbackends.h"
#include "stubinputbackend.h"

#include <QGuiApplication>

// Wayland sessions need the portals (InputCapture + RemoteDesktop) and libei: an X11 client under
// XWayland cannot see or inject global input. Plain X11 sessions use XInput2 + XTest.
InputBackend *InputBackend::create(QObject *parent)
{
    const QByteArray session = qgetenv("XDG_SESSION_TYPE").toLower();
    const bool wayland = session == "wayland" || (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && session != "x11");
    if (wayland) {
#ifdef DESKFLOWER_HAVE_LIBEI
        return createWaylandInputBackend(parent);
#else
        return new StubInputBackend("this build has no Wayland support (libei and liboeffis were not found)", parent);
#endif
    }
    if (!qEnvironmentVariableIsEmpty("DISPLAY"))
        return createX11InputBackend(parent);
    return new StubInputBackend("no X11 or Wayland display", parent);
}
