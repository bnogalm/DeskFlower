#pragma once

// Viewer side of the remote view: a frameless, always-on-top window covering one local monitor,
// showing one remote screen (letterboxed) and the remote cursor when it is on that screen. It never
// takes focus or input: the keyboard and mouse are captured by the input backend and forwarded.
// Until the first frame it shows the service's progress text; a failure reason is shown over the
// last image so the user sees why the view ended.

#include "framecodec.h"

#include <QCborMap>
#include <QRasterWindow>

class ViewerWindow : public QRasterWindow
{
public:
    // nativeMonitor: the local monitor in native desktop coordinates; index: the remote screen
    // shown (frames and the cursor are tagged with it); remoteSize: size of that screen in its own
    // coordinates (the cursor position is relative to it).
    ViewerWindow(const QRect &nativeMonitor, int index, QSize remoteSize);

    void applyFrame(const QCborMap &frame);
    void setRemoteCursor(QPoint p);
    // Another remote screen is shown here: blank until its first frame unless `canvas` holds its
    // image from the window that showed it before.
    void setRemote(int index, QSize remoteSize, const df::view::FrameCanvas &canvas = {});
    void setStatus(const QString &text, bool failed);
    bool hasFailed() const { return m_failed; }
    int index() const { return m_index; }
    QRect nativeMonitor() const { return m_monitor; }
    const df::view::FrameCanvas &canvas() const { return m_canvas; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRect target() const;
    QRect cursorRect(QPoint remote) const;

    QRect m_monitor;
    int m_index;
    QSize m_remoteSize;
    df::view::FrameCanvas m_canvas;
    QPoint m_cursor{-1, -1};
    QString m_status;
    bool m_failed = false;
};
