// Many-small-files benchmark of FileTransfer (not a test: not registered with ctest).
//
// Usage (loopback only, as every test):
//   set DESKFLOWER_BIND_ADDRESS=127.0.0.1
//   set DESKFLOWER_BENCH=1
//   bench_transfer <src> <dst> [tree] [big] [mixed] [micro] [recv] [link] [apis] [threads] [writers=N]
//                  [noreadahead]
// <src>: made by tests/bench/make_tree.py (<src>/tree, a folder of many small files, and
// <src>/big.bin, one large file). <dst>: an empty scratch folder; everything written there is
// removed again. Steps (all by default):
//   tree, big   an end-to-end fetch of <src>/tree or <src>/big.bin between two Nodes over
//               loopback: wall time, files/s, MB/s, how busy each Node's thread was
//   mixed       the same with <src>/tree and <src>/big.bin in one offer (many small files and one
//               huge one, as a folder with a disk image in it; not run by default)
//   micro       the receiver's per-file disk sequence (mkpath of the parent, the link check, create,
//               write, close), the sender's (open, read, close), that receiver sequence on 1-8
//               threads, and the frame encryption alone, each timed apart on the same tree
//   recv, link  only the receiver's per-file sequence, or only the link check timings, of micro
//   apis        the file APIs a receiver could write with (QFile, CreateFileW and variants,
//               CopyFileW as a reference) on 1-8 threads (not run by default)
//   threads     the two Nodes on threads of their own (as on two machines) instead of both on the
//               main thread (as in tst_transfer)
//   writers=N   every fetch with N writer threads instead of what FileTransfer chooses
//   noreadahead the sender reads every file itself (FileTransfer::setReadAheadForTests(false))
// If src/core/benchprobe.h exists (temporary timers added by hand, never committed), each fetch
// also prints where its threads spent their time.
// Without DESKFLOWER_BENCH=1 it prints a line and exits (nothing is run by accident).

#include "crypto.h"
#include "filetransfer.h"
#include "fsutil.h"
#include "node.h"
#include "testutil.h"

#if __has_include("benchprobe.h")
#include "benchprobe.h"
#define DF_HAVE_PROBES 1
#endif

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace df;

namespace {

constexpr int kChunk = FileTransfer::kChunk;

double ms(qint64 ns) { return double(ns) / 1e6; }

void out(const char *fmt, auto... args)
{
    std::printf(fmt, args...);
    std::fflush(stdout);
}

// How long a thread's event loop was busy (between waking up and going back to wait).
class BusyMeter : public QObject
{
public:
    void install(QAbstractEventDispatcher *d)
    {
        connect(d, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (m_busy) {
                m_busyNs += m_since.nsecsElapsed();
                m_busy = false;
            }
        }, Qt::DirectConnection);
        connect(d, &QAbstractEventDispatcher::awake, this, [this] {
            if (!m_busy) {
                m_since.start();
                m_busy = true;
            }
        }, Qt::DirectConnection);
    }
    // On the measured thread.
    void reset()
    {
        m_busyNs = 0;
        m_since.start();
        m_busy = true;
    }
    qint64 busyNs()
    {
        return m_busyNs + (m_busy ? m_since.nsecsElapsed() : 0);
    }

private:
    QElapsedTimer m_since;
    qint64 m_busyNs = 0;
    bool m_busy = true;
};

// A Node and its FileTransfer, on the main thread or on a thread of its own.
struct Side {
    std::unique_ptr<QThread> thread;
    QObject *ctx = nullptr; // lives on the side's thread
    std::unique_ptr<Node> node;
    std::unique_ptr<FileTransfer> ft;
    BusyMeter *busy = nullptr;

    template <typename F>
    void run(F f)
    {
        if (QThread::currentThread() == ctx->thread())
            f();
        else
            QMetaObject::invokeMethod(ctx, f, Qt::BlockingQueuedConnection);
    }
};

void startSide(Side &s, const QString &config, bool ownThread)
{
    if (ownThread) {
        s.thread = std::make_unique<QThread>();
        s.thread->start();
        s.ctx = new QObject;
        s.ctx->moveToThread(s.thread.get());
    } else {
        s.ctx = new QObject;
    }
    s.run([&] {
        s.node = test::makeNode(config);
        s.ft = std::make_unique<FileTransfer>(s.node.get());
        s.busy = new BusyMeter;
        s.busy->install(QAbstractEventDispatcher::instance());
    });
}

