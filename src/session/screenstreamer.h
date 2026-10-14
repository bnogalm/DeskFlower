#pragma once

// Source side of the remote view: captures one screen (the primary one unless the viewer chose
// another) when the viewer is ready for another frame (at most ~30 per second), scales it to the
// viewer's monitor and encodes the changed tiles.
// Capture (on Windows) and encoding run on a worker thread. When no image can be captured (locked
// or protected desktop) it retries for a few seconds, then reports failed() once and stops.

#include <QCborMap>
#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QThread>
#include <QTimer>

class CaptureWorker;

class ScreenStreamer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMinFrameIntervalMs = 33;
    static constexpr int kKeyFrameEvery = 300; // a full frame now and then repairs any damage
    static constexpr int kCaptureRetryMs = 250;
    static constexpr int kMaxCaptureFailures = 20; // about 5 s of retries

    explicit ScreenStreamer(QObject *parent = nullptr);
    ~ScreenStreamer() override;

    // screen: native geometry of the screen to capture; empty: the primary screen.
    void start(QSize maxSize, const QRect &screen = {});
    void setScreen(const QRect &screen);
    void stop();
    void next(); // the viewer showed the previous frame
    bool isRunning() const { return m_running; }

signals:
    // {s, w, h, full, tiles: [{x, y, w, h, d}]}
    void frameReady(const QCborMap &frame);
    void failed(const QString &reason);

private:
    void request();
    void onCaptureFailed();

    QThread m_thread;
    CaptureWorker *m_worker = nullptr;
    bool m_hasNativeGrabber = false;
    bool m_running = false;
    QSize m_maxSize;
    QRect m_screen;
    bool m_forceFull = false; // the next frame is a complete one (another screen)
    quint32 m_seq = 0;
    quint32 m_generation = 0; // one per start(): late results of an earlier stream are dropped
    int m_failures = 0;
    QElapsedTimer m_sinceLast;
    QTimer m_pace;
};
