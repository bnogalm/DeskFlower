#pragma once

// Windows: one streaming paste seen from the agent (ClipboardBridge::PasteSessionBase).
//
// A session runs the paste's request to the service on a thread of its own (`run`: a streaming
// clipFetch on a connection of its own, see Backend), and holds what the streams it serves need:
// the folder the files arrive in and how the transfer stands. Its owners (shared_ptr) are the
// clipboard object while the session is usable and every stream made from it; when the last one
// goes, the request is stopped and its connection closed (the service then cancels a transfer
// nobody else waits for, and stops keeping its folder).
//
// Abandon: while the transfer runs, a session nobody reads (no Read in progress, and no Read or
// stream request for kAbandonMs) gives it up: it stops its request and ends as Abandoned. This
// covers a Cancel in Explorer between two files, a stream released early, an Explorer that crashed,
// and a dialog left open in the middle of a paste (its click is a fresh paste: the next stream
// request opens a new session). A finished transfer is never given up: its folder stays pinned
// for the streams until the session goes.
//
// Quitting: a session's thread runs Qt code (its request's socket, logging) for as long as the
// session lives, and a stream Explorer still holds can keep it alive past the agent's end. Before
// the application object goes, stopAll() stops every live session's request and joins its thread;
// what is left of those sessions then only answers their streams (which fail once
// ClipboardBridge::fetchesAbandoned()).

#include "clipboardbridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class PasteSession final : public ClipboardBridge::PasteSessionBase
{
public:
    static constexpr int kAbandonMs = 60 * 1000;

    // What the request reports, on the session's thread.
    class Link
    {
    public:
        // The request should end now (the session is going, or nobody reads): it closes its
        // connection and returns. Checked at least every few hundred milliseconds.
        bool stopRequested();
        // The folder the files arrive in is known (it may not exist yet).
        void started(const QString &root);
        // The transfer ended: Succeeded, Failed (error), Cancelled. Only the first report counts.
        void finished(State state, const QString &error = {});
        // The session's paste moved the files away (consumed()): the request tells the service
        // before it closes its connection.
        bool consumeRequested();

    private:
        friend class PasteSession;
        explicit Link(PasteSession *session) : m_session(session) {}
        PasteSession *m_session;
    };
    // The request (blocking, on the session's thread): reports through the link and returns when
    // the transfer failed, its connection ended, or stopRequested() turned true. A session that
    // returned without finishing failed.
    using Run = std::function<void(Link &link)>;

    // Starts a session and waits until its folder is known: the session, or null + error if the
    // request failed or was refused first (ClipboardBridge::kNotAPaste for want of a paste).
    // Blocking: called on a worker thread (runWhilePumping). abandonMs: shortened by the tests.
    // shownByShell: false for a move paste (Explorer shows no progress of its own while it
    // waits for CF_HDROP): its transfer is shown in DeskFlower's window (not in liveRoots()).
    static std::shared_ptr<PasteSession> open(Run run, QString *error, int abandonMs = kAbandonMs,
                                              bool shownByShell = true);
    ~PasteSession() override;
    // Stops the request of every live session and waits for their threads (the application quits,
    // see above). A session keeps its state (a finished transfer stays Succeeded); one still
    // running ends as Failed. On the application's thread, after ClipboardBridge::abandonFetches().
    static void stopAll();
    // The folders of the live sessions whose folder is known (whatever their state), move pastes
    // aside: transfers that a file manager shows itself (DeskFlower's progress window leaves them
    // out). Any thread.
    static QStringList liveRoots();
    // The folders of the live move pastes' sessions (see open): their copies are moved away by
    // the paste, so DeskFlower's window offers no "Open folder" for them. Any thread.
    static QStringList liveMoveRoots();
    // Called (on the session's thread, with no lock held) each time a session's folder becomes
    // known, so that what liveRoots() answered before can be asked again (the transfer's first
    // report may come before it). Empty: none. Any thread; the hook must only post elsewhere.
    static void setStartedHook(std::function<void()> hook);
    // Whether any live session (move pastes included) has a Read in progress, or had a Read or a
    // stream request in the last `ms` milliseconds: a file manager is still copying a paste (it may
    // lag well behind the transfer, which may be over). Any thread.
    static bool activeWithin(int ms);

    QString root() const override;
    State state() const override;
    void readStarted() override;
    void readEnded() override;
    void touch() override;
    void consumed() override;
    QString error() const;

private:
    PasteSession(int abandonMs, bool shownByShell);
    static QStringList roots(bool shownByShell);
    bool stopRequested();
    void started(const QString &root);
    void finished(State state, const QString &error);
    void runEnded();
    void stopAndJoin();

    using Clock = std::chrono::steady_clock;
    const int m_abandonMs;
    const bool m_shownByShell;
    std::atomic<bool> m_consumed{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_started = false; // root is known
    bool m_runEnded = false;
    State m_state = State::Running;
    QString m_root;
    QString m_error;
    int m_reading = 0; // Reads in progress
    Clock::time_point m_lastActivity;
    std::atomic<bool> m_stop{false};
    std::mutex m_joinMutex; // the owner's destructor and stopAll() may both join
    std::thread m_thread;
};