void stopSide(Side &s)
{
    s.run([&] {
        s.ft.reset();
        s.node.reset();
        delete s.busy;
        s.busy = nullptr;
    });
    if (s.thread) {
        QObject *ctx = s.ctx;
        QMetaObject::invokeMethod(ctx, [ctx] { delete ctx; }, Qt::BlockingQueuedConnection);
        s.thread->quit();
        s.thread->wait();
        s.thread.reset();
    } else {
        delete s.ctx;
    }
    s.ctx = nullptr;
}

// Runs the main thread's event loop until `done` or the timeout.
bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    QElapsedTimer t;
    t.start();
    bool ok = true;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done())
            loop.quit();
        else if (t.elapsed() > timeoutMs) {
            ok = false;
            loop.quit();
        }
    });
    poll.start(5);
    loop.exec();
    return ok;
}

struct TreeFile {
    QString abs;
    QString rel;
    qint64 size;
};

void listTree(const QString &root, QStringList *dirs, std::vector<TreeFile> *files)
{
    const QDir base(QFileInfo(root).absolutePath());
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isDir())
            dirs->append(base.relativeFilePath(info.absoluteFilePath()));
        else
            files->push_back({info.absoluteFilePath(), base.relativeFilePath(info.absoluteFilePath()), info.size()});
    }
}

#ifdef DF_HAVE_PROBES
void printProbes(qint64 wallNs)
{
    out("    probes (sum over calls; S sender reader thread, M a Node's thread, R receiver worker):\n");
    for (int i = 0; i < benchprobe::Count; ++i) {
        const long long n = benchprobe::calls[i].load();
        if (!n)
            continue;
        const long long t = benchprobe::ns[i].load();
        out("      %-36s %10.1f ms %6.1f%% of wall  %9lld calls  %8.2f us/call\n", benchprobe::kNames[i], ms(t),
            100.0 * double(t) / double(wallNs), n, double(t) / double(n) / 1000.0);
    }
}
#endif

// One end-to-end fetch of `paths` from a to b into dst/<name>.
void benchFetch(Side &a, Side &b, const QStringList &paths, const QString &dst, const char *name)
{
    Offer offer;
    QString error;
    QElapsedTimer t;
    t.start();
    bool added = false;
    a.run([&] { added = a.ft->addOfferPaths(&offer, paths, qint64(1) << 40, 200000, &error); });
    const qint64 enumNs = t.nsecsElapsed();
    if (!added) {
        out("%s: offer failed: %s\n", name, qPrintable(error));
        return;
    }
    const qint64 bytes = offer.fileBytes();
    const int files = int(offer.files.size());
    quint64 id = 0;
    a.run([&] { id = a.ft->publish(offer); });
    const QString target = QDir(dst).filePath(QString::fromLatin1(name));

    std::atomic<bool> done{false};
    FetchResult result;
#ifdef DF_HAVE_PROBES
    benchprobe::reset();
#endif
    a.run([&] { a.busy->reset(); });
    if (b.ctx != a.ctx)
        b.run([&] { b.busy->reset(); });
    FileTransfer::Counters sentBefore, receivedBefore;
    a.run([&] { sentBefore = a.ft->countersForTests(); });
    b.run([&] { receivedBefore = b.ft->countersForTests(); });
    t.start();
    b.run([&] {
        // A kind: progress is reported, as for a paste.
        b.ft->fetch(a.node->id(), id, target, true, qint64(1) << 40,
                    [&](const FetchResult &r) {
                        result = r;
                        done.store(true);
                    },
                    QStringLiteral("clipboard"));
    });
    if (!waitFor([&] { return done.load(); }, 3600 * 1000)) {
        out("%s: timed out\n", name);
        return;
    }
    const qint64 wall = t.nsecsElapsed();
    qint64 busyA = 0, busyB = 0;
    a.run([&] { busyA = a.busy->busyNs(); });
    if (b.ctx != a.ctx)
        b.run([&] { busyB = b.busy->busyNs(); });
    const double secs = double(wall) / 1e9;
    out("%s: %s, %d files, %d folders, %.1f MB: offer built in %.0f ms; fetch %.2f s = %.0f files/s, %.1f MB/s\n", name,
        result.ok ? "ok" : qPrintable(QStringLiteral("FAILED (") + result.error + QLatin1Char(')')), files,
        int(offer.dirs.size()), double(bytes) / 1e6, ms(enumNs), secs, files / secs, double(bytes) / 1e6 / secs);
    if (b.ctx == a.ctx)
        out("    both Nodes' thread (main) busy %.0f ms (%.0f%%)\n", ms(busyA), 100.0 * double(busyA) / double(wall));
    else
        out("    sender Node thread busy %.0f ms (%.0f%%), receiver Node thread busy %.0f ms (%.0f%%)\n", ms(busyA),
            100.0 * double(busyA) / double(wall), ms(busyB), 100.0 * double(busyB) / double(wall));
    FileTransfer::Counters sent, received;
    a.run([&] { sent = a.ft->countersForTests(); });
    int writers = 0;
    b.run([&] {
        received = b.ft->countersForTests();
        writers = b.ft->lastWriterCountForTests();
    });
    const qint64 busyNs = received.writerBusyNs - receivedBefore.writerBusyNs;
    out("    sender read ahead %lld files, its reader waited %.0f ms for them\n",
        sent.readAheadFiles - sentBefore.readAheadFiles, ms(sent.readAheadWaitNs - sentBefore.readAheadWaitNs));
    out("    data frames: %lld sent, %lld received; %d writer(s) busy %.0f%% of the fetch on average; the worker "
        "waited %.0f ms for room in their queues\n",
        sent.dataFramesSent - sentBefore.dataFramesSent, received.dataFramesReceived - receivedBefore.dataFramesReceived,
        writers, writers > 0 ? 100.0 * double(busyNs) / double(writers) / double(wall) : 0.0,
        ms(received.writerRoomWaitNs - receivedBefore.writerRoomWaitNs));
#ifdef DF_HAVE_PROBES
    printProbes(wall);
#endif
    a.run([&] { a.ft->unpublish(id); });
    t.start();
    QDir(target).removeRecursively();
    out("    (removing the received copy took %.2f s)\n", double(t.nsecsElapsed()) / 1e9);
}

