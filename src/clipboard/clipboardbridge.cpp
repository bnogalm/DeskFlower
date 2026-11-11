#include "clipboardbridge.h"
#include "clipboardbackend.h"

#ifdef Q_OS_WIN
#include "virtualfiles.h"
#endif
#ifdef Q_OS_MACOS
#include "clipboardbridge_mac.h"
#endif

#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <utility>

#ifdef Q_OS_WIN
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

#ifndef Q_OS_WIN
Q_DECLARE_LOGGING_CATEGORY(lcClipBridge) // defined below, used by the lazy mime data first

namespace {
// Renders in progress (retrieveData of a SealedMimeData): the service is asked in a nested event
// loop (SessionClient::requestFromService), during which a clipboard replacement would delete the
// object being rendered. ClipboardBridge defers a replacement while this is non-zero.
int s_rendering = 0;
struct RenderScope {
    RenderScope() { ++s_rendering; }
    ~RenderScope() { --s_rendering; }
};

// Also the native pasteboard items on macOS (clipboardbridge_mac.mm).
bool renderInProgress()
{
#ifdef Q_OS_MACOS
    if (df::macclipboard::rendering())
        return true;
#endif
    return s_rendering > 0;
}

// Sealed content on Linux/macOS: a lazy QMimeData whose formats are decrypted only when another
// application reads them (retrieveData), and whose copied files (text/uri-list) are transferred
// only when read. A rendered format is kept for the object's lifetime. Gated content (a guest's
// clipboard): a refusal is never cached (a manager asking on arrival must not spoil the paste that
// follows), and a rendered format is kept only as long as the paste grant latches, then dropped:
// a manager polling later asks the service again and is refused. With copied files the object
// also offers KDE's password manager hint ("secret"), which Klipper and the clipboard managers
// that follow it take as "do not record": their data moves only for a paste.
class SealedMimeData final : public QMimeData
{
public:
    static constexpr qint64 kRenderedKeptMs = 5000;
    static constexpr const char *kPasswordManagerHint = "x-kde-passwordManagerHint";

    SealedMimeData(const QList<ClipboardBridge::SealedFormat> &formats, int pendingFiles,
                   ClipboardBridge::UnsealFormat unseal, ClipboardBridge::FetchFiles fetch, bool gated)
        : m_pendingFiles(pendingFiles), m_gated(gated), m_unseal(std::move(unseal)), m_fetch(std::move(fetch))
    {
        for (const ClipboardBridge::SealedFormat &f : formats)
            m_formats.append(f.mime);
    }

    QStringList formats() const override
    {
        QStringList out;
        for (const QString &mime : m_formats) {
            out.append(mime);
            if (mime == QLatin1String("text/plain"))
                out.append(QStringLiteral("text/plain;charset=utf-8"));
        }
        if (m_pendingFiles > 0) {
            out.append(QStringLiteral("text/uri-list"));
            out.append(QStringLiteral("x-special/gnome-copied-files"));
            out.append(QStringLiteral("application/x-kde-cutselection"));
            out.append(QLatin1String(kPasswordManagerHint));
        }
        return out;
    }
    bool hasFormat(const QString &mime) const override { return formats().contains(mime); }

protected:
    QVariant retrieveData(const QString &mimeType, QMetaType type) const override
    {
        const RenderScope rendering;
        if (mimeType == QLatin1String("application/x-kde-cutselection"))
            return m_pendingFiles > 0 ? QVariant(QByteArray("0")) : QVariant();
        if (mimeType == QLatin1String(kPasswordManagerHint))
            return m_pendingFiles > 0 ? QVariant(QByteArray("secret")) : QVariant();
        if (mimeType == QLatin1String("text/uri-list") || mimeType == QLatin1String("x-special/gnome-copied-files")) {
            const QStringList files = pastedFiles();
            if (files.isEmpty())
                return {};
            if (mimeType != QLatin1String("text/uri-list"))
                return ClipboardBridge::gnomeCopiedFiles(files);
            QByteArray list;
            for (const QString &f : files)
                list += QUrl::fromLocalFile(f).toEncoded() + "\r\n";
            return list;
        }
        QString mime = mimeType;
        if (mime.startsWith(QLatin1String("text/plain")))
            mime = QStringLiteral("text/plain");
        if (!m_formats.contains(mime))
            return {};
        if (!m_clock.isValid())
            m_clock.start();
        const qint64 now = m_clock.elapsed();
        auto it = m_cache.find(mime);
        if (it != m_cache.end() && m_gated && now - it->at >= kRenderedKeptMs) {
            it->bytes.fill('\0');
            m_cache.erase(it);
            it = m_cache.end();
        }
        QByteArray plain;
        if (it != m_cache.end()) {
            plain = it->bytes;
        } else {
            QString error;
            plain = m_unseal ? m_unseal(mime, &error) : QByteArray();
            if (plain.isEmpty())
                return {}; // refused: not cached, the next request asks again
            m_cache.insert(mime, Rendered{plain, now});
        }
        if (mimeType.startsWith(QLatin1String("text/")) && type.id() == QMetaType::QString)
            return QString::fromUtf8(plain);
        if (mime == QLatin1String("image/png") && type.id() == QMetaType::QImage) {
            QImage image;
            if (image.loadFromData(plain, "PNG"))
                return image;
        }
        return plain;
    }

private:
    // The copied files' local paths, transferred (once) when first read, i.e. at the paste. The
    // helper's fetch does not hold a paste for a long transfer: it may answer "not yet" (empty),
    // and a paste once the transfer ended gets them.
    QStringList pastedFiles() const
    {
        if (m_pendingFiles <= 0)
            return {};
        if (m_files.isEmpty()) {
            QString error;
            m_files = m_fetch ? m_fetch(&error) : QStringList();
            if (m_files.isEmpty()) {
                if (error == QLatin1String(ClipboardBridge::kNotAPaste))
                    qCInfo(lcClipBridge) << "Copied files not transferred: not a paste";
                else
                    qCWarning(lcClipBridge) << "Paste failed:" << error;
                return {}; // refused, failed or not there yet: not cached, the next request asks again
            }
        }
        return m_files;
    }

