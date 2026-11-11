#include "streamhost_win.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

Q_LOGGING_CATEGORY(lcStreamHost, "deskflower.clipboard.virtual", QtInfoMsg)

namespace {

using Session = ClipboardBridge::PasteSessionBase;
using Clock = std::chrono::steady_clock;

qint64 nsBetween(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Waits about `ms` milliseconds. Sleep's resolution is the system timer's (15.6 ms unless some
// program raised it), so a Read that looks again after "1 ms" would really wait 16, and after 20 ms
// about 31: a high-resolution waitable timer (Windows 10 1803 and later) waits what it is asked.
// One per thread, kept for the thread's life (COM's threads are pooled); Sleep where there is none.
void pause(DWORD ms)
{
    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer()
        {
            if (handle)
                CloseHandle(handle);
        }
    };
    thread_local Timer timer;
    if (timer.handle) {
        LARGE_INTEGER due = {};
        due.QuadPart = -qint64(ms) * 10000; // relative, in 100 ns units
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)
            && WaitForSingleObject(timer.handle, ms + 1000) == WAIT_OBJECT_0)
            return;
    }
    Sleep(ms);
}

// A Read in progress, for the session's abandon rule (never given up while one waits).
class ActiveRead
{
public:
    explicit ActiveRead(Session &session) : m_session(session) { m_session.readStarted(); }
    ~ActiveRead() { m_session.readEnded(); }
    ActiveRead(const ActiveRead &) = delete;
    ActiveRead &operator=(const ActiveRead &) = delete;

private:
    Session &m_session;
};

// One file of a streaming paste, read while the transfer writes it (see streamhost_win.h). Lives in
// the multithreaded apartment: its methods run on any of its threads, so the position and the
// handle are under a mutex (a Read that waits holds it: calls on one stream are sequential anyway).
class ProgressiveStream final : public IStream
{
public:
    ProgressiveStream(std::shared_ptr<Session> session, QString path, qint64 size,
                      std::shared_ptr<df::streamhost::PasteStats> stats, qint64 pos = 0)
        : m_session(std::move(session)), m_path(std::move(path)), m_size(qMax<qint64>(0, size)),
          m_stats(std::move(stats)), m_pos(pos)
    {
    }
    ~ProgressiveStream()
    {
        if (m_file != INVALID_HANDLE_VALUE)
            CloseHandle(m_file);
    }

    // How long a Read waits between two looks at the file: short at first (a file manager usually
    // lags behind the transfer, and a file that is not quite there yet is often there a moment
    // later), then longer, at most the last step; back to the first once the file grew.
    static constexpr DWORD kSlicesMs[] = {1, 2, 5, 10, 20};
    // What CopyTo reads and writes at a time (one buffer per call).
    static constexpr ULONG kCopyChunk = 1024 * 1024;

    // --- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *ppv = static_cast<IStream *>(this);
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

