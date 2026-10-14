#pragma once

#include <QQuickImageProvider>

// Renders "image://qr/<percent-encoded text>" as a QR code (black on white, with quiet zone).
class QrImageProvider : public QQuickImageProvider
{
public:
    QrImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
