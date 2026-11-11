// Streaming paste benchmark, seen from the consumer (not a test: not registered with ctest).
//
// Where does a program like Explorer lose time when it pastes virtual files from the agent's
// streaming object (src/clipboard/virtualfiles_win.cpp with ClipboardBridge::FileStreaming,
// streams from src/clipboard/streamhost_win.cpp)? This process plays the agent: it creates the
// object with a fake paste session (no service, no network: the transfer side is not the
// variable), marshals it (local COM only, as tst_virtualstream's crossProcessPaste) and keeps its
// single-threaded apartment pumping in a Qt event loop. A child process plays Explorer: it
// unmarshals the object in a single-threaded apartment, reads the descriptor, creates every
// folder, then for each file in order asks for FileContents in the media Explorer asks for
// (TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE), takes what it gets (a small complete file
// comes in memory, anything else as a stream read to its end) and writes the file under a scratch
// folder. No clipboard, shell, input or network is touched.
//
// Usage (loopback only, as every test):
//   set DESKFLOWER_BIND_ADDRESS=127.0.0.1
//   set DESKFLOWER_BENCH=1
//   bench_paste <src> <work> [tree] [big] [baseline] [ready] [live] [r64] [r1m] [copyto] [load]
//               [only-load] [istream]
// <src>: made by tests/bench/make_tree.py (<src>/tree and <src>/big.bin). <work>: a scratch
// folder; what a run writes there (<work>/dst, <work>/live) is removed after each run.
// Datasets: tree (many small files), big (one large file); both by default.
// Modes:
//   baseline  the child's copy code reading <src> directly (no COM): a single-threaded copier's floor
//   ready     every file complete from the start, session Succeeded: the pure serving cost
//   live      a writer thread copies <src> into the session folder (<work>/live) at full speed, in
//             descriptor order, starting at the first FileContents request; Succeeded when done
// Consumers (each paste mode is run with each): r64 (IStream::Read with 64 KiB buffers), r1m (1 MiB
// buffers), copyto (IStream::CopyTo into SHCreateStreamOnFileEx's file stream).
// load: also run ready and live with r64 while this process's main thread is busy (a timer doing
// 2 ms of work every 5 ms). only-load: only those runs. istream: the child asks for streams only
// (TYMED_ISTREAM), as the bench did before the agent served small files in memory.
// Without DESKFLOWER_BENCH=1 it prints a line and exits (nothing is run by accident).

#include "clipboardbridge.h"
#include "virtualfiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>

#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Session = ClipboardBridge::PasteSessionBase;
using State = Session::State;
using Clock = std::chrono::steady_clock;

namespace {

void out(const char *fmt, auto... args)
{
    std::printf(fmt, args...);
    std::fflush(stdout);
}

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
double msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

const UINT kDescriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
const UINT kContents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);

FORMATETC format(UINT cf, DWORD tymed, LONG index = -1)
{
    return FORMATETC{CLIPFORMAT(cf), nullptr, DVASPECT_CONTENT, index, tymed};
}

std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }
// Long-path form, so deep trees under a long scratch folder never hit MAX_PATH.
std::wstring longPath(const QString &path) { return L"\\\\?\\" + native(QDir::cleanPath(QDir(path).absolutePath())); }

// p50/p95/max of a sample, in its unit.
struct Dist {
    double p50 = 0, p95 = 0, max = 0, sum = 0;
    size_t n = 0;
};
Dist dist(std::vector<double> v)
{
    Dist d;
    d.n = v.size();
    if (v.empty())
        return d;
    std::sort(v.begin(), v.end());
    d.p50 = v[v.size() / 2];
    d.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))];
    d.max = v.back();
    for (double x : v)
        d.sum += x;
    return d;
}

// ------------------------------------------------------------------------------------------------
// The consumer (the child process, or in-process for the baseline)

enum class Consumer { Read64, Read1M, CopyTo };