    struct Rendered {
        QByteArray bytes;
        qint64 at = 0;
    };
    QStringList m_formats;
    int m_pendingFiles = 0;
    bool m_gated = false;
    ClipboardBridge::UnsealFormat m_unseal;
    ClipboardBridge::FetchFiles m_fetch;
    mutable QHash<QString, Rendered> m_cache;
    mutable QStringList m_files;
    mutable QElapsedTimer m_clock;
};
} // namespace
#endif

Q_LOGGING_CATEGORY(lcClipBridge, "deskflower.clipboard.local", QtInfoMsg)

ClipboardBridge::ClipboardBridge(QObject *parent) : QObject(parent)
{
    // Debounce bursts of change notifications (some applications set formats one by one).
    m_debounce.setInterval(150);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &ClipboardBridge::onChanged);
    setBackend(ClipboardBackend::createDefault());
#ifdef Q_OS_MACOS
    // Qt notices a change of the pasteboard only when the application is activated (never, for
    // the background helper): its change counter is polled. Cheap (a counter, no content, no
    // permission); our own writes are skipped.
    m_lastChangeCount = df::macclipboard::changeCount();
    m_pasteboardPoll.setInterval(300);
    connect(&m_pasteboardPoll, &QTimer::timeout, this, [this] {
        const qint64 count = df::macclipboard::changeCount();
        if (count == m_lastChangeCount)
            return;
        m_lastChangeCount = count;
        if (m_enabled && count != m_ownChangeCount)
            m_debounce.start();
    });
    m_pasteboardPoll.start();
#endif
}

void ClipboardBridge::setBackend(ClipboardBackend *backend)
{
    delete m_backend;
    m_backend = backend;
    m_backend->setParent(this);
#ifndef Q_OS_MACOS
    connect(m_backend, &ClipboardBackend::changed, this, [this] {
        if (m_enabled)
            m_debounce.start();
    });
#endif
}

QByteArray ClipboardBridge::gnomeCopiedFiles(const QStringList &paths)
{
    QByteArray out("copy");
    for (const QString &p : paths)
        out += '\n' + QUrl::fromLocalFile(p).toEncoded();
    return out;
}

ClipboardBridge::~ClipboardBridge()
{
    releaseDeferred();
}

bool ClipboardBridge::supportsDeferredFiles()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void ClipboardBridge::applyDeferredFiles(int count, FetchFiles fetch)
{
#ifdef Q_OS_WIN
    if (deferWhileRendering([this, count, fetch] { applyDeferredFiles(count, fetch); }))
        return; // see applySealed
    releaseDeferred();
    // A deferred CF_HDROP that transfers the files at the first request (with the clipboard
    // object's markers: history and monitors leave it alone).
    IDataObject *object = createSealedData({}, {}, {}, count, std::move(fetch));
    const HRESULT hr = OleSetClipboard(object);
    if (FAILED(hr)) {
        qCWarning(lcClipBridge) << "OleSetClipboard failed:" << Qt::hex << quint32(hr);
        object->Release();
        return;
    }
    m_deferred = object; // our reference; the clipboard holds its own
    qCInfo(lcClipBridge) << "Clipboard set:" << count << "files, transferred when pasted";
#else
    Q_UNUSED(count)
    Q_UNUSED(fetch)
#endif
}

