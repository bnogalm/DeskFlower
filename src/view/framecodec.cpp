#include "framecodec.h"

#include <QBuffer>
#include <QPainter>

#include <cstring>

namespace df::view {

bool FrameEncoder::tileChanged(const QImage &frame, const QRect &rect) const
{
    const int bytes = rect.width() * 4;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *a = frame.constScanLine(y) + rect.left() * 4;
        const uchar *b = m_previous.constScanLine(y) + rect.left() * 4;
        if (std::memcmp(a, b, size_t(bytes)) != 0)
            return true;
    }
    return false;
}

QList<Tile> FrameEncoder::encode(const QImage &input, bool full)
{
    const QImage frame = input.format() == QImage::Format_RGB32 ? input : input.convertToFormat(QImage::Format_RGB32);
    full = full || m_previous.size() != frame.size();

    QList<Tile> tiles;
    for (int y = 0; y < frame.height(); y += kTileSize) {
        for (int x = 0; x < frame.width(); x += kTileSize) {
            const QRect rect = QRect(x, y, kTileSize, kTileSize).intersected(frame.rect());
            if (!full && !tileChanged(frame, rect))
                continue;
            QByteArray jpeg;
            QBuffer buffer(&jpeg);
            buffer.open(QIODevice::WriteOnly);
            frame.copy(rect).save(&buffer, "JPG", m_quality);
            tiles.append(Tile{rect, jpeg});
        }
    }
    m_previous = frame;
    return tiles;
}

bool FrameCanvas::apply(QSize frameSize, const QList<Tile> &tiles)
{
    if (frameSize.isEmpty())
        return false;
    if (m_image.size() != frameSize) {
        m_image = QImage(frameSize, QImage::Format_RGB32);
        m_image.fill(Qt::black);
    }
    bool ok = true;
    QPainter painter(&m_image);
    for (const Tile &tile : tiles) {
        const QImage part = QImage::fromData(tile.jpeg, "JPG");
        if (part.isNull() || !m_image.rect().contains(tile.rect) || part.size() != tile.rect.size()) {
            ok = false;
            continue;
        }
        painter.drawImage(tile.rect.topLeft(), part);
    }
    return ok;
}

} // namespace df::view
