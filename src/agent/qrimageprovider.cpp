#include "qrimageprovider.h"

#include <QPainter>
#include <QUrl>

#include <qrcodegen.hpp>

QImage QrImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QByteArray text = QUrl::fromPercentEncoding(id.toUtf8()).toUtf8();
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int kQuietZone = 4; // modules, as the standard asks
    const int modules = qr.getSize() + 2 * kQuietZone;
    const int wanted = requestedSize.isValid() ? qMin(requestedSize.width(), requestedSize.height()) : 512;
    const int scale = qMax(1, wanted / modules);

    QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y))
                p.fillRect((x + kQuietZone) * scale, (y + kQuietZone) * scale, scale, scale, Qt::black);
        }
    }
    p.end();
    if (size)
        *size = image.size();
    return image;
}