    // --- ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override
    {
        if (pcbRead)
            *pcbRead = 0;
        if (!pv && cb > 0)
            return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        return readLocked(static_cast<char *>(pv), cb, pcbRead);
    }
    HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *pcbWritten) override
    {
        if (pcbWritten)
            *pcbWritten = 0;
        return E_NOTIMPL; // read only
    }

    // --- IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPosition) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        qint64 base = 0;
        switch (origin) {
        case STREAM_SEEK_SET:
            base = 0;
            break;
        case STREAM_SEEK_CUR:
            base = m_pos;
            break;
        case STREAM_SEEK_END:
            base = m_size;
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        const qint64 target = base + move.QuadPart;
        if (target < 0)
            return STG_E_INVALIDFUNCTION;
        m_pos = target;
        if (newPosition)
            newPosition->QuadPart = quint64(target);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream *target, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead,
                                     ULARGE_INTEGER *pcbWritten) override
    {
        if (pcbRead)
            pcbRead->QuadPart = 0;
        if (pcbWritten)
            pcbWritten->QuadPart = 0;
        if (!target)
            return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<char> buffer(size_t(qMax<quint64>(1, qMin<quint64>(cb.QuadPart, kCopyChunk))));
        quint64 left = cb.QuadPart;
        quint64 totalRead = 0;
        quint64 totalWritten = 0;
        HRESULT hr = S_OK;
        while (left > 0) {
            const ULONG chunk = ULONG(qMin<quint64>(left, buffer.size()));
            ULONG got = 0;
            hr = readLocked(buffer.data(), chunk, &got);
            if (FAILED(hr) || got == 0)
                break;
            totalRead += got;
            ULONG put = 0;
            hr = target->Write(buffer.data(), got, &put);
            totalWritten += put;
            if (FAILED(hr))
                break;
            if (put < got) {
                hr = STG_E_MEDIUMFULL;
                break;
            }
            left -= got;
        }
        if (pcbRead)
            pcbRead->QuadPart = totalRead;
        if (pcbWritten)
            pcbWritten->QuadPart = totalWritten;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD flags) override
    {
        if (!stat)
            return STG_E_INVALIDPOINTER;
        ZeroMemory(stat, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = quint64(m_size); // the announced size, also while the file grows
        stat->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
        if (!(flags & STATFLAG_NONAME)) {
            const std::wstring name = QFileInfo(m_path).fileName().toStdWString();
            auto *copy = static_cast<wchar_t *>(CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t)));
            if (!copy)
                return STG_E_INSUFFICIENTMEMORY;
            memcpy(copy, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
            stat->pwcsName = copy;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream **out) override
    {
        if (!out)
            return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        // Called on a thread of this apartment: the clone lives here too.
        *out = new ProgressiveStream(m_session, m_path, m_size, m_stats, m_pos);
        return S_OK;
    }

private:
    // The file's current size on disk, opening it once it exists; -1 while it does not (or cannot
    // be opened yet). Opened with delete sharing: a failed or cancelled transfer removes what it
    // wrote at once, also while a stream still holds a file (the read then fails on the session's
    // state, never on a missing file).
    qint64 onDisk()
    {
        if (m_file == INVALID_HANDLE_VALUE) {
            m_file = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(m_path).utf16()),
                                 GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_file == INVALID_HANDLE_VALUE)
                return -1; // not there yet
        }
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(m_file, &size))
            return -1;
        return size.QuadPart;
    }

    // The session ended without the file: its handle goes at once (nothing keeps it open).
    HRESULT failed(HRESULT hr)
    {
        if (m_file != INVALID_HANDLE_VALUE) {
            CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
        }
        return hr;
    }

    // Read with the mutex held: returns once min(cb, size - pos) bytes were read (never fewer
    // before the end), or with the error that ended the wait.
    HRESULT readLocked(char *out, ULONG cb, ULONG *pcbRead)
    {
        if (pcbRead)
            *pcbRead = 0;
        if (cb == 0 || m_pos >= m_size)
            return S_OK; // the end (an empty file is there at once)
        const qint64 want = qMin<qint64>(qint64(cb), m_size - m_pos);
        const ActiveRead active(*m_session);
        const Clock::time_point started = Clock::now();
        size_t slice = 0;
        qint64 seen = -2; // the size at the last look (-1: no file yet)
        for (;;) {
            if (ClipboardBridge::fetchesAbandoned())
                return failed(STG_E_READFAULT); // the agent quits
            // The state first: a session that succeeded before the size was looked at wrote
            // everything, so a short file then is short for good.
            const Session::State state = m_session->state();
            const qint64 size = onDisk();
            if (size > m_size) {
                // The file changed at its source after it was listed (the transfer writes what the
                // sender has now): its end here would cut it short without a word.
                qCWarning(lcStreamHost) << "A transferred file is larger than listed:" << QFileInfo(m_path).fileName();
                return STG_E_READFAULT;
            }
            if (size >= m_pos + want)
                break;
            switch (state) {
            case Session::State::Running:
                break;
            case Session::State::Cancelled:
                return failed(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            case Session::State::Succeeded:
                qCWarning(lcStreamHost) << "A transferred file is shorter than listed:" << QFileInfo(m_path).fileName();
                return STG_E_READFAULT;
            case Session::State::Failed:
            case Session::State::Abandoned:
                return failed(STG_E_READFAULT);
            }
            if (size != seen) {
                if (seen != -2)
                    slice = 0; // it grew: look again soon
                seen = size;
            }
            pause(kSlicesMs[slice]);
            slice = qMin(slice + 1, std::size(kSlicesMs) - 1);
        }
        const Clock::time_point ready = Clock::now();
        LARGE_INTEGER at = {};
        at.QuadPart = m_pos;
        if (!SetFilePointerEx(m_file, at, nullptr, FILE_BEGIN))
            return STG_E_READFAULT;
        qint64 done = 0;
        while (done < want) {
            DWORD got = 0;
            if (!ReadFile(m_file, out + done, DWORD(want - done), &got, nullptr) || got == 0)
                return STG_E_READFAULT;
            done += got;
        }
        m_pos += want;
        if (pcbRead)
            *pcbRead = ULONG(want);
        if (m_stats)
            m_stats->read(nsBetween(started, ready), nsBetween(ready, Clock::now()));
        return S_OK;
    }

    LONG m_ref = 1;
    const std::shared_ptr<Session> m_session;
    const QString m_path;
    const qint64 m_size;
    const std::shared_ptr<df::streamhost::PasteStats> m_stats;
    std::mutex m_mutex;
    qint64 m_pos = 0;
    HANDLE m_file = INVALID_HANDLE_VALUE;
};

