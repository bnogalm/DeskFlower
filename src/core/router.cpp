#include "router.h"

#include <QSet>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace df {

void Router::set(const QHash<QByteArray, QList<ScreenInfo>> &screens, const QMap<QByteArray, QPoint> &offsets)
{
    m_screens.clear();
    for (auto it = screens.constBegin(); it != screens.constEnd(); ++it) {
        if (!offsets.contains(it.key()))
            continue;
        const QPoint offset = offsets.value(it.key());
        for (const ScreenInfo &s : it.value())
            m_screens.append(Screen{it.key(), s.geometry, s.geometry.translated(offset), s.primary});
    }
}

bool Router::contains(const QByteArray &device, QPoint p) const
{
    for (const Screen &s : m_screens) {
        if (s.device == device && s.local.contains(p))
            return true;
    }
    return false;
}

QPoint Router::clamp(const QByteArray &device, QPoint from, QPoint to) const
{
    const Screen *best = nullptr;
    qint64 bestDistance = LLONG_MAX;
    for (const Screen &s : m_screens) {
        if (s.device != device)
            continue;
        if (s.local.contains(from)) {
            best = &s;
            break;
        }
        const qint64 dx = qMax(0, qMax(s.local.left() - from.x(), from.x() - s.local.right()));
        const qint64 dy = qMax(0, qMax(s.local.top() - from.y(), from.y() - s.local.bottom()));
        if (dx * dx + dy * dy < bestDistance) {
            bestDistance = dx * dx + dy * dy;
            best = &s;
        }
    }
    if (!best)
        return from;
    return QPoint(qBound(best->local.left(), to.x(), best->local.right()),
                  qBound(best->local.top(), to.y(), best->local.bottom()));
}

std::optional<Crossing> Router::cross(const QByteArray &device, QPoint p, Side side) const
{
    // Global position of the cursor.
    QPoint g;
    bool found = false;
    for (const Screen &s : m_screens) {
        if (s.device == device && s.local.contains(p)) {
            g = p + (s.global.topLeft() - s.local.topLeft());
            found = true;
            break;
        }
    }
    if (!found)
        return std::nullopt;

    const bool horizontal = side == Side::Left || side == Side::Right;
    const Screen *best = nullptr;
    int bestGap = INT_MAX;
    for (const Screen &s : m_screens) {
        if (s.device == device)
            continue;
        const QRect &r = s.global;
        // The target must span the cursor along the edge...
        if (horizontal ? (g.y() < r.top() || g.y() > r.bottom()) : (g.x() < r.left() || g.x() > r.right()))
            continue;
        // ...and start just beyond it. Negative gaps allow slightly overlapping packs.
        int gap = 0;
        switch (side) {
        case Side::Right: gap = r.left() - (g.x() + 1); break;
        case Side::Left: gap = (g.x() - 1) - r.right(); break;
        case Side::Bottom: gap = r.top() - (g.y() + 1); break;
        case Side::Top: gap = (g.y() - 1) - r.bottom(); break;
        }
        if (gap > kMaxGap || gap < -kMaxGap)
            continue;
        if (std::abs(gap) < bestGap) {
            bestGap = std::abs(gap);
            best = &s;
        }
    }
    if (!best)
        return std::nullopt;

    // Enter on the target's facing edge, keeping the coordinate along the edge.
    const QRect &r = best->global;
    QPoint entry = g;
    switch (side) {
    case Side::Right: entry.setX(r.left()); break;
    case Side::Left: entry.setX(r.right()); break;
    case Side::Bottom: entry.setY(r.top()); break;
    case Side::Top: entry.setY(r.bottom()); break;
    }
    return Crossing{best->device, entry - (best->global.topLeft() - best->local.topLeft())};
}

namespace {
// Whether the cursor can cross between the two screens (in the shared plane), as in cross.
bool touches(const QRect &a, const QRect &b)
{
    const auto near = [](int gap) { return gap <= Router::kMaxGap && gap >= -Router::kMaxGap; };
    const bool alongY = a.top() <= b.bottom() && b.top() <= a.bottom();
    const bool alongX = a.left() <= b.right() && b.left() <= a.right();
    return (alongY && (near(b.left() - (a.right() + 1)) || near(a.left() - (b.right() + 1))))
           || (alongX && (near(b.top() - (a.bottom() + 1)) || near(a.top() - (b.bottom() + 1))));
}
} // namespace

bool Router::canReach(const QByteArray &from, const QByteArray &to) const
{
    const auto hasScreens = [this](const QByteArray &device) {
        return std::any_of(m_screens.begin(), m_screens.end(), [&](const Screen &s) { return s.device == device; });
    };
    if (!hasScreens(from) || !hasScreens(to))
        return false;
    QSet<QByteArray> seen{from};
    QList<QByteArray> queue{from};
    while (!queue.isEmpty()) {
        const QByteArray device = queue.takeFirst();
        if (device == to)
            return true;
        for (const Screen &a : m_screens) {
            if (a.device != device)
                continue;
            for (const Screen &b : m_screens) {
                if (b.device != device && !seen.contains(b.device) && touches(a.global, b.global)) {
                    seen.insert(b.device);
                    queue.append(b.device);
                }
            }
        }
    }
    return false;
}

QPoint Router::primaryCenter(const QByteArray &device) const
{
    const Screen *any = nullptr;
    for (const Screen &s : m_screens) {
        if (s.device != device)
            continue;
        if (s.primary)
            return s.local.center();
        if (!any)
            any = &s;
    }
    return any ? any->local.center() : QPoint();
}

QRect Router::primaryRect(const QByteArray &device) const
{
    QRect any;
    for (const Screen &s : m_screens) {
        if (s.device != device)
            continue;
        if (s.primary)
            return s.local;
        if (any.isNull())
            any = s.local;
    }
    return any;
}

QList<QRect> Router::screens(const QByteArray &device) const
{
    QList<QRect> out;
    for (const Screen &s : m_screens) {
        if (s.device == device)
            out.append(s.local);
    }
    return out;
}

QRect Router::screenAt(const QByteArray &device, QPoint p) const
{
    for (const Screen &s : m_screens) {
        if (s.device == device && s.local.contains(p))
            return s.local;
    }
    return {};
}

} // namespace df
