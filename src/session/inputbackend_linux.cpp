#include "inputbackend.h"
#include "linuxbackends.h"
#include "stubinputbackend.h"

#include <QCoreApplication>

bool isWaylandSession()
{
    const QByteArray session = qgetenv("XDG_SESSION_TYPE").toLower();
    return session == "wayland" || (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && session != "x11");
}

// Wayland sessions need the portals (InputCapture + RemoteDesktop) and libei: an X11 client under
// XWayland cannot see or inject global input. Plain X11 sessions use XInput2 + XTest.
InputBackend *InputBackend::create(QObject *parent)
{
    if (isWaylandSession()) {
#ifdef DESKFLOWER_HAVE_LIBEI
        return createWaylandInputBackend(parent);
#else
        return new StubInputBackend(QCoreApplication::translate(
                                        "InputBackend", "This build of DeskFlower has no Wayland support (libei was not found)"),
                                    parent);
#endif
    }
    if (!qEnvironmentVariableIsEmpty("DISPLAY"))
        return createX11InputBackend(parent);
    return new StubInputBackend(
        QCoreApplication::translate("InputBackend", "The session helper found no X11 or Wayland display"), parent);
}
