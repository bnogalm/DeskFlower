#pragma once

// The local end of clipboard sharing: watches this session's clipboard and reports changes
// (text/plain, text/html, image/png, copied files), and puts content received from the service on
// it. Used by the process that owns the user's clipboard: on Windows in service mode the agent,
// which runs as the user (the session helper runs as LocalSystem, and Windows does not give it the
// user's clipboard reliably), elsewhere the session helper.

#include <QCborMap>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

class ClipboardBridge : public QObject
{
    Q_OBJECT
public:
    explicit ClipboardBridge(QObject *parent = nullptr);
    ~ClipboardBridge() override;

    // Transfers the files of a deferred paste and returns their local paths (empty + error if not).
    using FetchFiles = std::function<QStringList(QString *error)>;

    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }
    // Puts content on the local clipboard; its own change notification is not reported back.
    void apply(const QCborMap &formats, const QStringList &files);
    // Offers `count` files copied on another device; they are transferred (fetch, blocking) only
    // when an application asks for them, i.e. when the user pastes. Windows only.
    static bool supportsDeferredFiles();
    void applyDeferredFiles(int count, FetchFiles fetch);
    // Same, with the names and sizes known: offered as virtual files (file descriptors + contents),
    // which other programs can look at without causing the transfer; Explorer reads the contents,
    // with its own progress, only when the user pastes. relativePaths use '/'.
    struct ListedFile {
        QString path;
        qint64 size = 0;
    };
    void applyVirtualFiles(const QList<ListedFile> &files, FetchFiles fetch);

signals:
    // The user copied something: {mime: bytes} and local file paths.
    void changed(const QCborMap &formats, const QStringList &files);

private:
    void onChanged();
    void releaseDeferred();

    bool m_enabled = false;
    QTimer m_debounce;
    QString m_appliedSignature; // what we last put on the clipboard ourselves...
    qint64 m_suppressUntil = 0; // ...and until when its echo is ignored
    void *m_deferred = nullptr;  // Windows: our IDataObject while it is (or was) on the clipboard
};
