#pragma once

#include <QByteArray>
#include <QCborArray>
#include <QList>
#include <QRect>
#include <QImage>
#include <QString>

#include <functional>

class QScreen;

namespace LocalScreens {

// Monitors of this session in native desktop coordinates (the ones the input system uses):
// physical pixels on Windows, points on macOS, pixels on X11/Wayland.
QCborArray describe();

// A screen's geometry in native desktop coordinates.
QRect nativeGeometry(QScreen *screen);

// Stable identifier of a screen within this device.
QString screenId(QScreen *screen);

// Small JPEG capture of a screen's content, at most maxWidth x maxHeight.
QByteArray captureThumbnail(QScreen *screen, int maxWidth = 480, int maxHeight = 300);

// Where screen images come from instead of QScreen::grabWindow (Wayland: the portal's screen
// cast, which a background program needs): the image of the screen with that native geometry,
// null if there is none (yet).
void setCaptureSource(std::function<QImage(const QRect &nativeGeometry)> source);

} // namespace LocalScreens
