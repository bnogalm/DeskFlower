#include "screenstreamer.h"

#include "framecodec.h"
#include "localscreens.h"
#include "screengrabber.h"

#include <QCborArray>
#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>

#include <utility>

// Lives on the streamer's worker thread.
class CaptureWorker : public QObject
{
    Q_OBJECT
public:
    explicit CaptureWorker(std::unique_ptr<ScreenGrabber> grabber) : m_grabber(std::move(grabber)) {}

public slots:
    // Windows: grab here (waits up to 100 ms for a change), then encode.
    void captureAndEncode(QSize maxSize, bool full, quint32 seq, quint32 generation)
    {
        QImage image;
        const ScreenGrabber::Result r = m_grabber ? m_grabber->grab(&image, 100) : ScreenGrabber::Result::Failed;
        if (r == ScreenGrabber::Result::Changed)
            m_last = image;
        // A failure counts even with an older image: a locked desktop must not stream a frozen one.
        if (r == ScreenGrabber::Result::Failed || m_last.isNull()) {
            emit captureFailed(generation);
            return;
        }
        encode(m_last, maxSize, full, seq, generation);
    }

    // Elsewhere the GUI thread grabs and hands the image over.
    void encode(const QImage &image, QSize maxSize, bool full, quint32 seq, quint32 generation)
    {
        if (image.isNull()) {
            emit captureFailed(generation);
            return;
        }
        // Scaled to the viewer's monitor: a 4K screen shown on a 1080p monitor sends a quarter.
        QImage frame = image;
        if (maxSize.isValid() && (image.width() > maxSize.width() || image.height() > maxSize.height()))
            frame = image.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QCborArray tiles;
        for (const df::view::Tile &t : m_encoder.encode(frame, full)) {
            QCborMap m;
            m.insert(QStringLiteral("x"), t.rect.x());
            m.insert(QStringLiteral("y"), t.rect.y());
            m.insert(QStringLiteral("w"), t.rect.width());
            m.insert(QStringLiteral("h"), t.rect.height());
            m.insert(QStringLiteral("d"), t.jpeg);
            tiles.append(m);
        }
        QCborMap f;
        f.insert(QStringLiteral("s"), qint64(seq));
        f.insert(QStringLiteral("w"), frame.width());
        f.insert(QStringLiteral("h"), frame.height());
        f.insert(QStringLiteral("full"), full);
        f.insert(QStringLiteral("tiles"), tiles);
        emit encoded(f, generation);
    }

    // Another screen: a complete image of it comes next.
    void setScreen(const QRect &screen)
    {
        m_encoder.reset();
        m_last = QImage();
        if (m_grabber)
            m_grabber->setScreen(screen);
    }

    // A new stream: nothing from the previous one may be sent again.
    void reset()
    {
        m_encoder.reset();
        m_last = QImage();
        if (m_grabber)
            m_grabber->reset();
    }

signals:
    // `generation` tells streams apart: results of an earlier one are dropped.
    void encoded(const QCborMap &frame, quint32 generation);
    void captureFailed(quint32 generation);

private:
    std::unique_ptr<ScreenGrabber> m_grabber;
    df::view::FrameEncoder m_encoder;
    QImage m_last;
};

ScreenStreamer::ScreenStreamer(QObject *parent) : QObject(parent)
{
    std::unique_ptr<ScreenGrabber> grabber = ScreenGrabber::create();
    m_hasNativeGrabber = grabber != nullptr;
    m_worker = new CaptureWorker(std::move(grabber));
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &CaptureWorker::encoded, this, [this](const QCborMap &frame, quint32 generation) {
        if (!m_running || generation != m_generation)
            return;
        m_failures = 0;
        emit frameReady(frame);
    });
    connect(m_worker, &CaptureWorker::captureFailed, this, [this](quint32 generation) {
        if (generation == m_generation)
            onCaptureFailed();
    });
    m_thread.start();

    m_pace.setSingleShot(true);
    connect(&m_pace, &QTimer::timeout, this, &ScreenStreamer::request);
}

ScreenStreamer::~ScreenStreamer()
{
    m_running = false;
    m_thread.quit();
    m_thread.wait();
}

void ScreenStreamer::start(QSize maxSize, const QRect &screen)
{
    m_maxSize = maxSize;
    m_screen = screen;
    m_forceFull = false;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, screen] { w->setScreen(screen); }, Qt::QueuedConnection);
    m_running = true;
    ++m_generation;
    m_seq = 0;
    m_failures = 0;
    QMetaObject::invokeMethod(m_worker, &CaptureWorker::reset, Qt::QueuedConnection);
    request();
}

void ScreenStreamer::setScreen(const QRect &screen)
{
    if (screen == m_screen)
        return;
    m_screen = screen;
    m_forceFull = true;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, screen] { w->setScreen(screen); }, Qt::QueuedConnection);
}

void ScreenStreamer::stop()
{
    m_running = false;
    m_pace.stop();
}

void ScreenStreamer::next()
{
    if (!m_running)
        return;
    const qint64 elapsed = m_sinceLast.isValid() ? m_sinceLast.elapsed() : kMinFrameIntervalMs;
    if (elapsed >= kMinFrameIntervalMs)
        request();
    else
        m_pace.start(int(kMinFrameIntervalMs - elapsed));
}

void ScreenStreamer::request()
{
    if (!m_running)
        return;
    m_sinceLast.restart();
    const quint32 seq = ++m_seq;
    const bool full = seq % kKeyFrameEvery == 1 || std::exchange(m_forceFull, false);
    if (m_hasNativeGrabber) {
        QMetaObject::invokeMethod(m_worker, "captureAndEncode", Qt::QueuedConnection, Q_ARG(QSize, m_maxSize),
                                  Q_ARG(bool, full), Q_ARG(quint32, seq), Q_ARG(quint32, m_generation));
        return;
    }
    QScreen *screen = QGuiApplication::primaryScreen();
    for (QScreen *s : QGuiApplication::screens()) {
        if (!m_screen.isEmpty() && LocalScreens::nativeGeometry(s) == m_screen)
            screen = s;
    }
    if (!screen) {
        onCaptureFailed();
        return;
    }
    const QImage image = screen->grabWindow(0).toImage();
    QMetaObject::invokeMethod(m_worker, "encode", Qt::QueuedConnection, Q_ARG(QImage, image),
                              Q_ARG(QSize, m_maxSize), Q_ARG(bool, full), Q_ARG(quint32, seq),
                              Q_ARG(quint32, m_generation));
}

void ScreenStreamer::onCaptureFailed()
{
    if (!m_running)
        return;
    if (++m_failures < kMaxCaptureFailures) {
        m_pace.start(kCaptureRetryMs);
        return;
    }
    qWarning("Screen capture keeps failing: giving up");
    stop();
    emit failed(QStringLiteral("Cannot capture the screen (locked or protected desktop)"));
}

#include "screenstreamer.moc"
