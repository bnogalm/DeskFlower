#pragma once

// Windows service mode: the agent is the process that owns the user's clipboard (and shows
// prompts, drags files...). If it dies, nothing is copied or pasted and nothing says so. The
// service keeps one running in the console session: when a user is signed in (the session helper
// is up) and no agent has been connected for kGraceMs, it starts one (platform::relaunchAgent,
// which does nothing if the process is there already), retrying with exponential back-off from
// kFirstRetryMs up to kMaxRetryMs. The back-off starts over once an agent stayed connected for
// kStableMs, so an agent that crashes at once is not restarted in a tight loop.
//
// The watch is armed only when the agent should run: it starts at every sign-in (the user's
// autostart choice), or one was connected in this user's session (it went away without the user
// asking: quitting from the tray stops the service with it). Right after a sign-in it waits
// kSignInGraceMs instead: Windows starts the Run key's programs (the agent) late after a sign-in,
// and an agent started meanwhile would be a second one (the agent also refuses to run twice).

#include <QtGlobal>
#include <functional>

namespace df {

class AgentWatch
{
public:
    static constexpr qint64 kGraceMs = 5000;
    static constexpr qint64 kSignInGraceMs = 30000;
    static constexpr qint64 kFirstRetryMs = 5000;
    static constexpr qint64 kMaxRetryMs = 5 * 60 * 1000;
    static constexpr qint64 kStableMs = 60 * 1000;
    // How often the service calls check().
    static constexpr int kCheckIntervalMs = 2000;

    using Clock = std::function<qint64()>;
    explicit AgentWatch(std::function<void()> relaunch, Clock clock = {});

    // The agent starts at every sign-in (autostart).
    void setAutostart(bool on) { m_autostart = on; }
    // A user is signed in at the console (the session helper is connected).
    void setUserPresent(bool on);
    // At least one agent is connected.
    void setAgentConnected(bool on);
    // The console user changed: whether an agent ran for the previous one says nothing now, and
    // the new one's agent is given kSignInGraceMs to start by itself.
    void userChanged();
    // Starts an agent if one is due (called every kCheckIntervalMs).
    void check();

    int relaunches() const { return m_relaunches; }

private:
    qint64 now() const;
    bool armed() const { return m_autostart || m_agentSeen; }

    std::function<void()> m_relaunch;
    Clock m_clock;
    bool m_autostart = true;
    bool m_userPresent = false;
    bool m_agentConnected = false;
    bool m_agentSeen = false;     // an agent was connected in this user's session
    qint64 m_connectedAt = -1;    // since when an agent is connected
    qint64 m_missingSince = -1;   // since when none is (while a user is present)
    qint64 m_nextAttempt = 0;
    qint64 m_retryMs = kFirstRetryMs;
    int m_relaunches = 0;
};

} // namespace df