void ClipboardBridge::applyVirtualFiles(const QList<ListedFile> &files, FetchFiles fetch, FileStreaming streaming)
{
#ifdef Q_OS_WIN
    if (deferWhileRendering([this, files, fetch, streaming] { applyVirtualFiles(files, fetch, streaming); }))
        return; // see applySealed
    releaseDeferred();
    // The clipboard flavour of the object (the transfer runs while the agent keeps working).
    IDataObject *object = createSealedData({}, {}, files, int(files.size()), std::move(fetch), false,
                                           DROPEFFECT_COPY, std::move(streaming));
    const HRESULT hr = OleSetClipboard(object);
    if (FAILED(hr)) {
        qCWarning(lcClipBridge) << "OleSetClipboard failed:" << Qt::hex << quint32(hr);
        object->Release();
        return;
    }
    m_deferred = object;
    qCInfo(lcClipBridge) << "Clipboard set:" << files.size() << "virtual files, transferred when pasted";
#else
    Q_UNUSED(files)
    Q_UNUSED(fetch)
    Q_UNUSED(streaming)
#endif
}

void ClipboardBridge::applySealed(const QList<SealedFormat> &formats, const QList<ListedFile> &files, int pendingFiles,
                                  UnsealFormat unseal, FetchFiles fetchFiles, bool gated, FileStreaming streaming)
{
#ifdef Q_OS_WIN
    // A paste of the object on the clipboard is transferring (fetchWhilePumping keeps this
    // thread's messages flowing meanwhile): replacing it now would release it under that render.
    // Once the render returns (deferWhileRendering: only the latest content waits).
    if (deferWhileRendering([this, formats, files, pendingFiles, unseal, fetchFiles, gated, streaming] {
            applySealed(formats, files, pendingFiles, unseal, fetchFiles, gated, streaming);
        }))
        return;
    releaseDeferred();
    IDataObject *object = createSealedData(formats, std::move(unseal), files, pendingFiles, std::move(fetchFiles),
                                           gated, DROPEFFECT_COPY, std::move(streaming));
    const HRESULT hr = OleSetClipboard(object);
    if (FAILED(hr)) {
        qCWarning(lcClipBridge) << "OleSetClipboard failed:" << Qt::hex << quint32(hr);
        object->Release();
        return;
    }
    // Deliberately not OleFlushClipboard: flushing would render every format at once (an unseal
    // request per format, refused since no paste was ordered), replacing the content with nothing.
    m_deferred = object;
    qCInfo(lcClipBridge) << "Clipboard set:" << formats.size() << "sealed formats," << pendingFiles
                         << "files, rendered on paste";
#elif defined(Q_OS_MACOS)
    // Native items (one per top-level file or folder, see clipboardbridge_mac.h): Qt's own
    // pasteboard code asks for every URL as soon as the data is set, which would transfer the
    // files at the copy (and be refused). Not while an item is being rendered: the pasteboard
    // server is waiting for that data (see below).
    Q_UNUSED(gated) // the service's grant decides (see the header)
    Q_UNUSED(streaming) // no file promises on macOS: the transfer happens at the paste
    if (renderInProgress()) {
        QTimer::singleShot(20, this, [this, formats, files, pendingFiles, unseal, fetchFiles] {
            applySealed(formats, files, pendingFiles, unseal, fetchFiles);
        });
        return;
    }
    releaseDeferred();
    m_ownsSealed = false;
    QStringList names;
    for (const ListedFile &f : files) {
        const QString top = f.path.section(QLatin1Char('/'), 0, 0);
        if (!top.isEmpty() && !names.contains(top))
            names.append(top);
    }
    if (names.isEmpty() && pendingFiles > 0) {
        // Unlisted: one item for them all. Once transferred, the files themselves (one file URL
        // each, as Finder pastes best) replace it for the pastes that follow.
        fetchFiles = [this, inner = std::move(fetchFiles)](QString *error) {
            const QStringList fetched = inner(error);
            if (fetched.size() > 1) {
                const qint64 sealedChange = m_ownChangeCount;
                QTimer::singleShot(0, this, [this, fetched, sealedChange] {
                    if (m_ownChangeCount == sealedChange && ownsClipboard())
                        apply({}, fetched);
                });
            }
            return fetched;
        };
    }
    m_ownChangeCount =
        df::macclipboard::setSealed(formats, names, pendingFiles, std::move(unseal), std::move(fetchFiles));
    m_lastChangeCount = m_ownChangeCount;
    qCInfo(lcClipBridge) << "Clipboard set:" << formats.size() << "sealed formats," << names.size()
                         << "items, rendered on paste";
#else
    if (renderInProgress()) {
        // The object on the clipboard is being rendered (a paste in progress, served in a nested
        // event loop): replacing it now would delete it under that render. Once the render returns.
        QTimer::singleShot(20, this, [this, formats, files, pendingFiles, unseal, fetchFiles, gated] {
            applySealed(formats, files, pendingFiles, unseal, fetchFiles, gated);
        });
        return;
    }
    Q_UNUSED(files)
    Q_UNUSED(streaming) // the helper's paste transfers the files whole
    releaseDeferred();
    auto *mime = new SealedMimeData(formats, pendingFiles, std::move(unseal), std::move(fetchFiles), gated);
    m_ownsSealed = true;
    m_backend->setMimeData(mime);
    qCInfo(lcClipBridge) << "Clipboard set:" << formats.size() << "sealed formats," << pendingFiles
                         << "files, rendered on paste";
#endif
}

