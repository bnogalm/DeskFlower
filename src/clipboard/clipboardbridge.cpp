#include "clipboardbridge.h"

#ifdef Q_OS_WIN
#include "virtualfiles.h"
#endif

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_WIN
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

Q_LOGGING_CATEGORY(lcClipBridge, "deskflower.clipboard.local", QtInfoMsg)

#ifdef Q_OS_WIN
namespace {
// Files copied on another device, offered as CF_HDROP with delayed rendering: nothing is transferred
// until an application (Explorer, on paste) asks for the data. "Preferred DropEffect" says copy.
class DeferredFiles final : public IDataObject
{
public:
    explicit DeferredFiles(ClipboardBridge::FetchFiles fetch) : m_fetch(std::move(fetch)) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&m_ref)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG ref = InterlockedDecrement(&m_ref);
        if (ref == 0)
            delete this;
        return ULONG(ref);
    }

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
    {
        if (!format || !medium)
            return E_INVALIDARG;
        if (!supported(format))
            return DV_E_FORMATETC;
        if (format->cfFormat == m_dropEffect) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!h)
                return E_OUTOFMEMORY;
            *static_cast<DWORD *>(GlobalLock(h)) = DROPEFFECT_COPY;
            GlobalUnlock(h);
            return fill(medium, h);
        }
        if (m_files.isEmpty()) {
            QString error;
            m_files = m_fetch(&error); // the transfer happens here, while the paste waits
            if (m_files.isEmpty()) {
                qCWarning(lcClipBridge) << "Paste failed:" << error;
                return E_FAIL;
            }
        }
        std::wstring list;
        for (const QString &f : std::as_const(m_files))
            list += f.toStdWString() + L'\0';
        list += L'\0';
        const SIZE_T bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!h)
            return E_OUTOFMEMORY;
        auto *drop = static_cast<DROPFILES *>(GlobalLock(h));
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        memcpy(reinterpret_cast<char *>(drop) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        GlobalUnlock(h);
        return fill(medium, h);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
    {
        return supported(format) ? S_OK : DV_E_FORMATETC; // never transfers anything
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *in, FORMATETC *out) override
    {
        if (!out)
            return E_INVALIDARG;
        if (in)
            *out = *in;
        out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *medium, BOOL release) override
    {
        // Explorer reports "Performed DropEffect" / "Paste Succeeded": accepted and ignored.
        if (release && medium)
            ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
    {
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        FORMATETC formats[2] = {{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                                {CLIPFORMAT(m_dropEffect), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(2, formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    bool supported(const FORMATETC *f) const
    {
        return f && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT
               && (f->cfFormat == CF_HDROP || f->cfFormat == m_dropEffect);
    }
    static HRESULT fill(STGMEDIUM *medium, HGLOBAL h)
    {
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = h;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    LONG m_ref = 1;
    ClipboardBridge::FetchFiles m_fetch;
    QStringList m_files;
    const UINT m_dropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
};
} // namespace
#endif

ClipboardBridge::ClipboardBridge(QObject *parent) : QObject(parent)
{
    // Debounce bursts of change notifications (some applications set formats one by one).
    m_debounce.setInterval(150);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &ClipboardBridge::onChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (m_enabled)
            m_debounce.start();
    });
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
    releaseDeferred();
    auto *object = new DeferredFiles(std::move(fetch));
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

void ClipboardBridge::applyVirtualFiles(const QList<ListedFile> &files, FetchFiles fetch)
{
#ifdef Q_OS_WIN
    releaseDeferred();
    IDataObject *object = createVirtualFiles(files, std::move(fetch));
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
#endif
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
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
    if (QDateTime::currentMSecsSinceEpoch() < m_suppressUntil && signature == m_appliedSignature)
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
    releaseDeferred();
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
    }
    m_appliedSignature = text + QLatin1Char('\n') + files.join(QLatin1Char('\n'));
    m_suppressUntil = QDateTime::currentMSecsSinceEpoch() + 1000;
    QGuiApplication::clipboard()->setMimeData(mime);
#ifdef Q_OS_WIN
    // Rendered into the system clipboard right away: the content stays pasteable if this process
    // ends, and other processes do not have to call back into it.
    if (const HRESULT hr = OleFlushClipboard(); FAILED(hr))
        qCWarning(lcClipBridge) << "OleFlushClipboard failed:" << Qt::hex << quint32(hr);
#endif
    qCInfo(lcClipBridge) << "Clipboard set:" << formats.size() << "formats," << files.size() << "files";
}
