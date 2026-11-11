#pragma once

// Requests the service sends to one or more of its agents (IPC clients), answered by the first of
// them to reply. Each request remembers which clients it was sent to: only those may answer it,
// and once every one of them has disconnected it is answered at once (empty) instead of waiting
// for its timeout, so that, e.g., a drag shown by an agent that crashed ends straight away.

#include <QCborMap>
#include <QHash>
#include <QList>
#include <QSet>
#include <functional>
#include <utility>

namespace df {

template <typename Client>
class AgentRequests
{
public:
    using Done = std::function<void(const QCborMap &)>;

    qint64 add(Done done, const QList<Client *> &asked)
    {
        const qint64 id = ++m_seq;
        m_requests.insert(id, Request{std::move(done), QSet<Client *>(asked.begin(), asked.end())});
        return id;
    }

    // A reply from `from`: answers request `id` if it was asked to that client (the first wins).
    bool reply(qint64 id, Client *from, const QCborMap &answer)
    {
        const auto it = m_requests.find(id);
        if (it == m_requests.end() || !it->asked.contains(from))
            return false;
        const Done done = std::move(it->done);
        m_requests.erase(it);
        done(answer);
        return true;
    }

    // No reply in time: answered empty (nothing if it was answered meanwhile).
    void expire(qint64 id)
    {
        const auto it = m_requests.find(id);
        if (it == m_requests.end())
            return;
        const Done done = std::move(it->done);
        m_requests.erase(it);
        done({});
    }

    // `client` went away: the requests only it could still answer are answered empty.
    void disconnected(Client *client)
    {
        QList<Done> orphans;
        for (auto it = m_requests.begin(); it != m_requests.end();) {
            if (it->asked.remove(client) && it->asked.isEmpty()) {
                orphans.append(std::move(it->done));
                it = m_requests.erase(it);
            } else {
                ++it;
            }
        }
        for (const Done &done : orphans)
            done({});
    }

    bool isPending(qint64 id) const { return m_requests.contains(id); }

private:
    struct Request {
        Done done;
        QSet<Client *> asked; // still connected
    };
    qint64 m_seq = 0;
    QHash<qint64, Request> m_requests;
};

} // namespace df
