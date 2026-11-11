#pragma once

// Windows: the streams a streaming paste hands to Explorer (FileContents, see virtualfiles.h).
//
// Each one is a file that is still being written by the service's transfer, read while it grows:
// Read returns only once every byte asked for (up to the end of the file) is on disk, so it may
// wait a long time. Such waits must never block the agent's own thread (its window, its IPC, the
// clipboard object Explorer calls into), so the streams live in the agent's multithreaded
// apartment: one thread kept for the process's lifetime creates them there and hands each one to
// the calling thread marshalled. A proxy given on to Explorer refers to the object in that
// apartment, and Explorer's reads run on COM's own threads.
//
// A stream:
// - has the size the list announced; the bytes available are the file's current size on disk (the
//   transfer writes unbuffered and never preallocates), so a file is complete when both are equal;
// - waits in short slices (a backoff of 1, 2, 5, 10 then 20 ms, back to 1 ms whenever the file
//   grew; a high-resolution waitable timer where the system has one, so a 1 ms wait lasts about
//   1 ms and not the 15.6 ms of the default timer), opening the file once it exists, and returns
//   at once when the session
//   is cancelled (HRESULT_FROM_WIN32(ERROR_CANCELLED)), failed or given up (STG_E_READFAULT), or
//   succeeded while the file is still short (STG_E_READFAULT: never a hang); an empty file is at
//   its end at once;
// - fails (STG_E_READFAULT) once the file on disk is larger than listed (it changed at its source
//   after it was listed: its listed end would cut it short without a word);
// - reads with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, and closes the file once the
//   session failed, was cancelled or given up: a transfer that ends that way removes what it wrote
//   at once (a live paste's folder is kept by the service's pin, not by these handles);
// - fails at once once the agent quits (ClipboardBridge::fetchesAbandoned());
// - implements Read, Seek, Stat, CopyTo (1 MiB at a time) and Clone; the rest is not supported;
// - owns its paste session: reads (and their absence) are its activity;
// - counts its Reads in the session's PasteStats, if given: how long they waited for the
//   transfer, and how long they spent reading what was there.
//
// A streaming paste's small files that are already complete when Explorer asks for them are not
// served as streams at all (virtualfiles_win.cpp: their bytes in an HGLOBAL, which saves Explorer
// the Read and Release calls of a stream); readCompleteFile is that read.

#include "clipboardbridge.h"

#include <atomic>
#include <memory>
#include <vector>

#include <objidl.h>

namespace df::streamhost {

// What one streaming paste cost on the agent, logged at info level once (when the last holder lets
// go: the clipboard object that opened the session and every stream made from it, i.e. when the
// session ends on the agent). Cheap: counters, and one number per contents request (no logging per
// call). The object's side (contents requests) is written on the clipboard's thread only; the
// streams' side (Reads) on any thread of the multithreaded apartment, with atomics.
class PasteStats
{
public:
    // sinceFirstListedMs: how long before this session's first contents request the descriptor was
    // first read (since the previous session of the same object), -1 if it was not.
    explicit PasteStats(qint64 sinceFirstListedMs) : m_sinceFirstListedMs(sinceFirstListedMs) {}
    ~PasteStats();
    PasteStats(const PasteStats &) = delete;
    PasteStats &operator=(const PasteStats &) = delete;

    // A FileContents request answered (clipboard thread): how long it took, and how.
    enum class Served { HGlobal, Stream, Failed };
    void contents(qint64 ns, Served served);
    // A stream's Read (any thread): time spent waiting for the transfer, then reading.
    void read(qint64 waitNs, qint64 readNs)
    {
        m_reads.fetch_add(1, std::memory_order_relaxed);
        m_readWaitNs.fetch_add(waitNs, std::memory_order_relaxed);
        m_readNs.fetch_add(readNs, std::memory_order_relaxed);
    }

private:
    const qint64 m_sinceFirstListedMs;
    std::vector<quint32> m_contentsUs; // clipboard thread only (read once nobody else holds this)
    qint64 m_contentsNs = 0;
    int m_hglobal = 0;
    int m_streams = 0;
    int m_failed = 0;
    std::atomic<qint64> m_reads{0};
    std::atomic<qint64> m_readWaitNs{0};
    std::atomic<qint64> m_readNs{0};
};

// The bytes of the complete file at `path` (absolute) in a new movable HGLOBAL, read with
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE: exactly `size` bytes, and only if the
// file on disk has exactly that size now (the transfer writes unbuffered and never preallocates,
// so a file of its listed size is complete). Never waits: null if the file is missing, shorter
// or longer, empty (`size` <= 0), or anything fails. Blocking file I/O on the calling thread: for
// small files only.
HGLOBAL readCompleteFile(const QString &path, qint64 size);

// A stream of the file at `path` (absolute), announced with `size` bytes, read while `session`
// writes it. The calling thread must have entered an apartment (the agent's thread is an STA): it
// gets a proxy to the object, which lives in the multithreaded apartment. E_FAIL if anything
// fails (there is no fallback in the caller's apartment).
// stats: where its Reads are counted (and its clones'), or null.
HRESULT createProgressiveStream(std::shared_ptr<ClipboardBridge::PasteSessionBase> session, const QString &path,
                                qint64 size, IStream **out, std::shared_ptr<PasteStats> stats = {});

} // namespace df::streamhost