namespace {
std::atomic<bool> s_fetchesAbandoned{false};
} // namespace

void ClipboardBridge::abandonFetches() { s_fetchesAbandoned = true; }

bool ClipboardBridge::fetchesAbandoned() { return s_fetchesAbandoned; }

void ClipboardBridge::clearOwned()
{
#ifdef Q_OS_WIN
    if (deferWhileRendering([this] { clearOwned(); }))
        return;
    if (m_deferred)
        qCInfo(lcClipBridge) << "Dropping a clipboard the service no longer holds";
    releaseDeferred(); // empties the clipboard only if our object is still on it
#elif defined(Q_OS_MACOS)
    if (renderInProgress()) {
        QTimer::singleShot(20, this, &ClipboardBridge::clearOwned);
        return;
    }
    if (ownsClipboard()) {
        qCInfo(lcClipBridge) << "Dropping a clipboard the service no longer holds";
        m_backend->setMimeData(new QMimeData);
        m_ownChangeCount = df::macclipboard::changeCount(); // not reported back (the poll skips it)
        m_lastChangeCount = m_ownChangeCount;
    }
#else
    if (renderInProgress()) {
        QTimer::singleShot(20, this, &ClipboardBridge::clearOwned);
        return;
    }
    if (m_ownsSealed && m_backend->ownsClipboard()) {
        qCInfo(lcClipBridge) << "Dropping a clipboard the service no longer holds";
        m_backend->setMimeData(new QMimeData);
    }
    m_ownsSealed = false;
#endif
}

bool ClipboardBridge::deferWhileRendering(std::function<void()> retry)
{
#ifdef Q_OS_WIN
    if (!clipboardRenderInProgress()) {
        m_pendingApply = nullptr; // this call is newer than whatever still waited
        return false;
    }
    // Only the latest change waits: a move paste's transfer may hold the render for minutes, and
    // the changes made meanwhile must not race each other once it returns (the oldest winning).
    m_pendingApply = std::move(retry);
    if (!m_pendingApplyScheduled) {
        m_pendingApplyScheduled = true;
        QTimer::singleShot(20, this, [this] {
            m_pendingApplyScheduled = false;
            if (std::function<void()> pending = std::exchange(m_pendingApply, nullptr))
                pending(); // defers itself again while the render goes on
        });
    }
    return true;
#else
    Q_UNUSED(retry)
    return false;
#endif
}

bool ClipboardBridge::ownsClipboard() const
{
#ifdef Q_OS_MACOS
    return m_ownChangeCount >= 0 && df::macclipboard::changeCount() == m_ownChangeCount;
#else
    return false;
#endif
}

void ClipboardBridge::releaseDeferred()
{
#ifdef Q_OS_WIN
    if (!m_deferred)
        return;
    auto *object = static_cast<IDataObject *>(m_deferred);
    // Still ours on the clipboard: empty it rather than let OLE flush it when this process ends,
    // which would transfer every file.
    if (OleIsCurrentClipboard(object) == S_OK)
        OleSetClipboard(nullptr);
    object->Release();
    m_deferred = nullptr;
#endif
}

