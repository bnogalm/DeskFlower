#pragma once

// Building blocks for the transfer work done off the service's main thread (FileTransfer): the
// main thread routes input, keepalives and the network, and must never wait for a disk.
//
// - BoundedQueue: frames handed from the main thread to a worker (or back), bounded in bytes.
// - AsUserBatch: the console user's rights for a batch of disk work on a worker thread.
// - TaskPool: a few threads for short disk jobs (deleting old received copies).

#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace df::detail {

// A queue of items with a byte budget. Producers see "full" at the high-water mark (one item of
// slack: an item is accepted while the queue holds less than `high`, whatever its size); a paused
// producer is told to resume once the consumer brings it down to the low-water mark.
// Thread-safe. close() drops what is queued, refuses every later push and wakes all waiters.
template <typename T>
class BoundedQueue
{
public:
    BoundedQueue(qint64 high, qint64 low) : m_high(high), m_low(low) {}

    // Never blocks: accepted while below the high-water mark, refused when full or closed.
    bool tryPush(T item, qint64 bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_bytes >= m_high)
            return false;
        add(std::move(item), bytes);
        return true;
    }
    // Accepted whatever the budget (an end of stream, an abort): refused only when closed.
    bool pushAlways(T item, qint64 bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
            return false;
        add(std::move(item), bytes);
        return true;
    }
    // Waits while full; false once closed.
    bool push(T item, qint64 bytes)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_closed || m_bytes < m_high; });
        if (m_closed)
            return false;
        add(std::move(item), bytes);
        return true;
    }
    // The next item, waiting up to timeoutMs (0: not at all); nothing on timeout or once closed.
    std::optional<T> pop(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (timeoutMs > 0)
            m_notEmpty.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [this] { return m_closed || !m_items.empty(); });
        if (m_closed || m_items.empty())
            return std::nullopt;
        auto [item, bytes] = std::move(m_items.front());
        m_items.pop_front();
        m_bytes -= bytes;
        if (m_bytes < m_high)
            m_notFull.notify_all();
        return std::move(item);
    }
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            m_items.clear();
            m_bytes = 0;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }
    bool isClosed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }
    qint64 bytes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_bytes;
    }
    bool isFull() const { return bytes() >= m_high; }
    bool isLow() const { return bytes() <= m_low; }

    // Coalesces the notifications of the other side: true when the flag was clear (post one),
    // false when one is already on its way. The side that handles it clears the flag *before*
    // draining, so that whatever arrives meanwhile is either drained or notified again.
    bool setNotifyPending() { return !m_notifyPending.exchange(true); }
    void clearNotifyPending() { m_notifyPending.store(false); }

private:
    void add(T item, qint64 bytes)
    {
        m_items.emplace_back(std::move(item), bytes);
        m_bytes += bytes;
        m_notEmpty.notify_one();
    }

    const qint64 m_high;
    const qint64 m_low;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<std::pair<T, qint64>> m_items;
    qint64 m_bytes = 0;
    bool m_closed = false;
    std::atomic<bool> m_notifyPending{false};
};

// Who a job's first batch of work as the user found signed in, shared by the job's threads (a
// fetch's dispatcher and writers): each compares against it, and a change seen by any of them is
// seen by all.
struct IdentityAnchor {
    std::mutex mutex;
    QString first;
    bool have = false;
    std::atomic<bool> changed{false};
};

// The console user's rights (FileTransfer's Impersonator) for a batch of work on a worker thread:
// one guard serves consecutive work for at most kMaxBatchMs, and is released before any wait
// (release()), so a thread never holds the user's token while idle. Each new guard also checks
// who the user is (the identity function, called while impersonating): a different user than at
// the first batch (of this batch object, or of the job when an anchor is shared) means the console
// user changed under a running job. An empty identity (the query failed) is unknown: neither
// recorded nor taken for a change.
class AsUserBatch
{
public:
    enum class Status { Ok, NoUser, UserChanged };
    static constexpr int kMaxBatchMs = 100;

    AsUserBatch(std::function<std::shared_ptr<void>()> impersonate, std::function<QString()> identity,
                std::shared_ptr<IdentityAnchor> anchor = {})
        : m_impersonate(std::move(impersonate)), m_identity(std::move(identity)), m_anchor(std::move(anchor))
    {
    }
    ~AsUserBatch() { release(); }
    AsUserBatch(const AsUserBatch &) = delete;
    AsUserBatch &operator=(const AsUserBatch &) = delete;

    // The guard for the work that follows: the one held, while it is young, or a new one.
    Status acquire();
    void release() { m_guard.reset(); }
    bool userChanged() const { return m_userChanged; }

private:
    std::function<std::shared_ptr<void>()> m_impersonate;
    std::function<QString()> m_identity;
    std::shared_ptr<IdentityAnchor> m_anchor;
    std::shared_ptr<void> m_guard;
    QElapsedTimer m_since;
    QString m_firstUser;
    bool m_haveUser = false;
    bool m_userChanged = false;
};

// A fixed number of threads (started when first needed) running posted tasks in order.
// shutdown() drops the tasks not started, waits for the running ones until `deadline` and leaves
// the rest to finish on their own (their threads are detached and keep the pool's state alive).
class TaskPool
{
public:
    explicit TaskPool(int threads);
    ~TaskPool();
    TaskPool(const TaskPool &) = delete;
    TaskPool &operator=(const TaskPool &) = delete;

    void post(std::function<void()> task);
    void shutdown(std::chrono::steady_clock::time_point deadline);

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace df::detail
