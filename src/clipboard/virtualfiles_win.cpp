#include "virtualfiles.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>

#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <vector>

Q_LOGGING_CATEGORY(lcVirtualFiles, "deskflower.clipboard.virtual", QtInfoMsg)

namespace {

// Files copied on another device, offered as virtual files: FileGroupDescriptorW (names, sizes,
// folders) and FileContents (one stream per file). Looking at the list costs nothing; the transfer
// happens at the first FileContents request, i.e. when Explorer pastes, and each file is then
// handed over as a stream of the transferred copy.
class VirtualFiles final : public IDataObject
{
public:
    VirtualFiles(const QList<ClipboardBridge::ListedFile> &files, ClipboardBridge::FetchFiles fetch, DWORD effect)
        : m_fetch(std::move(fetch)), m_preferredEffect(effect)
    {
        // Folders first (Explorer creates them), then the files; a file's index is its lindex.
        QStringList folders;
        for (const ClipboardBridge::ListedFile &f : files) {
            const QStringList parts = f.path.split(QLatin1Char('/'));
            for (int i = 1; i < parts.size(); ++i) {
                const QString folder = parts.mid(0, i).join(QLatin1Char('/'));
                if (!folders.contains(folder)) {
                    folders.append(folder);
                    m_entries.append(Entry{folder, 0, true});
                }
            }
        }
        for (const ClipboardBridge::ListedFile &f : files)
            m_entries.append(Entry{f.path, f.size, false});
    }

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

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
    {
        if (!format || !medium)
            return E_INVALIDARG;
        qCDebug(lcVirtualFiles) << "GetData" << formatName(format->cfFormat) << "tymed" << format->tymed << "lindex"
                              << format->lindex;
        if (QueryGetData(format) != S_OK)
            return DV_E_FORMATETC;
        if (m_stored.contains(format->cfFormat)) {
            const Stored stored = m_stored.value(format->cfFormat);
            const QByteArray &bytes = stored.bytes;
            if (stored.tymed == TYMED_ISTREAM) {
                IStream *stream = SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.constData()), UINT(bytes.size()));
                if (!stream)
                    return E_OUTOFMEMORY;
                medium->tymed = TYMED_ISTREAM;
                medium->pstm = stream;
                medium->pUnkForRelease = nullptr;
                return S_OK;
            }
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(qMax<qsizetype>(1, bytes.size())));
            if (!h)
                return E_OUTOFMEMORY;
            memcpy(GlobalLock(h), bytes.constData(), size_t(bytes.size()));
            GlobalUnlock(h);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = h;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }
        if (format->cfFormat == m_dropEffect) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!h)
                return E_OUTOFMEMORY;
            *static_cast<DWORD *>(GlobalLock(h)) = m_preferredEffect;
            GlobalUnlock(h);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = h;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }
        if (format->cfFormat == m_descriptor)
            return descriptor(medium);
        return contents(format->lindex, medium);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override
    {
        if (f)
            qCDebug(lcVirtualFiles) << "QueryGetData" << formatName(f->cfFormat) << "tymed" << f->tymed << "lindex"
                                  << f->lindex << "aspect" << f->dwAspect;
        if (!f || f->dwAspect != DVASPECT_CONTENT)
            return DV_E_FORMATETC;
        if (m_stored.contains(f->cfFormat) && (f->tymed & m_stored.value(f->cfFormat).tymed))
            return S_OK;
        if ((f->cfFormat == m_descriptor || f->cfFormat == m_dropEffect) && (f->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (f->cfFormat == m_contents && (f->tymed & TYMED_ISTREAM))
            return S_OK;
        return DV_E_FORMATETC;
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
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release) override
    {
        // The shell keeps its own data here (drag image, its context, drop description, "Paste
        // Succeeded"...) and reads it back through GetData: stored as bytes, in either medium.
        if (format && medium && medium->tymed == TYMED_HGLOBAL && medium->hGlobal) {
            const SIZE_T size = GlobalSize(medium->hGlobal);
            if (const void *data = GlobalLock(medium->hGlobal)) {
                m_stored.insert(format->cfFormat, Stored{QByteArray(static_cast<const char *>(data), qsizetype(size)),
                                                         TYMED_HGLOBAL});
                GlobalUnlock(medium->hGlobal);
            }
        } else if (format && medium && medium->tymed == TYMED_ISTREAM && medium->pstm) {
            QByteArray bytes;
            LARGE_INTEGER zero = {};
            medium->pstm->Seek(zero, STREAM_SEEK_SET, nullptr);
            char buffer[4096];
            ULONG read = 0;
            while (SUCCEEDED(medium->pstm->Read(buffer, sizeof(buffer), &read)) && read > 0)
                bytes.append(buffer, qsizetype(read));
            m_stored.insert(format->cfFormat, Stored{bytes, TYMED_ISTREAM});
        }
        if (release && medium)
            ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
    {
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        std::vector<FORMATETC> formats = {{CLIPFORMAT(m_descriptor), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                                          {CLIPFORMAT(m_contents), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
                                          {CLIPFORMAT(m_dropEffect), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        for (auto it = m_stored.constBegin(); it != m_stored.constEnd(); ++it)
            formats.push_back({CLIPFORMAT(it.key()), nullptr, DVASPECT_CONTENT, -1, it->tymed});
        return SHCreateStdEnumFmtEtc(UINT(formats.size()), formats.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    static QString formatName(CLIPFORMAT cf)
    {
        wchar_t name[128] = {};
        return GetClipboardFormatNameW(cf, name, 128) > 0 ? QString::fromWCharArray(name) : QString::number(cf);
    }

    struct Entry {
        QString path;
        qint64 size;
        bool folder;
    };

    HRESULT descriptor(STGMEDIUM *medium) const
    {
        const SIZE_T bytes = sizeof(FILEGROUPDESCRIPTORW) + sizeof(FILEDESCRIPTORW) * qMax<SIZE_T>(1, m_entries.size());
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!h)
            return E_OUTOFMEMORY;
        auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(h));
        group->cItems = UINT(m_entries.size());
        for (int i = 0; i < m_entries.size(); ++i) {
            const Entry &e = m_entries.at(i);
            FILEDESCRIPTORW &d = group->fgd[i];
            d.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI | (e.folder ? 0 : FD_FILESIZE);
            d.dwFileAttributes = e.folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            d.nFileSizeHigh = DWORD(quint64(e.size) >> 32);
            d.nFileSizeLow = DWORD(quint64(e.size) & 0xFFFFFFFF);
            const std::wstring name = QDir::toNativeSeparators(e.path).left(MAX_PATH - 1).toStdWString();
            wcsncpy_s(d.cFileName, name.c_str(), _TRUNCATE);
        }
        GlobalUnlock(h);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = h;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT contents(LONG index, STGMEDIUM *medium)
    {
        if (index < 0 || index >= m_entries.size() || m_entries.at(index).folder)
            return DV_E_LINDEX;
        if (m_root.isEmpty()) {
            // The paste starts: transfer everything once, then serve each file from the copy.
            QString error;
            const QStringList items = m_fetch(&error);
            if (items.isEmpty()) {
                qCWarning(lcVirtualFiles) << "Paste failed:" << error;
                return E_FAIL;
            }
            m_root = QFileInfo(items.first()).absolutePath(); // the transferred items sit in one folder
        }
        const QString local = QDir(m_root).filePath(m_entries.at(index).path);
        IStream *stream = nullptr;
        const HRESULT hr = SHCreateStreamOnFileEx(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(local).utf16()),
                                                  STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                                  nullptr, &stream);
        if (FAILED(hr)) {
            qCWarning(lcVirtualFiles) << "Cannot open" << local << Qt::hex << quint32(hr);
            return hr;
        }
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    LONG m_ref = 1;
    ClipboardBridge::FetchFiles m_fetch;
    QList<Entry> m_entries;
    QString m_root;
    const UINT m_descriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    const UINT m_contents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    const UINT m_dropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    DWORD m_preferredEffect = DROPEFFECT_COPY;
    struct Stored {
        QByteArray bytes;
        DWORD tymed = TYMED_HGLOBAL;
    };
    QHash<UINT, Stored> m_stored; // formats set by the shell
};

} // namespace

IDataObject *createVirtualFiles(const QList<ClipboardBridge::ListedFile> &files, ClipboardBridge::FetchFiles fetch,
                                unsigned long preferredEffect)
{
    return new VirtualFiles(files, std::move(fetch), preferredEffect);
}