// The receiver's per-file sequence (DownloadJob::consume), each step timed apart, on one thread.
void microReceiver(const QString &dst, const QStringList &dirs, const std::vector<TreeFile> &files,
                   const QByteArray &pool)
{
    const QString target = QDir(dst).filePath(QStringLiteral("micro-recv"));
    qint64 tDirs = 0, tMkpath = 0, tLink = 0, tOpen = 0, tWrite = 0, tClose = 0;
    QElapsedTimer all, t;
    all.start();
    t.start();
    for (const QString &d : dirs) { // createDirs: link check, mkpath, link check
        const QString path = QDir(target).filePath(d);
        fs::passesThroughLink(path);
        QDir().mkpath(path);
        fs::passesThroughLink(path);
    }
    tDirs = t.nsecsElapsed();
    qint64 bytes = 0;
    for (const TreeFile &f : files) {
        const QString path = QDir(target).filePath(f.rel);
        const QString parent = QFileInfo(path).absolutePath();
        t.start();
        QDir().mkpath(parent);
        tMkpath += t.nsecsElapsed();
        t.start();
        fs::passesThroughLink(parent);
        tLink += t.nsecsElapsed();
        t.start();
        QFile file(path);
        (void)file.open(QIODevice::WriteOnly | QIODevice::Unbuffered);
        tOpen += t.nsecsElapsed();
        t.start();
        for (qint64 off = 0; off < f.size; off += kChunk) {
            const qint64 n = std::min<qint64>(kChunk, f.size - off);
            file.write(pool.constData() + (off % (pool.size() - kChunk)), n);
        }
        tWrite += t.nsecsElapsed();
        t.start();
        file.close();
        tClose += t.nsecsElapsed();
        bytes += f.size;
    }
    const qint64 wall = all.nsecsElapsed();
    const double n = double(files.size());
    out("micro receiver sequence, 1 thread: %.2f s = %.0f files/s, %.1f MB/s\n", double(wall) / 1e9,
        n / (double(wall) / 1e9), double(bytes) / 1e6 / (double(wall) / 1e9));
    out("    folders (link check, mkpath, link check) %7.0f ms  (%.1f us/folder)\n", ms(tDirs),
        double(tDirs) / 1000.0 / dirs.size());
    out("    mkpath(parent) per file                  %7.0f ms  (%.1f us/file)\n", ms(tMkpath), double(tMkpath) / 1000.0 / n);
    out("    passesThroughLink(parent) per file       %7.0f ms  (%.1f us/file)\n", ms(tLink), double(tLink) / 1000.0 / n);
    out("    create/open                              %7.0f ms  (%.1f us/file)\n", ms(tOpen), double(tOpen) / 1000.0 / n);
    out("    write                                    %7.0f ms  (%.1f us/file)\n", ms(tWrite), double(tWrite) / 1000.0 / n);
    out("    close                                    %7.0f ms  (%.1f us/file)\n", ms(tClose), double(tClose) / 1000.0 / n);
    // The same link checks again, now that the tree is written and settled.
    t.start();
    for (const TreeFile &f : files)
        fs::passesThroughLink(QFileInfo(QDir(target).filePath(f.rel)).absolutePath());
    out("    passesThroughLink(parent) again, afterwards %4.0f ms  (%.1f us/file)\n", ms(t.nsecsElapsed()),
        double(t.nsecsElapsed()) / 1000.0 / n);
    QDir(target).removeRecursively();
}

