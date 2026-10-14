#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

// Serves screen thumbnails to QML as "image://thumbs/<key>?r=<revision>".
// The revision only exists to make QML reload the image; it is ignored here.
class ThumbnailProvider : public QQuickImageProvider
{
public:
    ThumbnailProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    void set(const QString &key, const QImage &image)
    {
        QMutexLocker lock(&m_mutex);
        m_images.insert(key, image);
    }

    void remove(const QString &key)
    {
        QMutexLocker lock(&m_mutex);
        m_images.remove(key);
    }

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        QImage image;
        {
            QMutexLocker lock(&m_mutex);
            image = m_images.value(id.section(QLatin1Char('?'), 0, 0));
        }
        if (size)
            *size = image.size();
        if (!image.isNull() && requestedSize.isValid())
            image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return image;
    }

private:
    QMutex m_mutex;
    QHash<QString, QImage> m_images;
};