Consumer consumerFrom(const QString &s)
{
    if (s == QLatin1String("r1m"))
        return Consumer::Read1M;
    if (s == QLatin1String("copyto"))
        return Consumer::CopyTo;
    return Consumer::Read64;
}

struct ConsumerStats {
    std::vector<double> getDataMs, ttfbMs, readUs, perFileMs;
    double descriptorMs = 0, mkdirS = 0, openCloseS = 0, writeS = 0, releaseS = 0, readS = 0, copyToS = 0;
    qint64 bytes = 0, files = 0, folders = 0, errors = 0, inMemory = 0;
    double wallS = 0;

    void print(const char *label) const
    {
        const double mb = double(bytes) / 1e6;
        out("  %-8s wall %7.2f s  %7.0f files/s  %7.1f MB/s  (%lld files, %lld folders, %.1f MB, %lld errors)\n", label,
            wallS, double(files) / wallS, mb / wallS, files, folders, mb, errors);
        if (!getDataMs.empty()) {
            const Dist g = dist(getDataMs);
            out("           GetData(FileContents) ms p50 %.3f p95 %.3f max %.1f  sum %.2f s   descriptor %.1f ms   "
                "in memory %lld, streams %lld\n",
                g.p50, g.p95, g.max, g.sum / 1000.0, descriptorMs, inMemory, files - inMemory);
        }
        if (!ttfbMs.empty()) {
            const Dist t = dist(ttfbMs);
            out("           time to first byte ms p50 %.3f p95 %.3f max %.1f\n", t.p50, t.p95, t.max);
        }
        if (!readUs.empty()) {
            const Dist r = dist(readUs);
            out("           Read calls %zu (%.2f/file)  us p50 %.0f p95 %.0f max %.0f  sum %.2f s\n", r.n,
                double(r.n) / double(qMax<qint64>(1, files)), r.p50, r.p95, r.max, readS);
        }
        if (copyToS > 0)
            out("           CopyTo sum %.2f s (reads + writes back into this process)\n", copyToS);
        const Dist f = dist(perFileMs);
        out("           per file ms p50 %.3f p95 %.3f max %.1f   sums: mkdir %.2f s, create+close %.2f s, write %.2f s, "
            "stream release %.2f s\n",
            f.p50, f.p95, f.max, mkdirS, openCloseS, writeS, releaseS);
    }
};

// Reads `stream` (or with source != INVALID_HANDLE_VALUE, a local file) to its end into `dst`.
void copyOne(IStream *stream, HANDLE source, qint64 size, const std::wstring &dst, Consumer consumer,
             Clock::time_point gotStream, std::vector<char> &buffer, ConsumerStats &st)
{
    if (consumer == Consumer::CopyTo && stream) {
        auto t = Clock::now();
        IStream *file = nullptr;
        if (FAILED(SHCreateStreamOnFileEx(dst.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
                                          FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &file))) {
            ++st.errors;
            return;
        }
        st.openCloseS += secondsSince(t);
        t = Clock::now();
        ULARGE_INTEGER cb, read = {}, written = {};
        cb.QuadPart = quint64(size);
        const HRESULT hr = stream->CopyTo(file, cb, &read, &written);
        st.copyToS += secondsSince(t);
        if (FAILED(hr) || qint64(written.QuadPart) != size)
            ++st.errors;
        st.bytes += qint64(written.QuadPart);
        t = Clock::now();
        file->Release();
        st.openCloseS += secondsSince(t);
        return;
    }
    auto t = Clock::now();
    HANDLE h = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    st.openCloseS += secondsSince(t);
    if (h == INVALID_HANDLE_VALUE) {
        ++st.errors;
        return;
    }
    const ULONG chunk = ULONG(buffer.size());
    bool first = true;
    qint64 total = 0;
    for (;;) {
        ULONG got = 0;
        t = Clock::now();
        bool ok;
        if (stream) {
            ok = SUCCEEDED(stream->Read(buffer.data(), chunk, &got));
            const auto now = Clock::now();
            st.readUs.push_back(std::chrono::duration<double, std::micro>(now - t).count());
            st.readS += std::chrono::duration<double>(now - t).count();
            if (first) {
                st.ttfbMs.push_back(msBetween(gotStream, now));
                first = false;
            }
        } else {
            DWORD g = 0;
            ok = ReadFile(source, buffer.data(), chunk, &g, nullptr);
            got = g;
            st.readS += secondsSince(t);
        }
        if (!ok) {
            ++st.errors;
            break;
        }
        if (got == 0)
            break;
        t = Clock::now();
        DWORD put = 0;
        if (!WriteFile(h, buffer.data(), got, &put, nullptr) || put != got)
            ++st.errors;
        st.writeS += secondsSince(t);
        total += got;
        if (got < chunk)
            break; // short: the end (a stream is never short before it)
    }
    if (total != size)
        ++st.errors;
    st.bytes += total;
    t = Clock::now();
    CloseHandle(h);
    st.openCloseS += secondsSince(t);
}