// Create, write, close only (folders made first), the files dealt round-robin to `threads`
// threads: what parallel writing would gain on this disk (with its virus scanner).
void microParallelWrite(const QString &dst, const QStringList &dirs, const std::vector<TreeFile> &files,
                        const QByteArray &pool, int threads)
{
    const QString target = QDir(dst).filePath(QStringLiteral("micro-par%1").arg(threads));
    for (const QString &d : dirs)
        QDir().mkpath(QDir(target).filePath(d));
    QElapsedTimer t;
    t.start();
    std::vector<std::thread> pool_;
    std::atomic<qint64> closeNs{0};
    for (int k = 0; k < threads; ++k) {
        pool_.emplace_back([&, k] {
            QElapsedTimer c;
            for (size_t i = k; i < files.size(); i += threads) {
                const TreeFile &f = files[i];
                QFile file(QDir(target).filePath(f.rel));
                (void)file.open(QIODevice::WriteOnly | QIODevice::Unbuffered);
                for (qint64 off = 0; off < f.size; off += kChunk)
                    file.write(pool.constData() + (off % (pool.size() - kChunk)), std::min<qint64>(kChunk, f.size - off));
                c.start();
                file.close();
                closeNs += c.nsecsElapsed();
            }
        });
    }
    for (auto &th : pool_)
        th.join();
    const double secs = double(t.nsecsElapsed()) / 1e9;
    out("micro create+write+close on %d thread(s): %.2f s = %.0f files/s (close: %.1f us/file per thread)\n", threads,
        secs, double(files.size()) / secs, double(closeNs.load()) / 1000.0 / double(files.size()));
    QDir(target).removeRecursively();
}

