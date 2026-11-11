#include "pastesession_win.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <unordered_set>

Q_LOGGING_CATEGORY(lcPasteSession, "deskflower.clipboard.virtual", QtInfoMsg)

namespace {
// The live sessions (for stopAll). Never destroyed: a session may go after static destruction (a
// stream Explorer releases while the process ends).
struct Registry {
    std::mutex mutex;
    std::unordered_set<PasteSession *> sessions;
};
Registry &registry()
{
    static Registry *r = new Registry;
    return *r;
}
// setStartedHook's hook. A lock of its own: stopAll() joins threads with the registry's held, and
// a joined thread may be in started().
struct StartedHook {
    std::mutex mutex;
    std::function<void()> hook;
};
StartedHook &startedHook()
{
    static StartedHook *h = new StartedHook;
    return *h;
}
} // namespace

bool PasteSession::Link::stopRequested() { return m_session->stopRequested(); }

void PasteSession::Link::started(const QString &root) { m_session->started(root); }

void PasteSession::Link::finished(State state, const QString &error) { m_session->finished(state, error); }

bool PasteSession::Link::consumeRequested() { return m_session->m_consumed; }

PasteSession::PasteSession(int abandonMs, bool shownByShell)
    : m_abandonMs(abandonMs), m_shownByShell(shownByShell), m_lastActivity(Clock::now())
{
}

std::shared_ptr<PasteSession> PasteSession::open(Run run, QString *error, int abandonMs, bool shownByShell)
{
    std::shared_ptr<PasteSession> session(new PasteSession(abandonMs, shownByShell));
    // The thread holds no owner: the last owner's destructor stops and joins it.
    PasteSession *raw = session.get();
    {
        // Registered with its thread: stopAll() sees every thread there is.
        std::lock_guard<std::mutex> lock(registry().mutex);
        session->m_thread = std::thread([raw, run = std::move(run)] {
            Link link(raw);
            run(link);
            raw->runEnded();
        });
        registry().sessions.insert(raw);
    }
    std::unique_lock<std::mutex> lock(session->m_mutex);
    session->m_changed.wait(lock, [raw] { return raw->m_started || raw->m_state != State::Running || raw->m_runEnded; });
    if (session->m_state != State::Running && session->m_state != State::Succeeded) {
        if (error)
            *error = session->m_error;
        lock.unlock();
        return {}; // the destructor stops the request
    }
    if (!session->m_started) {
        if (error)
            *error = session->m_error.isEmpty()
                         ? QCoreApplication::translate("PasteSession", "The transfer did not start")
                         : session->m_error;
        lock.unlock();
        return {};
    }
    session->m_lastActivity = Clock::now(); // the abandon time counts from here
    return session;
}

PasteSession::~PasteSession()
{
    {
        // Waits for a stopAll() in progress (which may be joining this session's thread).
        std::lock_guard<std::mutex> lock(registry().mutex);
        registry().sessions.erase(this);
    }
    stopAndJoin();
}

void PasteSession::stopAll()
{
    std::lock_guard<std::mutex> lock(registry().mutex);
    for (PasteSession *session : registry().sessions)
        session->m_stop = true; // all at once: each thread notices within its next slice
    for (PasteSession *session : registry().sessions)
        session->stopAndJoin();
}

void PasteSession::setStartedHook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(startedHook().mutex);
    startedHook().hook = std::move(hook);
}

bool PasteSession::activeWithin(int ms)
{
    // Registry, then each session's lock (the order roots() takes them in too).
    std::lock_guard<std::mutex> lock(registry().mutex);
    const Clock::time_point now = Clock::now();
    for (PasteSession *session : registry().sessions) {
        std::lock_guard<std::mutex> sessionLock(session->m_mutex);
        if (session->m_reading > 0 || now - session->m_lastActivity < std::chrono::milliseconds(ms))
            return true;
    }
    return false;
}

QStringList PasteSession::liveRoots() { return roots(true); }

QStringList PasteSession::liveMoveRoots() { return roots(false); }

QStringList PasteSession::roots(bool shownByShell)
{
    // A session being destroyed waits for this lock before it leaves the registry: every session
    // listed here is whole.
    std::lock_guard<std::mutex> lock(registry().mutex);
    QStringList roots;
    for (PasteSession *session : registry().sessions) {
        if (session->m_shownByShell != shownByShell)
            continue;
        const QString root = session->root();
        if (!root.isEmpty())
            roots.append(root);
    }
    return roots;
}

void PasteSession::stopAndJoin()
{
    m_stop = true;
    std::lock_guard<std::mutex> lock(m_joinMutex);
    if (m_thread.joinable()) {
        if (m_thread.get_id() == std::this_thread::get_id())
            m_thread.detach(); // cannot happen (the thread owns nothing), but never join oneself
        else
            m_thread.join();
    }
}

QString PasteSession::root() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_root;
}

PasteSession::State PasteSession::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

QString PasteSession::error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void PasteSession::readStarted()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_reading;
    m_lastActivity = Clock::now();
}

void PasteSession::readEnded()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    --m_reading;
    m_lastActivity = Clock::now();
}

void PasteSession::touch()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastActivity = Clock::now();
}

void PasteSession::consumed() { m_consumed = true; }

bool PasteSession::stopRequested()
{
    if (m_stop)
        return true;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started && m_state == State::Running && m_reading == 0
        && Clock::now() - m_lastActivity >= std::chrono::milliseconds(m_abandonMs)) {
        qCInfo(lcPasteSession) << "Nobody reads the pasted files any more: their transfer is given up";
        m_state = State::Abandoned;
        m_error = QCoreApplication::translate("PasteSession", "The paste was given up");
        m_stop = true;
        m_changed.notify_all();
        return true;
    }
    return false;
}

void PasteSession::started(const QString &root)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started)
            return;
        m_root = root;
        m_started = true;
        m_lastActivity = Clock::now();
        m_changed.notify_all();
    }
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(startedHook().mutex);
        hook = startedHook().hook;
    }
    if (hook)
        hook();
}

void PasteSession::finished(State state, const QString &error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Running || state == State::Running)
        return;
    m_state = state;
    m_error = error;
    if (state == State::Succeeded)
        qCInfo(lcPasteSession) << "Every pasted file has arrived";
    else
        qCInfo(lcPasteSession) << "The paste's transfer ended:" << error;
    m_changed.notify_all();
}

void PasteSession::runEnded()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_runEnded = true;
    if (m_state == State::Running) {
        m_state = State::Failed;
        if (m_error.isEmpty())
            m_error = QCoreApplication::translate("PasteSession", "The connection to the DeskFlower service ended");
    }
    m_changed.notify_all();
}
