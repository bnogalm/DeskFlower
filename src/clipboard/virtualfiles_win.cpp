#include "virtualfiles.h"
#include "streamhost_win.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QLoggingCategory>
#include <QTimer>

#include <ole2.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

Q_LOGGING_CATEGORY(lcVirtualFiles, "deskflower.clipboard.virtual", QtInfoMsg)

namespace {

// Asked through QueryInterface to tell our own objects from anyone else's (dropReport).
const GUID kVirtualFilesIid = {0x6b1f3c2e, 0x8d4a, 0x4f61, {0x9a, 0x2e, 0x51, 0x7c, 0x0d, 0x93, 0xb4, 0x27}};

int s_pumping = 0; // fetchWhilePumping calls in progress (on the agent's main thread)

// A move paste's hand-out serves the reads of one paste: Explorer may read CF_HDROP more than once
// for it, but only this soon after it was given (setMoveReuseWindowForTests). Short: a second
// paste this soon would be given the same items while the first may still be moving them.
std::atomic<int> s_moveReuseMs{2000};

// The entries (the items themselves, and everything under the folders among them) of a move
// paste's hand-out, links not followed; -1 if an item is missing. A paste that moved part of them
// away (cancelled halfway through a move to another volume) leaves fewer. longest: the longest
// path among them, if asked.
qint64 countEntries(const QStringList &items, qsizetype *longest = nullptr)
{
    qint64 count = 0;
    auto note = [longest](const QString &path) {
        if (longest)
            *longest = qMax(*longest, path.size());
    };
    for (const QString &item : items) {
        const QFileInfo info(item);
        if (!info.exists() && !info.isSymLink())
            return -1;
        ++count;
        note(item);
        if (info.isDir() && !info.isSymLink()) {
            QDirIterator it(item, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                note(it.next());
                ++count;
            }
        }
    }
    return count;
}

// Whether none of a hand-out's items is where it was given any more (the paste moved them all).
bool allGone(const QStringList &items)
{
    return std::none_of(items.cbegin(), items.cend(), [](const QString &item) {
        const QFileInfo info(item);
        return info.exists() || info.isSymLink();
    });
}

// Hand-outs of move paste objects that went (the clipboard changed) while their items were still
// there: Explorer holds only their paths, and may still be moving them (to another volume, for
// minutes). Their sessions are kept a while longer (their folders stay pinned in the service, out
// of the prune), consumed once their items are gone, let go after s_moveLingerMs. On the
// application's thread only; dropped when it quits.
struct Lingering {
    std::shared_ptr<ClipboardBridge::PasteSessionBase> session;
    QStringList items;
    QElapsedTimer since;
};
std::vector<Lingering> s_lingering;
std::atomic<int> s_moveLingerMs{30 * 60 * 1000};
constexpr int kLingerCheckMs = 2000;
constexpr size_t kMaxLingering = 3; // the oldest beyond this are let go (left to the prune)
bool s_lingerScheduled = false;
bool s_quitting = false;

void checkLingering()
{
    for (auto it = s_lingering.begin(); it != s_lingering.end();) {
        if (allGone(it->items)) {
            qCInfo(lcVirtualFiles) << "A paste moved the files of a replaced clipboard: their transfer folder is let go";
            it->session->consumed();
            it = s_lingering.erase(it);
        } else if (it->since.elapsed() >= s_moveLingerMs) {
            it = s_lingering.erase(it); // left to the prune of older copies
        } else {
            ++it;
        }
    }
    if (s_lingering.empty() || s_lingerScheduled)
        return;
    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        s_lingering.clear();
        return;
    }
    s_lingerScheduled = true;
    QTimer::singleShot(kLingerCheckMs, app, [] {
        s_lingerScheduled = false;
        checkLingering();
    });
}

// Keeps `session` (its items not all gone) as above; false if it cannot (the application quits):
// let go now.
bool linger(std::shared_ptr<ClipboardBridge::PasteSessionBase> session, const QStringList &items)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app || s_quitting || s_moveLingerMs <= 0)
        return false;
    static bool watchingQuit = false;
    if (!watchingQuit) {
        watchingQuit = true;
        QObject::connect(app, &QCoreApplication::aboutToQuit, [] {
            s_quitting = true;
            s_lingering.clear();
        });
    }
    Lingering l;
    l.session = std::move(session);
    l.items = items;
    l.since.start();
    s_lingering.push_back(std::move(l));
    while (s_lingering.size() > kMaxLingering)
        s_lingering.erase(s_lingering.begin());
    checkLingering();
    return true;
}

// Whether the user did anything (a key, a click, the mouse) in the last `ms` milliseconds. A
// paste always follows input; a clipboard history or monitor reading the clipboard on its own
// does not.
bool recentUserInput(DWORD ms)
{
    LASTINPUTINFO info = {};
    info.cbSize = sizeof(info);
    return GetLastInputInfo(&info) && GetTickCount() - info.dwTime <= ms;
}

// Files copied on another device, offered as virtual files: FileGroupDescriptorW (names, sizes,
// folders) and FileContents (one stream per file). Looking at the list costs nothing; the transfer
// happens at the first FileContents request, i.e. when Explorer pastes, and each file is then
// handed over as a stream of the transferred copy.
class VirtualFiles final : public IDataObject, public IDataObjectAsyncCapability
{
public:
    VirtualFiles(const QList<ClipboardBridge::ListedFile> &files, ClipboardBridge::FetchFiles fetch, DWORD effect,
                 bool asyncDrop = false)
        : m_fetch(std::move(fetch)), m_preferredEffect(effect), m_asyncCapable(asyncDrop), m_async(asyncDrop)
    {
        // A descriptor holds a relative path of at most MAX_PATH - 1 characters. Truncating a
        // longer one would build a different tree from the one transferred (contents() looks files
        // up by their full path), so then the whole set is offered as a deferred CF_HDROP of the
        // transferred copies instead, which has no such limit.
        if (const qsizetype tooLong = setEntries(files); tooLong > 0) {
            qCWarning(lcVirtualFiles) << tooLong << "paths too long for virtual files: offering the transferred copies";
            m_deferredCount = int(qMax<qsizetype>(1, files.size()));
        }
    }