// The file APIs a parallel receiver could write with, on 1-8 threads (folders made first, files
// dealt round-robin): (a) QFile; (b) CreateFileW CREATE_ALWAYS + WriteFile + CloseHandle; (c) the
// same with CREATE_NEW (CREATE_ALWAYS when the file exists); (d) (b) with the closes handed to a
// thread of their own. The choice for fs::WriteFile.
void microWriteApis(const QString &dst, const QStringList &dirs, const std::vector<TreeFile> &files,
                    const QByteArray &pool)
{
#ifndef Q_OS_WIN
    Q_UNUSED(dst);
    Q_UNUSED(dirs);
    Q_UNUSED(files);
    Q_UNUSED(pool);
    out("micro write APIs: Windows only\n");
#else
    const char *names[] = {"QFile", "Win32 CREATE_ALWAYS", "Win32 CREATE_NEW", "Win32 + closer thread",
                           "Win32 dealt by folder", "CopyFileW (reference)", "Win32 + allocation size",
                           "Win32 + end of file first"};
    for (int api = 0; api < 8; ++api) {
        for (int threads : {1, 2, 4, 8}) {
            const QString target = QDir(dst).filePath(QStringLiteral("micro-api%1-%2").arg(api).arg(threads));
            for (const QString &d : dirs)
                QDir().mkpath(QDir(target).filePath(d));
            std::mutex closeMutex;
            std::condition_variable closeWake;
            std::deque<HANDLE> toClose;
            bool closing = true;
            std::thread closer;
            if (api == 3) {
                closer = std::thread([&] {
                    std::unique_lock<std::mutex> lock(closeMutex);
                    for (;;) {
                        closeWake.wait(lock, [&] { return !toClose.empty() || !closing; });
                        if (toClose.empty() && !closing)
                            return;
                        HANDLE h = toClose.front();
                        toClose.pop_front();
                        lock.unlock();
                        CloseHandle(h);
                        lock.lock();
                    }
                });
            }
            QElapsedTimer t;
            t.start();
            std::vector<std::thread> workers;
            for (int k = 0; k < threads; ++k) {
                workers.emplace_back([&, k] {
                    for (size_t i = 0; i < files.size(); ++i) {
                        const TreeFile &f = files[i];
                        const int owner = api == 4 ? int(qHash(QFileInfo(f.rel).path()) % uint(threads)) : int(i % threads);
                        if (owner != k)
                            continue;
                        const QString path = QDir(target).filePath(f.rel);
                        if (api == 5) {
                            CopyFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(f.abs).utf16()),
                                      reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()), FALSE);
                            continue;
                        }
                        if (api == 0) {
                            QFile file(path);
                            (void)file.open(QIODevice::WriteOnly | QIODevice::Unbuffered);
                            for (qint64 off = 0; off < f.size; off += 64 * 1024)
                                file.write(pool.constData() + (off % (pool.size() - 65536)), std::min<qint64>(65536, f.size - off));
                            continue;
                        }
                        const std::wstring w = (QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(path)).toStdWString();
                        constexpr DWORD kFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT;
                        HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, api == 2 ? CREATE_NEW : CREATE_ALWAYS, kFlags, nullptr);
                        if (h == INVALID_HANDLE_VALUE && api == 2)
                            h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, CREATE_ALWAYS, kFlags, nullptr);
                        if (h == INVALID_HANDLE_VALUE)
                            continue;
                        if (api == 6) {
                            FILE_ALLOCATION_INFO alloc{};
                            alloc.AllocationSize.QuadPart = f.size;
                            SetFileInformationByHandle(h, FileAllocationInfo, &alloc, sizeof(alloc));
                        } else if (api == 7) {
                            FILE_END_OF_FILE_INFO eof{};
                            eof.EndOfFile.QuadPart = f.size;
                            SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof));
                        }
                        for (qint64 off = 0; off < f.size; off += 64 * 1024) {
                            DWORD written = 0;
                            WriteFile(h, pool.constData() + (off % (pool.size() - 65536)),
                                      DWORD(std::min<qint64>(65536, f.size - off)), &written, nullptr);
                        }
                        if (api == 3) {
                            {
                                std::lock_guard<std::mutex> lock(closeMutex);
                                toClose.push_back(h);
                            }
                            closeWake.notify_one();
                        } else {
                            CloseHandle(h);
                        }
                    }
                });
            }
            for (auto &th : workers)
                th.join();
            if (api == 3) {
                {
                    std::lock_guard<std::mutex> lock(closeMutex);
                    closing = false;
                }
                closeWake.notify_one();
                closer.join();
            }
            const double secs = double(t.nsecsElapsed()) / 1e9;
            out("micro write API %-22s %d thread(s): %.2f s = %.0f files/s\n", names[api], threads, secs,
                double(files.size()) / secs);
            QDir(target).removeRecursively();
        }
    }
#endif
}

// The sender's sequence (UploadJob::run): open, read in kChunk pieces, close.
void microSender(const std::vector<TreeFile> &files, int threads)
{
    std::atomic<qint64> tOpen{0}, tRead{0}, tClose{0};
    QElapsedTimer t;
    t.start();
    std::vector<std::thread> pool_;
    for (int k = 0; k < threads; ++k) {
        pool_.emplace_back([&, k] {
            QElapsedTimer s;
            for (size_t i = k; i < files.size(); i += threads) {
                s.start();
                QFile file(files[i].abs);
                (void)file.open(QIODevice::ReadOnly);
                tOpen += s.nsecsElapsed();
                s.start();
                while (!file.read(kChunk).isEmpty()) {
                }
                tRead += s.nsecsElapsed();
                s.start();
                file.close();
                tClose += s.nsecsElapsed();
            }
        });
    }
    for (auto &th : pool_)
        th.join();
    const double secs = double(t.nsecsElapsed()) / 1e9;
    const double n = double(files.size());
    out("micro sender sequence (warm cache), %d thread(s): %.2f s = %.0f files/s; open %.1f, read %.1f, close %.1f us/file\n",
        threads, secs, n / secs, double(tOpen.load()) / 1000.0 / n, double(tRead.load()) / 1000.0 / n,
        double(tClose.load()) / 1000.0 / n);
}

