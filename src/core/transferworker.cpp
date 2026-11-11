#include "transferworker.h"

#include <vector>

namespace df::detail {

// --- AsUserBatch -------------------------------------------------------------------------------

AsUserBatch::Status AsUserBatch::acquire()
{
    if (m_userChanged || (m_anchor && m_anchor->changed.load())) {
        m_userChanged = true;
        m_guard.reset();
        return Status::UserChanged;
    }
    if (m_guard && m_since.isValid() && m_since.elapsed() < kMaxBatchMs)
        return Status::Ok;
    m_guard.reset(); // the previous one is put back before the next is taken
    m_guard = m_impersonate ? m_impersonate() : std::shared_ptr<void>(std::make_shared<int>(0));
    if (!m_guard)
        return Status::NoUser;
    m_since.start();
    if (m_identity) {
        // An empty answer is a failed query of the token (a passing failure), not a different
        // user: asked once more, then taken as unknown. It is never recorded as the first user,
        // and an unknown one is let through, as without a check.
        QString user = m_identity();
        if (user.isEmpty())
            user = m_identity();
        if (user.isEmpty())
            return Status::Ok;
        if (m_anchor) {
            std::lock_guard<std::mutex> lock(m_anchor->mutex);
            if (!m_anchor->have) {
                m_anchor->first = user;
                m_anchor->have = true;
            } else if (user != m_anchor->first) {
                m_anchor->changed.store(true);
                m_userChanged = true;
                m_guard.reset(); // not a moment longer as somebody else
                return Status::UserChanged;
            }
            return Status::Ok;
        }
        if (!m_haveUser) {
            m_firstUser = user;
            m_haveUser = true;
        } else if (user != m_firstUser) {
            m_userChanged = true;
            m_guard.reset(); // not a moment longer as somebody else
            return Status::UserChanged;
        }
    }
    return Status::Ok;
}

// --- TaskPool ----------------------------------------------------------------------------------

struct TaskPool::State {
    std::mutex mutex;
    std::condition_variable wake;  // a task, or the end
    std::condition_variable idle;  // a thread finished its task or exited
    std::deque<std::function<void()>> tasks;
    int threads = 0;
    int wanted = 0;
    int busy = 0;
    bool stopping = false;
};

TaskPool::TaskPool(int threads) : m_state(std::make_shared<State>())
{
    m_state->wanted = threads;
}

TaskPool::~TaskPool()
{
    shutdown(std::chrono::steady_clock::now());
}

void TaskPool::post(std::function<void()> task)
{
    const std::shared_ptr<State> state = m_state;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping)
        return;
    state->tasks.push_back(std::move(task));
    if (state->threads - state->busy < int(state->tasks.size()) && state->threads < state->wanted) {
        ++state->threads;
        std::thread([state] {
            std::unique_lock<std::mutex> lock(state->mutex);
            for (;;) {
                state->wake.wait(lock, [&] { return state->stopping || !state->tasks.empty(); });
                if (state->stopping)
                    break;
                std::function<void()> next = std::move(state->tasks.front());
                state->tasks.pop_front();
                ++state->busy;
                lock.unlock();
                next();
                next = nullptr; // its captures go before the pool is told it is idle
                lock.lock();
                --state->busy;
                state->idle.notify_all();
            }
            --state->threads;
            state->idle.notify_all();
        }).detach();
    }
    state->wake.notify_one();
}

void TaskPool::shutdown(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(m_state->mutex);
    m_state->stopping = true;
    m_state->tasks.clear();
    m_state->wake.notify_all();
    m_state->idle.wait_until(lock, deadline, [this] { return m_state->threads == 0; });
}

} // namespace df::detail
