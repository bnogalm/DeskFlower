#pragma once

// Where ClipboardBridge reads and writes the session's clipboard. The default goes through
// QClipboard (Windows, macOS, X11). A Wayland session helper runs in the background without a
// focused window, so the compositor gives it no clipboard through the Wayland protocol: there it
// is the xdg-desktop-portal Clipboard interface of the remote desktop session instead
// (src/session/portalclipboard.*).

#include <QObject>

class QMimeData;

class ClipboardBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~ClipboardBackend() override = default;

    // The current content (owned by the backend, valid until the next call or change). May be
    // null when there is nothing, or nothing readable.
    virtual const QMimeData *mimeData() = 0;
    // Puts `data` on the clipboard, taking ownership; its formats are rendered when another
    // application asks for them (QMimeData::retrieveData), as with QClipboard.
    virtual void setMimeData(QMimeData *data) = 0;
    // Whether the content on the clipboard is the one set here last.
    virtual bool ownsClipboard() const = 0;

    // The QClipboard implementation.
    static ClipboardBackend *createDefault(QObject *parent = nullptr);

signals:
    // The clipboard changed (also for our own setMimeData; ClipboardBridge tells them apart).
    void changed();
};