// The frames a fetch of these files encrypts and decrypts, without the network.
void microCrypto(const std::vector<TreeFile> &files, const QByteArray &pool)
{
    const QByteArray key(crypto::kKeySize, 'k');
    std::vector<QByteArray> frames;
    for (const TreeFile &f : files) {
        for (qint64 off = 0; off < f.size; off += kChunk)
            frames.push_back(pool.mid(0, int(std::min<qint64>(kChunk, f.size - off)) + 1));
    }
    QElapsedTimer t;
    t.start();
    std::vector<QByteArray> sealed;
    sealed.reserve(frames.size());
    quint64 nonce = 0;
    qint64 bytes = 0;
    for (const QByteArray &f : frames) {
        sealed.push_back(crypto::aeadEncrypt(key, nonce++, {}, f));
        bytes += f.size();
    }
    const qint64 enc = t.nsecsElapsed();
    t.start();
    nonce = 0;
    for (const QByteArray &s : sealed)
        crypto::aeadDecrypt(key, nonce++, {}, s);
    const qint64 dec = t.nsecsElapsed();
    out("micro crypto: %zu frames, %.1f MB: encrypt %.0f ms (%.0f MB/s), decrypt %.0f ms (%.0f MB/s)\n", frames.size(),
        double(bytes) / 1e6, ms(enc), double(bytes) / 1e3 / ms(enc), ms(dec), double(bytes) / 1e3 / ms(dec));
}

// The cost of fs::passesThroughLink on one folder, against the stat calls it is made of.
void microLinkCheck(const QString &folder, const std::vector<TreeFile> &files)
{
    const int n = 2000;
    const int depth = int(QDir::cleanPath(folder).count(QLatin1Char('/'))) + 1;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < n; ++i)
        fs::passesThroughLink(folder);
    const qint64 link = t.nsecsElapsed();
    t.start();
    for (int i = 0; i < n; ++i) {
        QFileInfo info(folder);
        (void)info.isSymbolicLink();
        (void)info.isJunction();
    }
    const qint64 one = t.nsecsElapsed();
    out("micro link check: %d components: passesThroughLink %.1f us/call (%.1f us/component); one QFileInfo "
        "isSymbolicLink+isJunction %.1f us\n",
        depth, double(link) / 1000.0 / n, double(link) / 1000.0 / n / depth, double(one) / 1000.0 / n);
    // Every file's own folder once (distinct paths, as a fetch meets them).
    t.start();
    for (const TreeFile &f : files)
        fs::passesThroughLink(QFileInfo(f.abs).absolutePath());
    out("    passesThroughLink on each source file's folder: %.1f us/call\n",
        double(t.nsecsElapsed()) / 1000.0 / double(files.size()));
#ifdef Q_OS_WIN
    const std::wstring w = QDir::toNativeSeparators(folder).toStdWString();
    t.start();
    for (int i = 0; i < n; ++i) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &data);
    }
    out("    GetFileAttributesExW on the same folder: %.1f us\n", double(t.nsecsElapsed()) / 1000.0 / n);
