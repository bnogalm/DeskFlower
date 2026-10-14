#pragma once

#include <QByteArray>
#include <QCborArray>
#include <QList>
#include <QRect>
#include <QString>

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

} // namespace LocalScreens
