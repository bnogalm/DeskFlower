#include "agentwatch.h"

#include "clock.h"

#include <algorithm>

namespace df {

AgentWatch::AgentWatch(std::function<void()> relaunch, Clock clock)
    : m_relaunch(std::move(relaunch)), m_clock(std::move(clock))
{
}

qint64 AgentWatch::now() const { return m_clock ? m_clock() : nowMs(); }

void AgentWatch::setUserPresent(bool on)
{
    if (m_userPresent == on)
        return;
    m_userPresent = on;
    m_missingSince = -1; // a fresh session gets the grace period again
}

void AgentWatch::setAgentConnected(bool on)
{
    if (m_agentConnected == on)
        return;
    m_agentConnected = on;
    if (on) {
        m_agentSeen = true;
        m_connectedAt = now();
    } else {
        m_connectedAt = -1;
        m_missingSince = -1;
    }
}

void AgentWatch::userChanged()
{
    m_agentSeen = m_agentConnected;
    m_missingSince = -1;
    m_retryMs = kFirstRetryMs;
    m_nextAttempt = now() + kSignInGraceMs;
}

void AgentWatch::check()
{
    const qint64 t = now();
    if (m_agentConnected) {
        if (m_connectedAt >= 0 && t - m_connectedAt >= kStableMs) {
            m_retryMs = kFirstRetryMs; // it lives: a later loss starts the back-off over
            m_nextAttempt = 0;
        }
        return;
    }
    if (!m_userPresent || !armed())
        return;
    if (m_missingSince < 0)
        m_missingSince = t;
    if (t - m_missingSince < kGraceMs || t < m_nextAttempt)
        return;
    ++m_relaunches;
    m_nextAttempt = t + m_retryMs;
    m_retryMs = std::min(m_retryMs * 2, kMaxRetryMs);
    if (m_relaunch)
        m_relaunch();
}

} // namespace df