#endif
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("DESKFLOWER_BENCH") != QLatin1String("1")) {
        out("bench_transfer: set DESKFLOWER_BENCH=1 (and DESKFLOWER_BIND_ADDRESS=127.0.0.1) to run it\n");
        return 0;
    }
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        out("usage: bench_transfer <src> <dst> [tree] [big] [micro] [threads]\n");
        return 2;
    }
    const QString src = QDir(args.at(1)).absolutePath();
    const QString dst = QDir(args.at(2)).absolutePath();
    QStringList steps = args.mid(3);
    const bool threaded = steps.removeAll(QStringLiteral("threads")) > 0;
    const bool noReadAhead = steps.removeAll(QStringLiteral("noreadahead")) > 0;
    // writers=N: every fetch with N writer threads (default: what FileTransfer chooses).
    for (const QString &step : QStringList(steps)) {
        if (step.startsWith(QLatin1String("writers="))) {
            FileTransfer::setDefaultWriterThreadsForTests(step.mid(8).toInt());
            out("writers: %d per fetch\n", step.mid(8).toInt());
            steps.removeAll(step);
        }
    }
    if (steps.isEmpty())
        steps = {QStringLiteral("tree"), QStringLiteral("big"), QStringLiteral("micro")};
    QDir().mkpath(dst);
    crypto::init();

    const QString tree = QDir(src).filePath(QStringLiteral("tree"));
    const QString big = QDir(src).filePath(QStringLiteral("big.bin"));

    if (steps.contains(QStringLiteral("tree")) || steps.contains(QStringLiteral("big"))
        || steps.contains(QStringLiteral("mixed"))) {
        const QString cfgA = QDir(dst).filePath(QStringLiteral("cfg-a"));
        const QString cfgB = QDir(dst).filePath(QStringLiteral("cfg-b"));
        QDir(cfgA).removeRecursively();
        QDir(cfgB).removeRecursively();
        Side a, b;
        {
            // Paired on the main thread; in threads mode the Nodes are then started again on their
            // own threads from the same configuration (they remember each other).
            auto na = test::makeNode(cfgA);
            auto nb = test::makeNode(cfgB);
            if (!test::pairNodes(*na, *nb)) {
                out("pairing failed\n");
                return 1;
            }
            if (!threaded) {
                a.ctx = new QObject;
                a.node = std::move(na);
                a.ft = std::make_unique<FileTransfer>(a.node.get());
                a.busy = new BusyMeter;
                a.busy->install(QAbstractEventDispatcher::instance());
                b.ctx = a.ctx;
                b.node = std::move(nb);
                b.ft = std::make_unique<FileTransfer>(b.node.get());
                b.busy = a.busy;
            }
        }
        if (threaded) {
            startSide(a, cfgA, true);
            startSide(b, cfgB, true);
            quint16 port = 0;
            b.run([&] { port = b.node->dataPort(); });
            a.run([&] { a.node->addManualPeer(QHostAddress(QHostAddress::LocalHost), port); });
            QByteArray idA, idB;
            a.run([&] { idA = a.node->id(); });
            b.run([&] { idB = b.node->id(); });
            const bool up = waitFor([&] {
                bool ok = false;
                a.run([&] { ok = test::connectedWithHello(*a.node, idB); });
                bool ok2 = false;
                b.run([&] { ok2 = test::connectedWithHello(*b.node, idA); });
                return ok && ok2;
            }, 15000);
            if (!up) {
                out("the Nodes on their own threads did not reconnect\n");
                return 1;
            }
        }
        out("Nodes: %s\n", threaded ? "each on a thread of its own" : "both on the main thread (as tst_transfer)");
        if (noReadAhead) {
            a.run([&] { a.ft->setReadAheadForTests(false); });
            out("sender read-ahead: off\n");
        }
        if (steps.contains(QStringLiteral("tree")))
            benchFetch(a, b, {tree}, dst, "tree");
        if (steps.contains(QStringLiteral("big")))
            benchFetch(a, b, {big}, dst, "big");
        if (steps.contains(QStringLiteral("mixed")))
            benchFetch(a, b, {tree, big}, dst, "mixed");
        if (threaded) {
            stopSide(a);
            stopSide(b);
        } else {
            a.ft.reset();
            b.ft.reset();
            a.node.reset();
            b.node.reset();
            delete a.busy;
            delete a.ctx;
        }
        QDir(cfgA).removeRecursively();
        QDir(cfgB).removeRecursively();
    }

    const bool micro = steps.contains(QStringLiteral("micro"));
    if (micro || steps.contains(QStringLiteral("recv")) || steps.contains(QStringLiteral("link"))
        || steps.contains(QStringLiteral("apis"))) {
        QStringList dirs;
        std::vector<TreeFile> files;
        listTree(tree, &dirs, &files);
        dirs.prepend(QStringLiteral("tree"));
        QByteArray pool(6 * 1024 * 1024, Qt::Uninitialized);
        QRandomGenerator(1).fillRange(reinterpret_cast<quint32 *>(pool.data()), pool.size() / 4);
        if (micro || steps.contains(QStringLiteral("link")))
            microLinkCheck(QFileInfo(files.back().abs).absolutePath(), files);
        if (micro) {
            microSender(files, 1);
            microSender(files, 8);
            microCrypto(files, pool);
        }
        if (micro || steps.contains(QStringLiteral("recv")))
            microReceiver(dst, dirs, files, pool);
        if (micro) {
            for (int threads : {1, 4, 8})
                microParallelWrite(dst, dirs, files, pool, threads);
        }
        if (steps.contains(QStringLiteral("apis")))
            microWriteApis(dst, dirs, files, pool);
    }
    return 0;
}