struct Entry {
    QString path; // relative, '/'
    qint64 size = 0;
    bool dir = false;
};

// A file served in memory (TYMED_HGLOBAL): its listed size written to dst.
void writeGlobal(HGLOBAL global, qint64 size, const std::wstring &dst, ConsumerStats &st)
{
    auto t = Clock::now();
    HANDLE h = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    st.openCloseS += secondsSince(t);
    if (h == INVALID_HANDLE_VALUE || qint64(GlobalSize(global)) < size) {
        ++st.errors;
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        return;
    }
    t = Clock::now();
    const void *bytes = GlobalLock(global);
    DWORD put = 0;
    if (size > 0 && (!WriteFile(h, bytes, DWORD(size), &put, nullptr) || qint64(put) != size))
        ++st.errors;
    GlobalUnlock(global);
    st.writeS += secondsSince(t);
    st.bytes += put;
    t = Clock::now();
    CloseHandle(h);
    st.openCloseS += secondsSince(t);
}

// The consumer over a data object (Explorer's sequence) into dst. tymed: the media asked for.
ConsumerStats consumeObject(IDataObject *data, const QString &dst, Consumer consumer, DWORD tymed)
{
    ConsumerStats st;
    const auto start = Clock::now();
    FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&fd, &medium))) {
        st.errors = 1;
        return st;
    }
    std::vector<Entry> entries;
    {
        const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
        entries.reserve(group->cItems);
        for (UINT i = 0; i < group->cItems; ++i) {
            const FILEDESCRIPTORW &d = group->fgd[i];
            entries.push_back(Entry{QString::fromWCharArray(d.cFileName),
                                    qint64((quint64(d.nFileSizeHigh) << 32) | d.nFileSizeLow),
                                    (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        }
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    st.descriptorMs = msBetween(start, Clock::now());
    auto t = Clock::now();
    CreateDirectoryW(longPath(dst).c_str(), nullptr);
    for (const Entry &e : entries) {
        if (e.dir) {
            CreateDirectoryW(longPath(dst + QLatin1Char('/') + e.path).c_str(), nullptr);
            ++st.folders;
        }
    }
    st.mkdirS = secondsSince(t);
    std::vector<char> buffer(consumer == Consumer::Read1M ? 1024 * 1024 : 64 * 1024);
    for (int i = 0; i < int(entries.size()); ++i) {
        const Entry &e = entries[size_t(i)];
        if (e.dir)
            continue;
        const auto fileStart = Clock::now();
        FORMATETC fc = format(kContents, tymed, i);
        STGMEDIUM m = {};
        const HRESULT hr = data->GetData(&fc, &m);
        const auto gotStream = Clock::now();
        st.getDataMs.push_back(msBetween(fileStart, gotStream));
        if (SUCCEEDED(hr) && m.tymed == TYMED_HGLOBAL) {
            ++st.inMemory;
            writeGlobal(m.hGlobal, e.size, longPath(dst + QLatin1Char('/') + e.path), st);
        } else if (SUCCEEDED(hr) && m.tymed == TYMED_ISTREAM) {
            copyOne(m.pstm, INVALID_HANDLE_VALUE, e.size, longPath(dst + QLatin1Char('/') + e.path), consumer,
                    gotStream, buffer, st);
        } else {
            if (SUCCEEDED(hr))
                ReleaseStgMedium(&m);
            ++st.errors;
            continue;
        }
        t = Clock::now();
        ReleaseStgMedium(&m);
        st.releaseS += secondsSince(t);
        ++st.files;
        st.perFileMs.push_back(msBetween(fileStart, Clock::now()));
    }
    st.wallS = secondsSince(start);
    return st;
}

// The same copy code from local files (no COM): the floor of a single-threaded copier.
ConsumerStats consumeLocal(const QString &root, const std::vector<Entry> &entries, const QString &dst, Consumer consumer)
{
    ConsumerStats st;
    const auto start = Clock::now();
    CreateDirectoryW(longPath(dst).c_str(), nullptr);
    for (const Entry &e : entries) {
        if (e.dir) {
            CreateDirectoryW(longPath(dst + QLatin1Char('/') + e.path).c_str(), nullptr);
            ++st.folders;
        }
    }
    st.mkdirS = secondsSince(start);
    std::vector<char> buffer(consumer == Consumer::Read1M ? 1024 * 1024 : 64 * 1024);
    for (const Entry &e : entries) {
        if (e.dir)
            continue;
        const auto fileStart = Clock::now();
        HANDLE src = CreateFileW(longPath(root + QLatin1Char('/') + e.path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        st.openCloseS += secondsSince(fileStart);
        if (src == INVALID_HANDLE_VALUE) {
            ++st.errors;
            continue;
        }
        copyOne(nullptr, src, e.size, longPath(dst + QLatin1Char('/') + e.path), consumer, fileStart, buffer, st);
        const auto t = Clock::now();
        CloseHandle(src);
        st.openCloseS += secondsSince(t);
        ++st.files;
        st.perFileMs.push_back(msBetween(fileStart, Clock::now()));
    }
    st.wallS = secondsSince(start);
    return st;
}

// The child: --child <marshalled hex> <consumer> <dst> <tymed asked for>
int runChild(const char *hex, const char *consumerName, const char *dst, const char *tymed)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 2;
    const QByteArray bytes = QByteArray::fromHex(hex);
    IStream *marshalled = SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.constData()), UINT(bytes.size()));
    IDataObject *data = nullptr;
    if (!marshalled || FAILED(CoUnmarshalInterface(marshalled, IID_IDataObject, reinterpret_cast<void **>(&data))))
        return 3;
    marshalled->Release();
    const QString name = QString::fromLatin1(consumerName);
    const ConsumerStats st =
        consumeObject(data, QString::fromLocal8Bit(dst), consumerFrom(name), DWORD(QByteArray(tymed).toULong()));
    data->Release();
    st.print(consumerName);
    CoUninitialize();
    return st.errors == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------
// The agent side (this process)

// Server-side timings: Read time inside the stream (readStarted..readEnded, on the agent's MTA
// threads, includes the polling waits) and GetData time on the agent's STA.
struct ServerStats {
    std::mutex mutex;
    std::vector<double> readUs;
    std::vector<double> getDataMs;
};

class FakeSession final : public Session
{
public:
    FakeSession(QString root, ServerStats *stats) : m_root(std::move(root)), m_stats(stats) {}
    QString root() const override { return m_root; }
    State state() const override { return m_state; }
    void readStarted() override { s_readStart = Clock::now(); }
    void readEnded() override
    {
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - s_readStart).count();
        std::lock_guard<std::mutex> lock(m_stats->mutex);
        m_stats->readUs.push_back(us);
    }
    void touch() override {}
    void set(State state) { m_state = state; }

private:
    static thread_local Clock::time_point s_readStart;
    const QString m_root;
    ServerStats *m_stats;
    std::atomic<State> m_state{State::Running};
};
thread_local Clock::time_point FakeSession::s_readStart;

// Forwards to the agent's object, timing FileContents requests on the agent's thread.
class TimedDataObject final : public IDataObject
{
public:
    TimedDataObject(IDataObject *inner, ServerStats *stats) : m_inner(inner), m_stats(stats) { m_inner->AddRef(); }
    ~TimedDataObject() { m_inner->Release(); }
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
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *f, STGMEDIUM *m) override
    {
        const auto t = Clock::now();
        const HRESULT hr = m_inner->GetData(f, m);
        if (f && f->cfFormat == kContents) {
            std::lock_guard<std::mutex> lock(m_stats->mutex);
            m_stats->getDataMs.push_back(msBetween(t, Clock::now()));
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *f, STGMEDIUM *m) override { return m_inner->GetDataHere(f, m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override { return m_inner->QueryGetData(f); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *a, FORMATETC *b) override
    {
        return m_inner->GetCanonicalFormatEtc(a, b);
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *f, STGMEDIUM *m, BOOL r) override { return m_inner->SetData(f, m, r); }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD d, IEnumFORMATETC **e) override { return m_inner->EnumFormatEtc(d, e); }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c) override
    {
        return m_inner->DAdvise(f, a, s, c);
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD c) override { return m_inner->DUnadvise(c); }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **e) override { return m_inner->EnumDAdvise(e); }

private:
    LONG m_ref = 1;
    IDataObject *m_inner;
    ServerStats *m_stats;
};

// The dataset as the service lists it: depth first, a folder before what it holds.
void walk(const QString &root, const QString &rel, std::vector<Entry> &out)
{
    QDir dir(rel.isEmpty() ? root : root + QLatin1Char('/') + rel);
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QFileInfo &f : files)
        out.push_back(Entry{rel.isEmpty() ? f.fileName() : rel + QLatin1Char('/') + f.fileName(), f.size(), false});
    const QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QFileInfo &d : dirs) {
        const QString sub = rel.isEmpty() ? d.fileName() : rel + QLatin1Char('/') + d.fileName();
        out.push_back(Entry{sub, 0, true});
        walk(root, sub, out);
    }
}

