#pragma once

// Geometry of the shared plane: decides where the cursor goes when it leaves a device's screens.
//
// Every device's screens are in that device's native desktop coordinates; a pack offset (from the
// workspace layout) places them in the shared plane. Crossing happens between screens of different
// devices whose edges touch (or are separated by a small gap) at the crossing point.

#include "workspace.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QPoint>
#include <QRect>
#include <optional>

namespace df {

enum class Side { Left = 0, Right = 1, Top = 2, Bottom = 3 };

struct Crossing {
    QByteArray device;
    QPoint point; // in the target device's native coordinates
};

// A stretch of a screen's outer edge behind which another device lies: pushing the cursor past
// `side` of `screen` at an along-edge coordinate (y for Left/Right, x for Top/Bottom) between
// `from` and `to` (inclusive, in the screen's own coordinates) would cross into `device`.
struct EdgeSpan {
    QRect screen;
    Side side = Side::Right;
    int from = 0;
    int to = 0;
    QByteArray device;
};

class Router
{
public:
    // Packs further apart than this (in layout units) are not considered neighbours.
    static constexpr int kMaxGap = 64;

    void set(const QHash<QByteArray, QList<ScreenInfo>> &screens, const QMap<QByteArray, QPoint> &offsets);

    bool contains(const QByteArray &device, QPoint p) const;
    // Clamps `to` into the screen of `device` that contains `from` (or the nearest one).
    QPoint clamp(const QByteArray &device, QPoint from, QPoint to) const;
    // Where a cursor at `p` on `device` pushing past `side` ends up, if another device is there.
    std::optional<Crossing> cross(const QByteArray &device, QPoint p, Side side) const;
    // Every stretch of the device's screen edges where cross() would find another device (the
    // same gap rule), one span per neighbouring screen; where two neighbours overlap along an
    // edge, the stretch goes to the one cross() would pick (the nearer). Pure geometry: whether
    // that device is connected or willing is for the caller to decide.
    QList<EdgeSpan> neighbourSpans(const QByteArray &device) const;
    // Whether the cursor can go from `from` to `to` through screen edges (directly or across other
    // devices), by the same rule as cross: screens of two devices that overlap along an edge and
    // are at most kMaxGap apart across it. False if either device has no screens.
    bool canReach(const QByteArray &from, const QByteArray &to) const;
    // Centre of the device's primary screen (fallback position).
    QPoint primaryCenter(const QByteArray &device) const;
    // The device's primary screen (local coordinates), or an empty rect if unknown.
    QRect primaryRect(const QByteArray &device) const;
    // The device's screen containing p (local coordinates), or an empty rect.
    QRect screenAt(const QByteArray &device, QPoint p) const;
    // The device's screens (local coordinates).
    QList<QRect> screens(const QByteArray &device) const;

private:
    struct Screen {
        QByteArray device;
        QRect local;
        QRect global;
        bool primary = false;
    };
    QList<Screen> m_screens;
};

} // namespace df