void ClipboardBridge::onChanged()
{
#ifdef Q_OS_WIN
    // Our deferred files on the clipboard: reading them would transfer them.
    if (m_deferred && OleIsCurrentClipboard(static_cast<IDataObject *>(m_deferred)) == S_OK)
        return;
#elif defined(Q_OS_MACOS)
    // Our own sealed items: reading them would ask the service to unseal our own content.
    if (ownsClipboard())
        return;
#else
    // Our own lazy sealed data: reading it would ask the service to unseal our own content.
    if (m_ownsSealed && m_backend->ownsClipboard())
        return;
    m_ownsSealed = false;
#endif
    const QMimeData *mime = m_backend->mimeData();
    if (!mime || !m_enabled)
        return;
    QString text = mime->hasText() ? mime->text() : QString();
    QStringList files;
    QStringList urlLines;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile())
                files.append(url.toLocalFile());
            urlLines.append(url.toString());
        }
    }
    // For copied files Qt synthesises text/plain from the URL list: that is not real text.
    const QStringList textLines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    if (!files.isEmpty() && std::all_of(textLines.begin(), textLines.end(), [&](const QString &line) {
            return urlLines.contains(line.trimmed()) || files.contains(line.trimmed());
        }))
        text.clear();
    // Our own apply() comes back as a change notification: do not report it as new content.
    const QString signature = text + QLatin1Char('\n') + files.join(QLatin1Char('\n'));
    if (m_clock.isValid() && m_clock.elapsed() < m_suppressUntil && signature == m_appliedSignature)
        return;

    QCborMap formats;
    if (!text.isEmpty())
        formats.insert(QStringLiteral("text/plain"), text.toUtf8());
    if (mime->hasHtml())
        formats.insert(QStringLiteral("text/html"), mime->html().toUtf8());
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.isNull() && image.save(&buffer, "PNG"))
            formats.insert(QStringLiteral("image/png"), png);
    }
    if (formats.isEmpty() && files.isEmpty())
        return;
    qCInfo(lcClipBridge) << "Clipboard changed:" << formats.keys().size() << "formats," << files.size() << "files";
    emit changed(formats, files);
}

void ClipboardBridge::apply(const QCborMap &formats, const QStringList &files)
{
#ifdef Q_OS_WIN
    if (deferWhileRendering([this, formats, files] { apply(formats, files); }))
        return; // see applySealed
#else
    if (renderInProgress()) { // see applySealed
        QTimer::singleShot(20, this, [this, formats, files] { apply(formats, files); });
        return;
    }
#endif
    releaseDeferred();
    m_ownsSealed = false;
    auto *mime = new QMimeData;
    const QString text = QString::fromUtf8(formats.value(QStringLiteral("text/plain")).toByteArray());
    if (!text.isEmpty())
        mime->setText(text);
    const QByteArray html = formats.value(QStringLiteral("text/html")).toByteArray();
    if (!html.isEmpty())
        mime->setHtml(QString::fromUtf8(html));
    const QByteArray png = formats.value(QStringLiteral("image/png")).toByteArray();
    if (!png.isEmpty()) {
        QImage image;
        if (image.loadFromData(png, "PNG"))
            mime->setImageData(image);
    }
    if (!files.isEmpty()) {
        QList<QUrl> urls;
        for (const QString &f : files)
            urls.append(QUrl::fromLocalFile(f));
        mime->setUrls(urls); // pasting in a file manager copies the received files
        mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeCopiedFiles(files));
        mime->setData(QStringLiteral("application/x-kde-cutselection"), QByteArray("0"));
    }
    m_appliedSignature = text + QLatin1Char('\n') + files.join(QLatin1Char('\n'));
    if (!m_clock.isValid())
        m_clock.start();
    m_suppressUntil = m_clock.elapsed() + 1000;
    m_backend->setMimeData(mime);
#ifdef Q_OS_MACOS
    m_ownChangeCount = df::macclipboard::changeCount(); // not reported back (the poll skips it)
    m_lastChangeCount = m_ownChangeCount;
#endif
#ifdef Q_OS_WIN
    // Rendered into the system clipboard right away: the content stays pasteable if this process
    // ends, and other processes do not have to call back into it.
    if (const HRESULT hr = OleFlushClipboard(); FAILED(hr))
        qCWarning(lcClipBridge) << "OleFlushClipboard failed:" << Qt::hex << quint32(hr);
#endif
    qCInfo(lcClipBridge) << "Clipboard set:" << formats.size() << "formats," << files.size() << "files";
}