// The thread that keeps the agent's multithreaded apartment and creates the streams in it. Started
// at the first stream and kept for the process's lifetime (never destroyed: a stream Explorer
// still holds keeps working until the process ends).
class StreamHost
{
public:
    static StreamHost &instance()
    {
        static StreamHost *host = new StreamHost;
        return *host;
    }

    // Runs `task` on the host's thread (in the multithreaded apartment) and waits for it. False if
    // the apartment could not be entered.
    bool run(const std::function<void()> &task)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_ready; });
        if (!m_ok)
            return false;
        auto done = std::make_shared<bool>(false);
        m_tasks.push_back([this, task, done] {
            task();
            std::lock_guard<std::mutex> doneLock(m_mutex);
            *done = true;
        });
        m_changed.notify_all();
        m_changed.wait(lock, [&done] { return *done; });
        return true;
    }

private:
    StreamHost()
    {
        std::thread([this] { loop(); }).detach();
    }

    void loop()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ok = SUCCEEDED(hr);
            m_ready = true;
        }
        m_changed.notify_all();
        if (FAILED(hr)) {
            qCWarning(lcStreamHost) << "Cannot enter the multithreaded apartment:" << Qt::hex << quint32(hr);
            return;
        }
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_changed.wait(lock, [this] { return !m_tasks.empty(); });
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            task();
            m_changed.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::function<void()>> m_tasks;
    bool m_ready = false;
    bool m_ok = false;
};

} // namespace

