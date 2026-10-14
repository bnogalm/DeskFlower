#include "localscreens.h"

#include <QBuffer>
#include <QCborMap>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QScreen>

#include <algorithm>

#ifdef Q_OS_WIN
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qscreen_platform.h>
#include <windows.h>
#endif

#if defined(Q_OS_LINUX)
// Last: X11 macros clash with Qt names.
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#endif

namespace LocalScreens {

namespace {

#if defined(Q_OS_LINUX)
// X11: XRandR monitors in root window pixels, the space XTest and pointer grabs use. Matched to
// QScreen by name (both are the output/monitor name, e.g. "DP-1").
QRect x11MonitorGeometry(QScreen *screen)
{
    static Display *display = XOpenDisplay(nullptr);
    if (!display)
        return {};
    int count = 0;
    XRRMonitorInfo *monitors = XRRGetMonitors(display, DefaultRootWindow(display), True, &count);
    QRect result;
    for (int i = 0; i < count && result.isNull(); ++i) {
        char *name = XGetAtomName(display, monitors[i].name);
        if (name && QString::fromUtf8(name) == screen->name())
            result = QRect(monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height);
        if (name)
            XFree(name);
    }
    if (monitors)
        XRRFreeMonitors(monitors);
    return result;
}
#endif

} // namespace

QRect nativeGeometry(QScreen *screen)
{
#ifdef Q_OS_WIN
    if (auto *win = screen->nativeInterface<QNativeInterface::QWindowsScreen>()) {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(win->handle(), &info)) {
            const RECT &r = info.rcMonitor;
            return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
        }
    }
#endif
#ifdef Q_OS_MACOS
    // Quartz global display coordinates are in points.
    return screen->geometry();
#elif defined(Q_OS_LINUX)
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        const QRect g = x11MonitorGeometry(screen);
        if (!g.isNull())
            return g;
    }
    // Wayland: logical coordinates, the space libei's absolute pointer and the portals use.
    return screen->geometry();
#else
    // Qt keeps the native origin and scales the size by the device pixel ratio.
    const QRect g = screen->geometry();
    return QRect(g.topLeft(), (QSizeF(g.size()) * screen->devicePixelRatio()).toSize());
#endif
}

namespace {

QString displayName(QScreen *screen)
{
    // The manufacturer string is often a generic driver vendor; the model is more telling.
    const QString model = screen->model().trimmed();
    return model.isEmpty() ? screen->name() : model;
}

} // namespace

QString screenId(QScreen *screen)
{
    // Qt 6 reports the monitor's friendly name, which two identical monitors share.
    const QString name = screen->name().trimmed();
    const QList<QScreen *> screens = QGuiApplication::screens();
    const auto sameName = std::count_if(screens.begin(), screens.end(),
                                        [&](QScreen *s) { return s->name().trimmed() == name; });
    if (sameName <= 1)
        return name;
    return QStringLiteral("%1@%2,%3").arg(name).arg(screen->geometry().x()).arg(screen->geometry().y());
}

QCborArray describe()
{
    QCborArray list;
    QScreen *primary = QGuiApplication::primaryScreen();
    for (QScreen *screen : QGuiApplication::screens()) {
        const QRect g = nativeGeometry(screen);
        QCborMap m;
        m.insert(QStringLiteral("i"), screenId(screen));
        m.insert(QStringLiteral("n"), displayName(screen));
        m.insert(QStringLiteral("x"), g.x());
        m.insert(QStringLiteral("y"), g.y());
        m.insert(QStringLiteral("w"), g.width());
        m.insert(QStringLiteral("h"), g.height());
        m.insert(QStringLiteral("s"), screen->devicePixelRatio());
        m.insert(QStringLiteral("p"), screen == primary);
        list.append(m);
    }
    return list;
}

QByteArray captureThumbnail(QScreen *screen, int maxWidth, int maxHeight)
{
    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull())
        return {};
    const QImage image =
        shot.toImage().scaled(maxWidth, maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 70);
    return jpeg;
}

} // namespace LocalScreens