    // The entries of the descriptor: folders first, parents before children (Explorer creates
    // them, empty ones included), then the files; an entry's index is its lindex. Returns how many
    // paths are too long for a descriptor (MAX_PATH - 1); with any, no entry is kept.
    qsizetype setEntries(const QList<ClipboardBridge::ListedFile> &files)
    {
        QList<Entry> entries;
        entries.reserve(files.size());
        QSet<QString> folders;
        auto addFolder = [&](const QString &folder) {
            if (!folders.contains(folder)) {
                folders.insert(folder);
                entries.append(Entry{folder, 0, true});
            }
        };
        for (const ClipboardBridge::ListedFile &f : files) {
            const QStringList parts = f.path.split(QLatin1Char('/'));
            for (int i = 1; i < parts.size(); ++i)
                addFolder(parts.mid(0, i).join(QLatin1Char('/')));
            if (f.dir)
                addFolder(f.path);
        }
        for (const ClipboardBridge::ListedFile &f : files) {
            if (!f.dir)
                entries.append(Entry{f.path, f.size, false});
        }
        const qsizetype tooLong = std::count_if(entries.cbegin(), entries.cend(),
                                                [](const Entry &e) { return e.path.size() > MAX_PATH - 1; });
        if (tooLong > 0)
            entries.clear();
        m_entries = std::move(entries);
        m_descriptorBytes.clear(); // built again at the next read
        return tooLong;
    }

    // Pasting while the files arrive (see ClipboardBridge::FileStreaming). onDemand: no list was
    // announced; the descriptor and the contents are offered anyway, and the list is asked for at
    // the first descriptor read.
    void setStreaming(ClipboardBridge::FileStreaming streaming, bool onDemand)
    {
        if (streaming.isEmpty() || (onDemand ? !streaming.list : m_entries.isEmpty()))
            return; // nothing to stream (no list source, or listed paths too long: whole transfer)
        m_streaming = std::move(streaming);
        m_onDemand = onDemand;
        m_deferredCount = 0;
    }

    // The move paste (ClipboardBridge::FileStreaming::move): a deferred CF_HDROP with Preferred
    // DropEffect = move instead of the descriptor and the contents. count: the entries offered.
    void setMove(std::function<std::shared_ptr<ClipboardBridge::PasteSessionBase>(QString *)> move, int count)
    {
        if (!move)
            return;
        m_move = std::move(move);
        m_entries.clear();
        m_descriptorBytes.clear();
        m_onDemand = false;
        m_streaming = {};
        m_deferredCount = qMax(1, count);
        m_preferredEffect = DROPEFFECT_MOVE;
    }

    ~VirtualFiles()
    {
        // The folders of hand-outs a paste emptied go now; the others are kept a while longer (a
        // paste may still be moving out of them, see linger), then pruned later with the older
        // received copies.
        if (!m_move)
            return;
        releaseConsumedHandOuts();
        if (m_handOut)
            m_earlierHandOuts.push_back(std::move(*m_handOut));
        for (HandOut &h : m_earlierHandOuts) {
            if (h.session)
                linger(std::move(h.session), h.items);
        }
    }

    struct SealedFmt {
        QString mime;
        UINT cf;
    };

    // Gated content (a guest's clipboard): rendered plaintext is kept this long (applications ask
    // several times per paste), then dropped, so a clipboard manager polling later goes through
    // the service's grant again; a read with no user input this recently is refused without
    // asking, and a refusal is remembered this long (readers retry in bursts).
    static constexpr qint64 kRenderedKeptMs = 5000;
    static constexpr DWORD kPasteInputMs = 1500;
    static constexpr qint64 kRefusalKeptMs = 200;

    // Clipboard objects (not drags): a transfer runs on a worker thread while this thread keeps
    // pumping messages (fetchWhilePumping), and the object offers asynchronous extraction so that a
    // drop target that supports it does not wait on its own UI thread.
    void setClipboardObject()
    {
        m_clipboardObject = true;
        m_async = TRUE;
    }

    // Files known only by their count (too many to list): offered as a deferred CF_HDROP that
    // transfers them at the first request.
    void setDeferredCount(int count)
    {
        if (m_entries.isEmpty())
            m_deferredCount = qMax(0, count);
    }

