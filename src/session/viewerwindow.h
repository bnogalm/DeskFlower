#pragma once

// Viewer side of the remote view: a frameless, always-on-top window covering one local monitor,
// showing the remote screen (letterboxed) and the remote cursor. It never takes focus or input:
// the keyboard and mouse are captured by the input backend and forwarded.
// Until the first frame it shows the service's progress text; a failure reason is shown over the
// last image so the user sees why the view ended.

#include "framecodec.h"

#include <QCborMap>
#include <QRasterWindow>

class ViewerWindow : public QRasterWindow
{
public:
    // nativeMonitor: the local monitor in native desktop coordinates; remoteSize: size of the remote
    // screen in its own coordinates (the cursor position is relative to it).
    ViewerWindow(const QRect &nativeMonitor, QSize remoteSize);

    void applyFrame(const QCborMap &frame);
    void setRemoteCursor(QPoint p);
    // Another remote screen is shown (the cursor position is relative to it).
    void setRemoteSize(QSize remoteSize);
    void setStatus(const QString &text, bool failed);
    bool hasFailed() const { return m_failed; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRect target() const;
    QRect cursorRect(QPoint remote) const;

    QSize m_remoteSize;
    df::view::FrameCanvas m_canvas;
    QPoint m_cursor{-1, -1};
    QString m_status;
    bool m_failed = false;
};
