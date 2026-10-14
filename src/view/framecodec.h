#pragma once

// Screen streaming codec for the remote view: the frame is cut into square tiles and only the
// tiles that changed since the previous frame are sent, each as a JPEG. Simple, portable and good
// enough on a LAN for desktop work; a video codec (H.264) could replace it later.

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QRect>

namespace df::view {

struct Tile {
    QRect rect;      // in frame pixels
    QByteArray jpeg; // encoded content of rect
};

class FrameEncoder
{
public:
    static constexpr int kTileSize = 128;

    explicit FrameEncoder(int quality = 70) : m_quality(quality) {}

    // Tiles that differ from the previous frame (all of them when `full`, on the first frame or
    // when the size changes).
    QList<Tile> encode(const QImage &frame, bool full);
    void reset() { m_previous = QImage(); }

private:
    bool tileChanged(const QImage &frame, const QRect &rect) const;

    int m_quality;
    QImage m_previous;
};

// Receiver side: applies tiles onto an image of the remote frame.
class FrameCanvas
{
public:
    // Returns false if a tile does not decode or lies outside the frame.
    bool apply(QSize frameSize, const QList<Tile> &tiles);
    const QImage &image() const { return m_image; }

private:
    QImage m_image;
};

} // namespace df::view