    // Clipboard history, the cloud clipboard and clipboard monitors are asked to leave this object
    // alone: they read what arrives on the clipboard on their own, and for a guest's content or for
    // files on another device (whose data must move only for a paste) they would be refused or,
    // worse, start a transfer nobody asked for.
    void markNoHistory()
    {
        const QByteArray zero(sizeof(DWORD), '\0');
        m_stored.insert(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"),
                        Stored{zero, TYMED_HGLOBAL});
        m_stored.insert(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), Stored{zero, TYMED_HGLOBAL});
        m_stored.insert(RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), Stored{zero, TYMED_HGLOBAL});
    }

    // Sealed text/HTML/image formats, decrypted lazily from `unseal` when an application pastes.
    void setSealed(const QList<ClipboardBridge::SealedFormat> &formats, ClipboardBridge::UnsealFormat unseal, bool gated)
    {
        m_unseal = std::move(unseal);
        m_gated = gated;
        if (gated)
            markNoHistory(); // a guest's clipboard is rendered only for a paste
        for (const ClipboardBridge::SealedFormat &f : formats) {
            if (f.mime == QLatin1String("text/plain"))
                m_sealed.append(SealedFmt{f.mime, CF_UNICODETEXT});
            else if (f.mime == QLatin1String("text/html"))
                m_sealed.append(SealedFmt{f.mime, m_html});
            else if (f.mime == QLatin1String("image/png")) {
                m_sealed.append(SealedFmt{f.mime, CF_DIB});
                m_sealed.append(SealedFmt{f.mime, m_png});
            }
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject || riid == kVirtualFilesIid) {
            *ppv = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IDataObjectAsyncCapability && (m_asyncCapable || m_clipboardObject)) {
            *ppv = static_cast<IDataObjectAsyncCapability *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // --- IDataObjectAsyncCapability (drags): the target extracts on a thread of its own, calling
    // back into this object on the drag's thread, which waits for EndOperation (waitForAsyncDrop).
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL async) override
    {
        m_async = async;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *async) override
    {
        if (!async)
            return E_POINTER;
        *async = m_async;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override
    {
        qCInfo(lcVirtualFiles) << "The drop target extracts the files on its own thread";
        m_operationStarted = true;
        m_inOperation = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL *inOperation) override
    {
        if (!inOperation)
            return E_POINTER;
        *inOperation = m_inOperation.load();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx *, DWORD effects) override
    {
        qCInfo(lcVirtualFiles) << "The drop target ended its extraction: result" << Qt::hex << quint32(result)
                               << "effect" << Qt::dec << effects;
        m_operationResult = result;
        m_operationEffect = effects;
        m_operationEnded = true;
        m_inOperation = false;
        if (m_move && SUCCEEDED(result) && (effects & DROPEFFECT_MOVE))
            moveReportedFromAnyThread();
        return S_OK;
    }
    // dropReport.
    bool asyncStarted() const { return m_operationStarted; }
    bool asyncPending() const { return m_inOperation; }
    bool asyncEnded(HRESULT *result, DWORD *effect) const
    {
        *result = m_operationResult;
        *effect = m_operationEffect;
        return m_operationEnded;
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
        for (const SealedFmt &s : m_sealed) {
            if (format->cfFormat == s.cf)
                return sealedData(s, medium);
        }
        if (format->cfFormat == CF_HDROP)
            return m_move ? moveDrop(medium) : deferredDrop(medium);
        if (format->cfFormat == m_descriptor) {
            if (m_onDemand && !m_listed && !listEntries())
                return E_FAIL;
            m_descriptorRead.start(); // the paste's own moment (see pasteSession)
            if (!m_firstDescriptorRead.isValid())
                m_firstDescriptorRead.start(); // for the paste's stats only
            return descriptor(medium);
        }
        return contents(format->lindex, format->tymed, medium);
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
        for (const SealedFmt &s : m_sealed) {
            if (f->cfFormat == s.cf && (f->tymed & TYMED_HGLOBAL))
                return S_OK;
        }
        if (f->cfFormat == m_dropEffect && (f->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (m_deferredCount > 0 && f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL))
            return S_OK; // never transfers anything
        if (hasFileList() && f->cfFormat == m_descriptor && (f->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (hasFileList() && f->cfFormat == m_contents && (f->tymed & contentsTymed()))
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
                // A move paste object keeps offering its own Preferred DropEffect (move), and does
                // not hand a paste's results to the next paste: those are only noted.
                if (!m_move || (!isDropResult(format->cfFormat) && format->cfFormat != m_dropEffect))
                    m_stored.insert(format->cfFormat, Stored{QByteArray(static_cast<const char *>(data), qsizetype(size)),
                                                             TYMED_HGLOBAL});
                bool moved = false;
                if (size >= sizeof(DWORD) && isDropResult(format->cfFormat)) {
                    const DWORD effect = *static_cast<const DWORD *>(data);
                    qCInfo(lcVirtualFiles) << "The drop target reports" << formatName(format->cfFormat) << effect;
                    // Any of them: Explorer moving the items itself (an optimized move) reports the
                    // move as the logical effect only. What is consumed is still only what is gone.
                    moved = (effect & DROPEFFECT_MOVE) != 0;
                }
                GlobalUnlock(medium->hGlobal);
                if (moved && m_move)
                    moveReported();
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
        std::vector<FORMATETC> formats;
        // Text/HTML/image come first, so an application that takes the first format it understands
        // gets the real content rather than the file list.
        for (const SealedFmt &s : m_sealed)
            formats.push_back({CLIPFORMAT(s.cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        if (hasFileList()) {
            formats.push_back({CLIPFORMAT(m_descriptor), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
            formats.push_back({CLIPFORMAT(m_contents), nullptr, DVASPECT_CONTENT, -1, contentsTymed()});
        } else if (m_deferredCount > 0) {
            formats.push_back({CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        }
        formats.push_back({CLIPFORMAT(m_dropEffect), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
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
    bool isDropResult(UINT cf) const
    {
        return cf == m_performed || cf == m_logicalPerformed || cf == m_pasteSucceeded;
    }
    // The descriptor and contents are offered: listed, or listed on demand.
    bool hasFileList() const { return !m_entries.isEmpty() || m_onDemand; }
    // The medium FileContents is offered in, and must be asked for in: a stream, always. A
    // streaming paste also serves a small file already complete in memory (an HGLOBAL, see
    // streamingContents), but only as an extra in a request that also takes a stream (Explorer
    // asks with a fixed HGLOBAL | ISTREAM | ISTORAGE, whatever is enumerated): GetData can serve
    // any file as a stream, and only some as an HGLOBAL (small, complete), so offering HGLOBAL
    // on its own would promise what a reader that takes only HGLOBAL cannot always get, and its
    // first request would open the paste session (the whole transfer) for nothing. Such a
    // request is refused here, in QueryGetData, before anything is opened. The whole transfer
    // (drags, older services) serves streams of the local copy only.
    static constexpr DWORD contentsTymed() { return TYMED_ISTREAM; }
    // The largest file a streaming paste serves in memory (an HGLOBAL) when it is complete.
    static constexpr qint64 kInMemoryMax = 1024 * 1024;

    // How long a reader of the descriptor waits for the list of an object announced without it
    // (Explorer waits on its own UI thread meanwhile).
    static constexpr qint64 kListTimeoutMs = 60 * 1000;

    // The list of an object announced without it, asked for (names and sizes only) while this
    // thread keeps pumping. Not remembered when it fails: the next read asks again.
    bool listEntries()
    {
        struct ListJob {
            ClipboardBridge::FileStreaming streaming;
            QList<ClipboardBridge::ListedFile> files;
            QString error;
        };
        auto job = std::make_shared<ListJob>();
        job->streaming = m_streaming;
        qCInfo(lcVirtualFiles) << "The target reads the list of the copied files: asking for it (no contents move)";
        const bool done =
            runWhilePumping([job] { job->files = job->streaming.list(&job->error); }, kListTimeoutMs);
        if (!done) {
            qCWarning(lcVirtualFiles) << "No list of the copied files in time";
            return false;
        }
        if (job->files.isEmpty()) {
            qCWarning(lcVirtualFiles) << "No list of the copied files:" << job->error;
            return false;
        }
        if (setEntries(job->files) > 0) {
            qCWarning(lcVirtualFiles) << "Copied paths too long for virtual files";
            return false;
        }
        m_listed = true;
        qCInfo(lcVirtualFiles) << "Listed" << m_entries.size() << "entries";
        return true;
    }

    // The paste session the streams of this object read from: the current one while it is usable
    // (running, or done), else a new one (the transfer starts; refused without a paste).
    std::shared_ptr<ClipboardBridge::PasteSessionBase> pasteSession(QString *error)
    {
        if (m_session && m_session->terminal() && !m_session->succeeded()) {
            m_session.reset(); // failed, cancelled or given up: a new paste asks again
            m_sessionStats.reset();
        }
        if (m_session)
            return m_session;
        struct OpenJob {
            ClipboardBridge::FileStreaming streaming;
            std::shared_ptr<ClipboardBridge::PasteSessionBase> session;
            QString error;
        };
        auto job = std::make_shared<OpenJob>();
        job->streaming = m_streaming;
        // How long ago the descriptor was read: a file manager reads it at the paste and creates
        // every folder before it reads the first file, so the paste's key or click may be well
        // before this request (the service also counts a press made around the descriptor read).
        const qint64 listedAgo = m_descriptorRead.isValid() ? m_descriptorRead.elapsed() : -1;
        const qint64 sinceFirstListed = m_firstDescriptorRead.isValid() ? m_firstDescriptorRead.elapsed() : -1;
        qCInfo(lcVirtualFiles) << "The target reads the file contents: transferring" << m_entries.size()
                               << "entries, served while they arrive";
        runWhilePumping([job, listedAgo] { job->session = job->streaming.open(&job->error, listedAgo); });
        if (!job->session) {
            *error = job->error;
            return {};
        }
        // A request that came in meanwhile (this thread pumped) may have opened one too: the
        // usable one stays, the other one goes with its streams.
        if (!m_session || (m_session->terminal() && !m_session->succeeded())) {
            m_session = job->session;
            m_sessionStats = std::make_shared<df::streamhost::PasteStats>(sinceFirstListed);
            m_firstDescriptorRead.invalidate(); // the next paste counts from its own first read
        }
        return job->session;
    }

    struct Entry {
        QString path;
        qint64 size;
        bool folder;
    };

    // The descriptor's bytes (FILEGROUPDESCRIPTORW), built once per list: about 600 bytes an
    // entry, so about 11 MB for 19,000 entries, which a file manager may read more than once.
    // Built again only after the entries changed (setEntries, setMove clear it).
    const QByteArray &descriptorBytes()
    {
        if (!m_descriptorBytes.isEmpty())
            return m_descriptorBytes;
        const qsizetype bytes =
            qsizetype(sizeof(FILEGROUPDESCRIPTORW) + sizeof(FILEDESCRIPTORW) * qMax<SIZE_T>(1, m_entries.size()));
        QByteArray built(bytes, '\0');
        auto *group = reinterpret_cast<FILEGROUPDESCRIPTORW *>(built.data());
        group->cItems = UINT(m_entries.size());
        for (int i = 0; i < m_entries.size(); ++i) {
            const Entry &e = m_entries.at(i);
            FILEDESCRIPTORW &d = group->fgd[i];
            d.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI | (e.folder ? 0 : FD_FILESIZE);
            d.dwFileAttributes = e.folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            d.nFileSizeHigh = DWORD(quint64(e.size) >> 32);
            d.nFileSizeLow = DWORD(quint64(e.size) & 0xFFFFFFFF);
            // Never truncated: longer paths make the constructor fall back to CF_HDROP.
            const std::wstring name = QDir::toNativeSeparators(e.path).toStdWString();
            wcsncpy_s(d.cFileName, name.c_str(), _TRUNCATE);
        }
        m_descriptorBytes = std::move(built);
        return m_descriptorBytes;
    }

    HRESULT descriptor(STGMEDIUM *medium)
    {
        const QByteArray &bytes = descriptorBytes();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(bytes.size()));
        if (!h)
            return E_OUTOFMEMORY;
        memcpy(GlobalLock(h), bytes.constData(), size_t(bytes.size()));
        GlobalUnlock(h);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = h;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT contents(LONG index, DWORD tymed, STGMEDIUM *medium)
    {
        if (index < 0 || index >= m_entries.size() || m_entries.at(index).folder)
            return DV_E_LINDEX;
        if (!m_streaming.isEmpty())
            return streamingContents(m_entries.at(index), tymed, medium);
        if (m_root.isEmpty()) {
            // The paste starts: transfer everything once, then serve each file from the copy. (A
            // call that comes in while this one waits joins the same transfer in the service.)
            qCInfo(lcVirtualFiles) << "The target reads the file contents: transferring" << m_entries.size() << "entries";
            QString error;
            const QStringList items = fetch(&error);
            if (items.isEmpty()) {
                logFetchFailure(error);
                return E_FAIL;
            }
            qCInfo(lcVirtualFiles) << "Transferred: serving the contents";
            m_root = QFileInfo(items.first()).absolutePath(); // the transferred items sit in one folder
        }
        const QString local = QDir(m_root).filePath(m_entries.at(index).path);
        IStream *stream = nullptr;
        const HRESULT hr = SHCreateStreamOnFileEx(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(local).utf16()),
                                                  STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                                  nullptr, &stream);
        if (FAILED(hr)) {
            qCWarning(lcVirtualFiles) << "Cannot open a received file" << Qt::hex << quint32(hr);
            return hr;
        }
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    // A file of a streaming paste (the list may be replaced while this thread pumps: `entry` is a
    // copy). Served while the transfer runs, never waiting on this thread:
    // - a small file (at most kInMemoryMax) already complete on disk, to a reader that takes an
    //   HGLOBAL (Explorer asks for HGLOBAL | ISTREAM | ISTORAGE): its bytes in memory, read here at
    //   once. A file manager usually lags behind the transfer, so most small files are complete
    //   when asked for, and each one saves the reader a stream's Read and Release calls (a round
    //   trip each, between processes);
    // - anything else (larger, not complete yet, empty, or a reader that takes no HGLOBAL): a
    //   stream in the agent's multithreaded apartment that waits for the bytes asked for
    //   (Explorer's reads never come to this thread).
    // A request must take a stream (contentsTymed: QueryGetData refuses one that takes only an
    // HGLOBAL, before any session is opened); the HGLOBAL is only the cheaper answer when it can be.
    HRESULT streamingContents(const Entry entry, DWORD tymed, STGMEDIUM *medium)
    {
        if (!(tymed & TYMED_ISTREAM))
            return DV_E_TYMED; // never reached through GetData (QueryGetData); never opens the session
        const bool inMemory = (tymed & TYMED_HGLOBAL) && entry.size > 0 && entry.size <= kInMemoryMax;
        QElapsedTimer took;
        took.start();
        QString error;
        const std::shared_ptr<ClipboardBridge::PasteSessionBase> session = pasteSession(&error);
        if (!session) {
            logFetchFailure(error);
            return E_FAIL;
        }
        // The session this object keeps, and its stats (not one opened by a request that came in
        // while this one pumped, which goes with its streams).
        const std::shared_ptr<df::streamhost::PasteStats> stats = session == m_session ? m_sessionStats : nullptr;
        auto done = [&](df::streamhost::PasteStats::Served served, HRESULT hr) {
            if (stats)
                stats->contents(took.nsecsElapsed(), served);
            return hr;
        };
        session->touch();
        const QString path = QDir(session->root()).filePath(entry.path);
        if (inMemory) {
            if (HGLOBAL h = df::streamhost::readCompleteFile(path, entry.size)) {
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = h;
                medium->pUnkForRelease = nullptr;
                return done(df::streamhost::PasteStats::Served::HGlobal, S_OK);
            }
        }
        IStream *stream = nullptr;
        const HRESULT hr = df::streamhost::createProgressiveStream(session, path, entry.size, &stream, stats);
        if (FAILED(hr) || !stream) {
            qCWarning(lcVirtualFiles) << "Cannot serve a file while it arrives" << Qt::hex << quint32(hr);
            return done(df::streamhost::PasteStats::Served::Failed, E_FAIL);
        }
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;
        medium->pUnkForRelease = nullptr;
        return done(df::streamhost::PasteStats::Served::Stream, S_OK);
    }

    // A reader that is not a paste (kNotAPaste: a clipboard monitor, refused by design) is only
    // noted; anything else is a paste that failed.
    static void logFetchFailure(const QString &error)
    {
        if (error == QLatin1String(ClipboardBridge::kNotAPaste))
            qCInfo(lcVirtualFiles) << "Not transferred:" << error;
        else
            qCWarning(lcVirtualFiles) << "Paste failed:" << error;
    }

    // CF_HDROP for files known only by their count: the transfer happens here, while the paste
    // waits, and the list names the transferred top-level items.
    HRESULT deferredDrop(STGMEDIUM *medium)
    {
        if (m_deferredCount <= 0 || !m_fetch)
            return DV_E_FORMATETC;
        if (m_dropped.isEmpty()) {
            QString error;
            m_dropped = fetch(&error);
            if (m_dropped.isEmpty()) {
                logFetchFailure(error);
                return E_FAIL;
            }
        }
        return hdrop(medium, m_dropped);
    }

    // --- The move paste ---------------------------------------------------------------------
    // Each paste (a read of CF_HDROP) is given a copy of its own, transferred for it into a new
    // folder, and Explorer moves the items out of it. A hand-out serves the reads of its paste
    // only: again within s_moveReuseMs, provided nothing was taken from it; any later read is
    // another paste, which gets a new transfer (the earlier copy may be gone, or still being moved
    // by its paste, or half moved by a paste cancelled midway). The session of every hand-out
    // stays here (its folder pinned in the service, never pruned) until its items are all gone
    // (consumed: the service removes the emptied folder), until kMaxEarlierHandOuts newer ones
    // came (let go: left to the service's prune of older copies), or until this object goes (kept
    // a while longer, see linger). A move report (SetData, EndOperation) cannot say which paste
    // it is from: it only makes the hand-outs be looked at for what is gone.
    struct HandOut {
        std::shared_ptr<ClipboardBridge::PasteSessionBase> session;
        QStringList items;  // the top-level items given
        qint64 entries = 0; // countEntries(items) when given
        QElapsedTimer since;
    };
    // Earlier hand-outs kept pinned besides the current one (pastes cancelled before they moved
    // anything leave theirs): beyond this the oldest is let go, so repeated pastes do not pile up
    // whole copies on the disk.
    static constexpr size_t kMaxEarlierHandOuts = 2;

    HRESULT moveDrop(STGMEDIUM *medium)
    {
        releaseConsumedHandOuts();
        if (m_moveFetching) {
            // Another paste of this object while the first one's transfer runs (this thread pumps
            // meanwhile): both would be given the same items to move. Refused; the user pastes
            // again once the first is done.
            qCWarning(lcVirtualFiles) << "A paste of these files is still transferring: this one is refused";
            return E_FAIL;
        }
        if (m_handOut && m_handOut->since.elapsed() < s_moveReuseMs
            && countEntries(m_handOut->items) == m_handOut->entries)
            return hdrop(medium, m_handOut->items); // the same paste reading again
        if (m_handOut) {
            m_earlierHandOuts.push_back(std::move(*m_handOut));
            m_handOut.reset();
        }
        while (m_earlierHandOuts.size() > kMaxEarlierHandOuts) {
            qCInfo(lcVirtualFiles) << "An earlier paste's copy is let go (left to the prune of older copies)";
            m_earlierHandOuts.erase(m_earlierHandOuts.begin());
        }
        struct MoveJob {
            std::function<std::shared_ptr<ClipboardBridge::PasteSessionBase>(QString *)> open;
            std::shared_ptr<ClipboardBridge::PasteSessionBase> session;
            QString error;
            QStringList items;
            qint64 entries = -1;
            qsizetype longest = 0;
        };
        auto job = std::make_shared<MoveJob>();
        job->open = m_move;
        qCInfo(lcVirtualFiles) << "The target reads the file list: transferring the files for a move paste";
        m_moveFetching = true;
        runWhilePumping([job] {
            job->session = job->open(&job->error);
            if (!job->session)
                return;
            // Waited for here, with DeskFlower's window showing the progress (and Cancel): a read
            // in progress, so the session is never given up meanwhile.
            job->session->readStarted();
            while (!job->session->terminal() && !ClipboardBridge::fetchesAbandoned())
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            job->session->readEnded();
            if (!job->session->succeeded())
                return;
            const QFileInfoList top = QDir(job->session->root())
                                          .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                                             | QDir::System,
                                                         QDir::Name);
            for (const QFileInfo &item : top)
                job->items.append(item.absoluteFilePath());
            job->entries = countEntries(job->items, &job->longest);
        });
        m_moveFetching = false;
        if (!job->session) {
            logFetchFailure(job->error);
            return E_FAIL;
        }
        if (!job->session->succeeded()) {
            if (job->session->state() == ClipboardBridge::PasteSessionBase::State::Cancelled)
                qCInfo(lcVirtualFiles) << "The move paste's transfer was cancelled";
            else
                qCWarning(lcVirtualFiles) << "The move paste's transfer did not complete";
            return E_FAIL; // its session goes: the service cancels or forgets the transfer
        }
        if (job->items.isEmpty() || job->entries <= 0) {
            qCWarning(lcVirtualFiles) << "The move paste's transfer left nothing to move";
            return E_FAIL;
        }
        qCInfo(lcVirtualFiles) << "Transferred: handing" << job->items.size() << "items over to be moved";
        if (job->longest >= MAX_PATH)
            qCWarning(lcVirtualFiles) << "Some paths of the move paste are" << job->longest
                                      << "characters long: Explorer may leave those items behind";
        HandOut out;
        out.session = std::move(job->session);
        out.items = std::move(job->items);
        out.entries = job->entries;
        out.since.start();
        m_handOut = std::move(out);
        return hdrop(medium, m_handOut->items);
    }

    // The target reported a move (on this thread): the hand-outs whose items are all gone are
    // consumed, now and a few times shortly after (the report may come while the last items are
    // still being moved; the object is held meanwhile).
    void moveReported()
    {
        releaseConsumedHandOuts();
        QCoreApplication *app = QCoreApplication::instance();
        if (!app || !hasUnconsumedHandOuts())
            return;
        for (const int ms : {500, 2000, 6000}) {
            AddRef();
            QTimer::singleShot(ms, app, [this] {
                releaseConsumedHandOuts();
                Release();
            });
        }
    }
    bool hasUnconsumedHandOuts() const { return m_handOut.has_value() || !m_earlierHandOuts.empty(); }
    // The same from the target's own thread (EndOperation may come on it): handed to this
    // object's thread.
    void moveReportedFromAnyThread()
    {
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return; // the next read checks what is gone anyway
        AddRef();
        QMetaObject::invokeMethod(
            app,
            [this] {
                moveReported();
                Release();
            },
            Qt::QueuedConnection);
    }
    // Hand-outs whose items a paste moved away: the service is told (it removes the emptied
    // folder, and the next paste transfers again) and their sessions go.
    void releaseConsumedHandOuts()
    {
        auto consume = [](HandOut &h) {
            qCInfo(lcVirtualFiles) << "The paste moved the files: their transfer folder is let go";
            h.session->consumed();
            h.session.reset();
        };
        if (m_handOut && allGone(m_handOut->items)) {
            consume(*m_handOut);
            m_handOut.reset();
        }
        for (auto it = m_earlierHandOuts.begin(); it != m_earlierHandOuts.end();) {
            if (allGone(it->items)) {
                consume(*it);
                it = m_earlierHandOuts.erase(it);
            } else {
                ++it;
            }
        }
    }

    // A CF_HDROP naming `paths`.
    static HRESULT hdrop(STGMEDIUM *medium, const QStringList &paths)
    {
        std::wstring list;
        for (const QString &f : paths)
            list += QDir::toNativeSeparators(f).toStdWString() + L'\0';
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
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = h;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    static HRESULT fillGlobal(STGMEDIUM *medium, const QByteArray &bytes)
    {
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

    // Decrypts the format behind `s` (cached a few seconds) and renders it as the Windows clipboard
    // payload.
    HRESULT sealedData(const SealedFmt &s, STGMEDIUM *medium)
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_gated) {
            // A guest's clipboard: what was rendered serves only a few seconds.
            for (auto it = m_renderedSealed.begin(); it != m_renderedSealed.end();) {
                if (now - it->at >= kRenderedKeptMs) {
                    it->bytes.fill('\0');
                    it = m_renderedSealed.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (m_renderedSealed.contains(s.cf))
            return fillGlobal(medium, m_renderedSealed.value(s.cf).bytes);
        if (!m_unseal)
            return E_FAIL;
        if (m_gated) {
            // Refused here, cheaply, without asking the service: a read with no user input just
            // before (a clipboard history or monitor reading on its own), or one right after a
            // refusal (readers retry in bursts).
            if (now - m_refusedAt.value(s.cf, -kRefusalKeptMs) < kRefusalKeptMs)
                return E_FAIL;
            if (!recentUserInput(kPasteInputMs)) {
                m_refusedAt.insert(s.cf, now);
                return E_FAIL;
            }
        }
        QString error;
        const QByteArray plain = m_unseal(s.mime, &error); // blocks while the paste waits
        if (plain.isEmpty()) {
            qCInfo(lcVirtualFiles) << "Paste refused for" << s.mime << ":" << error;
            if (m_gated)
                m_refusedAt.insert(s.cf, now);
            return E_FAIL;
        }
        QByteArray payload;
        if (s.cf == CF_UNICODETEXT) {
            // Windows text has CRLF line ends (Qt's writer does the same conversion).
            QString text = QString::fromUtf8(plain);
            text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
            text.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
            const std::wstring w = text.toStdWString();
            payload = QByteArray(reinterpret_cast<const char *>(w.c_str()), qsizetype((w.size() + 1) * sizeof(wchar_t)));
        } else if (s.cf == m_html) {
            payload = htmlClipboard(QString::fromUtf8(plain));
        } else if (s.cf == m_png) {
            payload = plain; // the PNG bytes as they are
        } else if (s.cf == CF_DIB) {
            payload = pngToDib(plain);
            if (payload.isEmpty())
                return E_FAIL;
        } else {
            return DV_E_FORMATETC;
        }
        m_renderedSealed.insert(s.cf, Rendered{payload, now});
        return fillGlobal(medium, payload);
    }

    // The "HTML Format" clipboard payload: a header with byte offsets, then the HTML. A document
    // that already carries the fragment markers (copied from a browser or Office on Windows, Qt
    // hands it over whole) is used as it is, with the offsets taken from the markers; otherwise the
    // HTML is wrapped in them, as Qt's writer does.
    static QByteArray htmlClipboard(const QString &html)
    {
        const QByteArray pre = "<!--StartFragment-->";
        const QByteArray post = "<!--EndFragment-->";
        QByteArray doc = html.toUtf8();
        if (!doc.contains(pre) || !doc.contains(post))
            doc = "<html><body>" + pre + doc + post + "</body></html>";
        // Offsets are counted after the header is rendered: pad each number to 10 digits so the
        // header length does not depend on the values.
        auto pad = [](int n) { return QByteArray::number(n).rightJustified(10, '0'); };
        const QByteArray fixed = QByteArray("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                                            "StartFragment:0000000000\r\nEndFragment:0000000000\r\n");
        const int startHtml = fixed.size();
        const int startFragment = startHtml + int(doc.indexOf(pre)) + pre.size();
        const int endFragment = startHtml + int(doc.lastIndexOf(post));
        const int endHtml = startHtml + doc.size();
        const QByteArray header = "Version:0.9\r\nStartHTML:" + pad(startHtml) + "\r\nEndHTML:" + pad(endHtml)
                                  + "\r\nStartFragment:" + pad(startFragment) + "\r\nEndFragment:" + pad(endFragment)
                                  + "\r\n";
        return header + doc;
    }

    // CF_DIB is a BITMAPINFOHEADER plus pixel data: a BMP file without its 14-byte file header.
    static QByteArray pngToDib(const QByteArray &png)
    {
        QImage image;
        if (!image.loadFromData(png, "PNG"))
            return {};
        QByteArray bmp;
        QBuffer buffer(&bmp);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "BMP"))
            return {};
        return bmp.size() > 14 ? bmp.mid(14) : QByteArray();
    }

    struct Rendered {
        QByteArray bytes;
        qint64 at = 0;
    };
    // The files of a paste or a drop: on a clipboard object, transferred while this thread keeps
    // pumping its messages; on a drag (its own thread), blocking as before.
    QStringList fetch(QString *error) const
    {
        if (!m_fetch)
            return {};
        return m_clipboardObject ? fetchWhilePumping(m_fetch, error) : m_fetch(error);
    }

    QList<SealedFmt> m_sealed;
    ClipboardBridge::UnsealFormat m_unseal;
    bool m_gated = false;
    bool m_clipboardObject = false;
    QHash<UINT, Rendered> m_renderedSealed;
    QHash<UINT, qint64> m_refusedAt; // gated: when a format was last refused
    const UINT m_html = RegisterClipboardFormatW(L"HTML Format");
    const UINT m_png = RegisterClipboardFormatW(L"PNG");

    LONG m_ref = 1;
    ClipboardBridge::FetchFiles m_fetch;
    QList<Entry> m_entries;
    ClipboardBridge::FileStreaming m_streaming; // empty: the whole transfer at the first contents
    bool m_onDemand = false;                    // listed through m_streaming.list...
    bool m_listed = false;                      // ...which answered
    std::shared_ptr<ClipboardBridge::PasteSessionBase> m_session;
    std::shared_ptr<df::streamhost::PasteStats> m_sessionStats; // m_session's (logged once all let go)
    QElapsedTimer m_descriptorRead; // since the descriptor was last given (invalid: never)
    // Since the descriptor was first given after the last session opened (invalid: not since):
    // the stats' delay to the paste's first contents request only.
    QElapsedTimer m_firstDescriptorRead;
    QByteArray m_descriptorBytes; // descriptorBytes(), empty until built
    QString m_root;
    int m_deferredCount = 0; // files offered as a deferred CF_HDROP (no list)
    QStringList m_dropped;   // ...and the transferred top-level items, once pasted
    // The move paste (setMove): its request, the current hand-out, earlier ones still pinned.
    std::function<std::shared_ptr<ClipboardBridge::PasteSessionBase>(QString *)> m_move;
    std::optional<HandOut> m_handOut;
    std::vector<HandOut> m_earlierHandOuts;
    bool m_moveFetching = false;
    const UINT m_descriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    const UINT m_contents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    const UINT m_dropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    DWORD m_preferredEffect = DROPEFFECT_COPY;
    struct Stored {
        QByteArray bytes;
        DWORD tymed = TYMED_HGLOBAL;
    };
    QHash<UINT, Stored> m_stored; // formats set by the shell
    const UINT m_performed = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
    const UINT m_logicalPerformed = RegisterClipboardFormatW(CFSTR_LOGICALPERFORMEDDROPEFFECT);
    const UINT m_pasteSucceeded = RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED);
    // IDataObjectAsyncCapability (drags only).
    const bool m_asyncCapable = false;
    BOOL m_async = FALSE;
    // Atomic: a target in this process may call from its own thread without marshalling.
    std::atomic_bool m_operationStarted = false;
    std::atomic_bool m_inOperation = false;
    std::atomic_bool m_operationEnded = false; // set last: result and effect are valid then
    HRESULT m_operationResult = S_OK;
    DWORD m_operationEffect = DROPEFFECT_NONE;
};

// A DWORD the target set on the object (GetData returns what SetData stored), if any.
bool storedEffect(IDataObject *data, const wchar_t *format, DWORD *effect)
{
    FORMATETC f = {CLIPFORMAT(RegisterClipboardFormatW(format)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&f, &medium)))
        return false;
    bool found = false;
    if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
        if (const auto *value = static_cast<const DWORD *>(GlobalLock(medium.hGlobal))) {
            *effect = *value;
            found = true;
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return found;
}

VirtualFiles *ownObject(IDataObject *data)
{
    IDataObject *own = nullptr;
    if (!data || FAILED(data->QueryInterface(kVirtualFilesIid, reinterpret_cast<void **>(&own))) || !own)
        return nullptr;
    own->Release(); // `data` still holds it
    return static_cast<VirtualFiles *>(own);
}

} // namespace

IDataObject *createVirtualFiles(const QList<ClipboardBridge::ListedFile> &files, ClipboardBridge::FetchFiles fetch,
                                unsigned long preferredEffect, bool asyncDrop)
{
    return new VirtualFiles(files, std::move(fetch), preferredEffect, asyncDrop);
}

DropReport dropReport(IDataObject *data)
{
    DropReport report;
    if (!data)
        return report;
    VirtualFiles *own = ownObject(data);
    if (own && own->asyncStarted()) {
        // In async mode only the end of the operation counts: the shell sets "Logical Performed
        // DropEffect" as it starts, before reading anything, even when the transfer then fails.
        report.asyncPending = own->asyncPending();
        HRESULT result = S_OK;
        DWORD endEffect = DROPEFFECT_NONE;
        if (own->asyncEnded(&result, &endEffect)) {
            report.asyncFailed = FAILED(result);
            if (!report.asyncFailed)
                report.effect = endEffect;
        }
        return report;
    }
    // Otherwise what the target set once done: the performed effect (never the logical one, set
    // up front, see above) and "Paste Succeeded". Virtual files cannot be moved by an optimised
    // move (which reports none as performed): the target writes them itself.
    DWORD effect = DROPEFFECT_NONE;
    if (storedEffect(data, CFSTR_PERFORMEDDROPEFFECT, &effect))
        report.effect = effect;
    if (storedEffect(data, CFSTR_PASTESUCCEEDED, &effect))
        report.effect |= effect;
    return report;
}

bool waitForAsyncDrop(IDataObject *data, unsigned long timeoutMs, const std::function<bool()> &abort)
{
    // Ours, or the shell's (the originals dragged back where they are): both say whether the
    // target is still at work on its own thread.
    IDataObjectAsyncCapability *async = nullptr;
    if (!data || FAILED(data->QueryInterface(IID_PPV_ARGS(&async))) || !async)
        return true;
    auto pending = [async] {
        BOOL in = FALSE;
        return SUCCEEDED(async->InOperation(&in)) && in;
    };
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool ended = true;
    while (pending()) {
        if ((abort && abort()) || GetTickCount64() >= deadline) {
            ended = false;
            break;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 200, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    async->Release();
    return ended;
}

IDataObject *createSealedData(const QList<ClipboardBridge::SealedFormat> &formats, ClipboardBridge::UnsealFormat unseal,
                              const QList<ClipboardBridge::ListedFile> &files, int pendingFiles,
                              ClipboardBridge::FetchFiles fetchFiles, bool gated, unsigned long preferredEffect,
                              ClipboardBridge::FileStreaming streaming)
{
    auto *object = new VirtualFiles(files, std::move(fetchFiles), preferredEffect);
    object->setClipboardObject();
    if (files.isEmpty())
        object->setDeferredCount(pendingFiles);
    auto move = std::move(streaming.move);
    if (!files.isEmpty() || pendingFiles > 0) {
        object->setStreaming(std::move(streaming), files.isEmpty());
        object->setMove(std::move(move), qMax(int(files.size()), pendingFiles));
    }
    object->setSealed(formats, std::move(unseal), gated);
    if (!files.isEmpty() || pendingFiles > 0)
        object->markNoHistory(); // files on another device move only for a paste
    return object;
}

bool runWhilePumping(std::function<void()> work, long long timeoutMs)
{
    struct Job {
        std::atomic<bool> done{false};
        std::function<void()> work;
    };
    auto job = std::make_shared<Job>();
    job->work = std::move(work);
    std::thread worker([job] {
        job->work();
        job->done = true;
    });
    // The worker only talks to the service over a connection of its own; this thread goes on
    // serving its window, its IPC connection (the transfer's progress is shown meanwhile) and COM.
    ++s_pumping;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(30);
    QElapsedTimer elapsed;
    elapsed.start();
    bool timedOut = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (job->done) {
            loop.quit();
        } else if (timeoutMs >= 0 && elapsed.elapsed() >= timeoutMs) {
            timedOut = true;
            loop.quit();
        }
    });
    poll.start();
    if (!job->done)
        loop.exec();
    --s_pumping;
    if (job->done) {
        worker.join();
        return true;
    }
    if (!timedOut) {
        // The loop ended early: the application quits (an update is being installed). The work
        // gives up at its next check instead of holding the process for the whole transfer.
        qCInfo(lcVirtualFiles) << "Quitting during a paste: its transfer is abandoned";
        ClipboardBridge::abandonFetches();
    }
    if (timeoutMs < 0) {
        worker.join();
        return true;
    }
    worker.detach(); // it owns what it uses (job)
    return false;
}

QStringList fetchWhilePumping(const ClipboardBridge::FetchFiles &fetch, QString *error)
{
    struct Result {
        QStringList files;
        QString error;
    };
    auto result = std::make_shared<Result>();
    runWhilePumping([result, fetch] { result->files = fetch(&result->error); });
    if (error)
        *error = result->error;
    return result->files;
}

bool clipboardRenderInProgress() { return s_pumping > 0; }

void setMoveReuseWindowForTests(int ms) { s_moveReuseMs = ms; }

void setMoveLingerForTests(int ms) { s_moveLingerMs = ms; }