// The fake transfer: copies the entries in order into `to`, at full speed, writing in 1 MiB chunks
// shared with readers (the way the service's receiver writes, never preallocated).
void copyTree(const QString &from, const QString &to, const std::vector<Entry> &entries)
{
    CreateDirectoryW(longPath(to).c_str(), nullptr);
    std::vector<char> buffer(1024 * 1024);
    for (const Entry &e : entries) {
        const std::wstring target = longPath(to + QLatin1Char('/') + e.path);
        if (e.dir) {
            CreateDirectoryW(target.c_str(), nullptr);
            continue;
        }
        HANDLE src = CreateFileW(longPath(from + QLatin1Char('/') + e.path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE dst = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        for (;;) {
            DWORD got = 0, put = 0;
            if (!ReadFile(src, buffer.data(), DWORD(buffer.size()), &got, nullptr) || got == 0)
                break;
            WriteFile(dst, buffer.data(), got, &put, nullptr);
        }
        CloseHandle(dst);
        CloseHandle(src);
    }
}

bool removeTree(const QString &path)
{
    QDir dir(path);
    return !dir.exists() || dir.removeRecursively();
}

void printServer(ServerStats &s)
{
    std::lock_guard<std::mutex> lock(s.mutex);
    const Dist g = dist(s.getDataMs);
    const Dist r = dist(s.readUs);
    out("           agent side: GetData(FileContents) on its STA ms p50 %.3f p95 %.3f max %.1f sum %.2f s; stream "
        "reads (in the MTA, incl. waits) %zu, us p50 %.0f p95 %.0f max %.0f sum %.2f s\n",
        g.p50, g.p95, g.max, g.sum / 1000.0, r.n, r.p50, r.p95, r.max, r.sum / 1e6);
}

// One paste: the object over `sessionRoot`, the child consuming it into <work>/dst.
void runPaste(const QString &label, const QString &src, const std::vector<Entry> &entries, const QString &work,
              bool live, const QString &consumer, bool load, DWORD tymed)
{
    out("%s\n", qPrintable(label));
    const QString dst = work + QStringLiteral("/dst");
    const QString liveRoot = work + QStringLiteral("/live");
    removeTree(dst);
    removeTree(liveRoot);
    ServerStats stats;
    auto session = std::make_shared<FakeSession>(live ? liveRoot : src, &stats);
    if (!live)
        session->set(State::Succeeded);
    std::thread writer;
    Clock::time_point writerStart, writerEnd;
    ClipboardBridge::FileStreaming streaming;
    streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
        if (live && !writer.joinable()) {
            writerStart = Clock::now();
            writer = std::thread([&] {
                copyTree(src, liveRoot, entries);
                writerEnd = Clock::now();
                session->set(State::Succeeded);
            });
        }
        return session;
    };
    QList<ClipboardBridge::ListedFile> listed;
    listed.reserve(qsizetype(entries.size()));
    for (const Entry &e : entries)
        listed.append(ClipboardBridge::ListedFile{e.path, e.size, e.dir});
    IDataObject *object =
        createSealedData({}, {}, listed, int(listed.size()), {}, false, DROPEFFECT_COPY, std::move(streaming));
    auto *timed = new TimedDataObject(object, &stats);
    object->Release();

    IStream *marshalStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &marshalStream);
    if (FAILED(CoMarshalInterface(marshalStream, IID_IDataObject, timed, MSHCTX_LOCAL, nullptr, MSHLFLAGS_NORMAL))) {
        out("  cannot marshal the object\n");
        marshalStream->Release();
        timed->Release();
        return;
    }
    HGLOBAL global = nullptr;
    GetHGlobalFromStream(marshalStream, &global);
    ULARGE_INTEGER end = {};
    LARGE_INTEGER zero = {};
    marshalStream->Seek(zero, STREAM_SEEK_CUR, &end);
    const QByteArray marshalled(static_cast<const char *>(GlobalLock(global)), qsizetype(end.QuadPart));
    GlobalUnlock(global);

    // The agent's thread: pumping (and, with load, busy 2 ms of every 5).
    QTimer busy;
    busy.setInterval(5);
    QObject::connect(&busy, &QTimer::timeout, [] {
        const auto until = Clock::now() + std::chrono::milliseconds(2);
        while (Clock::now() < until) {
        }
    });
    if (load)
        busy.start();
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    QEventLoop loop;
    QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&child, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    const auto start = Clock::now();
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--child"), QString::fromLatin1(marshalled.toHex()), consumer, dst, QString::number(tymed)});
    loop.exec();
    const double wall = secondsSince(start);
    busy.stop();
    if (child.exitCode() != 0)
        out("  child exit code %d\n", child.exitCode());
    printServer(stats);
    if (writer.joinable()) {
        writer.join();
        out("           live writer: started %.0f ms after the child, took %.2f s\n", msBetween(start, writerStart),
            std::chrono::duration<double>(writerEnd - writerStart).count());
    }
    out("           parent wall incl. child start %.2f s\n", wall);
    // The object goes (the child unmarshalled the reference and released its proxy).
    marshalStream->Release();
    timed->Release();
    removeTree(dst);
    removeTree(liveRoot);
}

