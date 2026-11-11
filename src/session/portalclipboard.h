#pragma once

// The clipboard of a Wayland session through the remote desktop portal's Clipboard interface
// (see PortalSession), for ClipboardBridge:
//  - reading: SelectionOwnerChanged says the clipboard changed and which formats it has; the
//    formats ClipboardBridge looks at are read through SelectionRead (a pipe) when it asks;
//  - writing: SetSelection announces the formats of the QMimeData set here; when an application
//    pastes, SelectionTransfer asks for one, which is rendered then (a sealed format is decrypted
//    only at that moment) and written through SelectionWrite, then SelectionWriteDone.
// Until the portal session runs (or if it has no clipboard) nothing is read and the content set
// here waits, announced as soon as the session starts.

#include "clipboardbackend.h"

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QStringList>
#include <QVariantMap>

#include <memory>

class PortalSession;
class QMimeData;

class PortalClipboard : public ClipboardBackend, protected QDBusContext
{
    Q_OBJECT
public:
    explicit PortalClipboard(PortalSession *portal, QObject *parent = nullptr);
    ~PortalClipboard() override;

    const QMimeData *mimeData() override;
    void setMimeData(QMimeData *data) override;
    bool ownsClipboard() const override { return m_owned; }

    // The formats announced for `data` (what applications see) and the bytes of one of them.
    static QStringList offeredFormats(const QMimeData *data);
    static QByteArray render(const QMimeData *data, const QString &mime);

private slots:
    void onOwnerChanged(const QDBusObjectPath &session, const QVariantMap &options);
    void onTransfer(const QDBusObjectPath &session, const QString &mime, uint serial);

private:
    void announce();
    QByteArray read(const QString &mime) const;

    PortalSession *m_portal;
    // What we set (rendered on request). Shared: a render (a sealed format waits for the service
    // in a nested event loop) keeps it alive when another application copies meanwhile.
    std::shared_ptr<QMimeData> m_own;
    bool m_owned = false;
    QStringList m_available;               // formats of another application's content
    std::unique_ptr<QMimeData> m_snapshot; // its formats read for ClipboardBridge
    bool m_snapshotValid = false;
};