namespace df::streamhost {

PasteStats::~PasteStats()
{
    if (m_contentsUs.empty() || ClipboardBridge::fetchesAbandoned())
        return; // nothing asked for (the list only), or the agent quits
    std::vector<quint32> sorted = m_contentsUs;
    std::sort(sorted.begin(), sorted.end());
    const quint32 p95 = sorted[qMin(sorted.size() - 1, size_t(double(sorted.size()) * 0.95))];
    qCInfo(lcStreamHost).noquote()
        << QStringLiteral("Streaming paste ended: %1 file requests (%2 as memory, %3 as streams, %4 failed), "
                          "%5 ms in them (p95 %6 ms, max %7 ms); %8 stream reads: %9 ms waiting for the "
                          "transfer, %10 ms reading; %11")
               .arg(sorted.size())
               .arg(m_hglobal)
               .arg(m_streams)
               .arg(m_failed)
               .arg(double(m_contentsNs) / 1e6, 0, 'f', 0)
               .arg(double(p95) / 1e3, 0, 'f', 2)
               .arg(double(sorted.back()) / 1e3, 0, 'f', 1)
               .arg(m_reads.load())
               .arg(double(m_readWaitNs.load()) / 1e6, 0, 'f', 0)
               .arg(double(m_readNs.load()) / 1e6, 0, 'f', 0)
               .arg(m_sinceFirstListedMs >= 0
                        ? QStringLiteral("first file request %1 ms after the list was first read").arg(m_sinceFirstListedMs)
                        : QStringLiteral("the list was not read before the first file request"));
}

void PasteStats::contents(qint64 ns, Served served)
{
    m_contentsUs.push_back(quint32(qBound<qint64>(0, ns / 1000, 0xFFFFFFFFLL)));
    m_contentsNs += ns;
    switch (served) {
    case Served::HGlobal:
        ++m_hglobal;
        break;
    case Served::Stream:
        ++m_streams;
        break;
    case Served::Failed:
        ++m_failed;
        break;
    }
}

HGLOBAL readCompleteFile(const QString &path, qint64 size)
{
    if (size <= 0 || size > qint64(0x7FFFFFFF))
        return nullptr;
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr; // not there yet
    HGLOBAL global = nullptr;
    LARGE_INTEGER onDisk = {};
    if (GetFileSizeEx(file, &onDisk) && onDisk.QuadPart == size) {
        global = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(size));
        if (global) {
            auto *out = static_cast<char *>(GlobalLock(global));
            qint64 done = 0;
            while (out && done < size) {
                DWORD got = 0;
                if (!ReadFile(file, out + done, DWORD(size - done), &got, nullptr) || got == 0)
                    break;
                done += got;
            }
            if (out)
                GlobalUnlock(global);
            // Exactly its size, and no more on disk now than listed (nothing changed meanwhile).
            if (!out || done != size || !GetFileSizeEx(file, &onDisk) || onDisk.QuadPart != size) {
                GlobalFree(global);
                global = nullptr;
            }
        }
    }
    CloseHandle(file);
    return global;
}

HRESULT createProgressiveStream(std::shared_ptr<ClipboardBridge::PasteSessionBase> session, const QString &path,
                                qint64 size, IStream **out, std::shared_ptr<PasteStats> stats)
{
    if (!out || !session)
        return E_INVALIDARG;
    *out = nullptr;
    // Created and marshalled in the multithreaded apartment; the creation reference goes once the
    // marshalled stream holds its own.
    IStream *marshalled = nullptr;
    HRESULT hr = E_FAIL;
    const bool ran = StreamHost::instance().run([&] {
        auto *stream = new ProgressiveStream(std::move(session), path, size, std::move(stats));
        hr = CoMarshalInterThreadInterfaceInStream(IID_IStream, stream, &marshalled);
        stream->Release();
    });
    if (!ran || FAILED(hr) || !marshalled) {
        qCWarning(lcStreamHost) << "Cannot create a stream in the multithreaded apartment:" << Qt::hex << quint32(hr);
        return E_FAIL;
    }
    hr = CoGetInterfaceAndReleaseStream(marshalled, IID_IStream, reinterpret_cast<void **>(out));
    if (FAILED(hr) || !*out) {
        qCWarning(lcStreamHost) << "Cannot hand a stream over from the multithreaded apartment:" << Qt::hex
                                << quint32(hr);
        *out = nullptr;
        return E_FAIL;
    }
    return S_OK;
}

} // namespace df::streamhost