void runBaseline(const QString &label, const QString &src, const std::vector<Entry> &entries, const QString &work,
                 Consumer consumer, const char *name)
{
    out("%s\n", qPrintable(label));
    const QString dst = work + QStringLiteral("/dst");
    removeTree(dst);
    const ConsumerStats st = consumeLocal(src, entries, dst, consumer);
    st.print(name);
    removeTree(dst);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 6 && qstrcmp(argv[1], "--child") == 0)
        return runChild(argv[2], argv[3], argv[4], argv[5]);
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("DESKFLOWER_BENCH") != QLatin1String("1")) {
        out("bench_paste: set DESKFLOWER_BENCH=1 (and DESKFLOWER_BIND_ADDRESS=127.0.0.1) to run it\n");
        return 0;
    }
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        out("usage: bench_paste <src> <work> [tree] [big] [baseline] [ready] [live] [r64] [r1m] [copyto] [load] "
            "[only-load] [istream]\n");
        return 2;
    }
    if (FAILED(OleInitialize(nullptr))) {
        out("OleInitialize failed\n");
        return 2;
    }
    const QString src = QDir(args.at(1)).absolutePath();
    const QString work = QDir(args.at(2)).absolutePath();
    QDir().mkpath(work);
    const QStringList opts = args.mid(3);
    auto has = [&](const char *o) { return opts.contains(QLatin1String(o)); };
    auto any = [&](std::initializer_list<const char *> list) {
        for (const char *o : list)
            if (has(o))
                return true;
        return false;
    };
    const bool allSets = !any({"tree", "big"});
    const bool allModes = !any({"baseline", "ready", "live"});
    const bool allConsumers = !any({"r64", "r1m", "copyto"});
    const bool onlyLoad = has("only-load");
    // What the child asks FileContents for: Explorer's media, or streams only.
    const DWORD tymed = has("istream") ? DWORD(TYMED_ISTREAM) : DWORD(TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE);
    QStringList consumers;
    for (const char *c : {"r64", "r1m", "copyto"})
        if (allConsumers || has(c))
            consumers << QLatin1String(c);

    struct Set {
        const char *name;
        std::vector<Entry> entries;
    };
    std::vector<Set> sets;
    if (allSets || has("tree")) {
        Set s{"tree", {}};
        s.entries.push_back(Entry{QStringLiteral("tree"), 0, true});
        walk(src, QStringLiteral("tree"), s.entries);
        sets.push_back(std::move(s));
    }
    if (allSets || has("big")) {
        Set s{"big", {}};
        s.entries.push_back(Entry{QStringLiteral("big.bin"), QFileInfo(src + QStringLiteral("/big.bin")).size(), false});
        sets.push_back(std::move(s));
    }
    for (const Set &s : sets) {
        qint64 bytes = 0, files = 0;
        for (const Entry &e : s.entries) {
            bytes += e.size;
            files += e.dir ? 0 : 1;
        }
        out("== dataset %s: %lld files, %zu entries, %.1f MB\n", s.name, files, s.entries.size(), double(bytes) / 1e6);
        if (!onlyLoad && (allModes || has("baseline"))) {
            runBaseline(QStringLiteral("-- %1 baseline (local files, no COM), 64 KiB").arg(QLatin1String(s.name)), src,
                        s.entries, work, Consumer::Read64, "base64");
            runBaseline(QStringLiteral("-- %1 baseline (local files, no COM), 1 MiB").arg(QLatin1String(s.name)), src,
                        s.entries, work, Consumer::Read1M, "base1m");
        }
        for (const bool live : {false, true}) {
            if (!(allModes || has(live ? "live" : "ready")))
                continue;
            if (!onlyLoad) {
                for (const QString &c : consumers)
                    runPaste(QStringLiteral("-- %1 %2 %3").arg(QLatin1String(s.name), QLatin1String(live ? "live" : "ready"), c),
                             src, s.entries, work, live, c, false, tymed);
            }
            if (has("load") || onlyLoad)
                runPaste(QStringLiteral("-- %1 %2 r64, agent thread busy 2 ms of every 5")
                             .arg(QLatin1String(s.name), QLatin1String(live ? "live" : "ready")),
                         src, s.entries, work, live, QStringLiteral("r64"), true, tymed);
        }
    }
    OleUninitialize();
    return 0;
}
