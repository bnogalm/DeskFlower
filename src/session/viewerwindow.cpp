#include "viewerwindow.h"

#include "localscreens.h"

#include <QCborArray>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>

namespace {
// Classic arrow, 12 x 19 logical pixels, tip at (0, 0).
QPainterPath arrow()
{
    QPainterPath path;
    path.moveTo(0, 0);
    path.lineTo(0, 17);
    path.lineTo(4, 13);
    path.lineTo(7, 19);
    path.lineTo(9, 18);
    path.lineTo(6, 12);
    path.lineTo(12, 12);
    path.closeSubpath();
    return path;
}
} // namespace

ViewerWindow::ViewerWindow(const QRect &nativeMonitor, QSize remoteSize) : m_remoteSize(remoteSize)
{
    setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus
             | Qt::WindowTransparentForInput);
    setTitle(QStringLiteral("DeskFlower remote view"));
    // The service speaks native coordinates; find the QScreen for that monitor.
    QScreen *screen = QGuiApplication::primaryScreen();
    for (QScreen *s : QGuiApplication::screens()) {
        if (LocalScreens::nativeGeometry(s) == nativeMonitor)
            screen = s;
    }
    setScreen(screen);
    setGeometry(screen->geometry());
    show();
}

void ViewerWindow::applyFrame(const QCborMap &frame)
{
    QList<df::view::Tile> tiles;
    for (const QCborValue &v : frame.value(QStringLiteral("tiles")).toArray()) {
        const QCborMap m = v.toMap();
        tiles.append(df::view::Tile{QRect(int(m.value(QStringLiteral("x")).toInteger()),
                                          int(m.value(QStringLiteral("y")).toInteger()),
                                          int(m.value(QStringLiteral("w")).toInteger()),
                                          int(m.value(QStringLiteral("h")).toInteger())),
                                    m.value(QStringLiteral("d")).toByteArray()});
    }
    const QSize size(int(frame.value(QStringLiteral("w")).toInteger()), int(frame.value(QStringLiteral("h")).toInteger()));
    const bool resized = m_canvas.image().size() != size;
    m_canvas.apply(size, tiles);
    if (resized || frame.value(QStringLiteral("full")).toBool()) {
        update();
        return;
    }
    // Repaint only the changed tiles, mapped to the window.
    const QRect t = target();
    const double sx = double(t.width()) / qMax(1, size.width()), sy = double(t.height()) / qMax(1, size.height());
    for (const df::view::Tile &tile : tiles) {
        update(QRect(t.left() + int(tile.rect.x() * sx) - 1, t.top() + int(tile.rect.y() * sy) - 1,
                     int(tile.rect.width() * sx) + 3, int(tile.rect.height() * sy) + 3));
    }
}

void ViewerWindow::setRemoteSize(QSize remoteSize)
{
    m_remoteSize = remoteSize;
    update();
}

void ViewerWindow::setRemoteCursor(QPoint p)
{
    const QRect before = cursorRect(m_cursor);
    m_cursor = p;
    update(before);
    update(cursorRect(m_cursor));
}

void ViewerWindow::setStatus(const QString &text, bool failed)
{
    m_status = text;
    m_failed = failed;
    update();
}

QRect ViewerWindow::target() const
{
    const QSize frame = m_canvas.image().isNull() ? m_remoteSize : m_canvas.image().size();
    const QSize fitted = frame.scaled(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
}

QRect ViewerWindow::cursorRect(QPoint remote) const
{
    if (remote.x() < 0 || m_remoteSize.isEmpty())
        return {};
    const QRect t = target();
    const QPoint at(t.left() + int(double(remote.x()) * t.width() / m_remoteSize.width()),
                    t.top() + int(double(remote.y()) * t.height() / m_remoteSize.height()));
    return QRect(at - QPoint(2, 2), QSize(18, 24));
}

void ViewerWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect all(QPoint(), size());
    p.fillRect(all, Qt::black);
    const QColor failedColor(255, 140, 140);
    if (m_canvas.image().isNull()) {
        p.setPen(m_failed ? failedColor : QColor(Qt::white));
        p.drawText(all, Qt::AlignCenter | Qt::TextWordWrap,
                   m_status.isEmpty() ? QStringLiteral("Connecting to the remote screen…") : m_status);
    } else {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target(), m_canvas.image());
        if (m_failed && !m_status.isEmpty()) {
            // The reason over the last image, on a dark band.
            QRect band(0, 0, width(), 3 * p.fontMetrics().height());
            band.moveCenter(all.center());
            p.fillRect(band, QColor(0, 0, 0, 200));
            p.setPen(failedColor);
            p.drawText(band, Qt::AlignCenter | Qt::TextWordWrap, m_status);
        }
    }
    if (m_failed)
        return; // the remote cursor means nothing any more
    const QRect c = cursorRect(m_cursor);
    if (!c.isNull()) {
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(c.topLeft() + QPoint(2, 2));
        p.setPen(QPen(Qt::black, 1.2));
        p.setBrush(Qt::white);
        p.drawPath(arrow());
    }
}
