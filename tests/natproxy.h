#pragma once

// A simulated router with NAT for in-process tests, between nodes bound to distinct 127.0.0.x
// addresses (Node::Options::bindAddress). Nodes inside route the datagrams they send to the
// outside through it (Options::datagramHook, see insideHook): it maps each inside node to one port
// of its public address (endpoint-independent mapping, as most home routers do), sends from that
// port, and hands back only what comes from where the mapping sent to (port-restricted cone), or
// from anyone with FullCone. Idle mappings are forgotten after the timeout (the next packet gets a
// new public port), newMappings() forgets them all (a router restart), and a blackout drops
// everything. A datagram from inside to the public address itself is delivered to the inside node
// mapped there, as from the sender's mapping (hairpin NAT). TCP is never forwarded inward: tests
// keep outside nodes from connecting in with Options::connectHook (outsideHooks).
//
// Everything stays on loopback, where every 127.0.0.x reaches every other: the hooks are what
// makes the private addresses unreachable from outside.

#include "node.h"

#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QObject>
#include <QSet>
#include <QUdpSocket>
#include <functional>
#include <map>
#include <memory>

namespace df::test {

class NatProxy : public QObject
{
public:
    enum class Filtering { PortRestricted, FullCone };

    // The node behind the router that an inside hook stands for (set it once the node exists,
    // and again when the test restarts it: the mapping stays, as a router's would for the same
    // address and port).
    using Slot = std::shared_ptr<Node *>;

    explicit NatProxy(const QHostAddress &publicAddress, Filtering filtering = Filtering::PortRestricted,
                      int mappingTimeoutMs = 30000)
        : m_public(publicAddress), m_filtering(filtering), m_timeoutMs(mappingTimeoutMs)
    {
        m_clock.start();
    }

    QHostAddress publicAddress() const { return m_public; }
    // The public port of the mapping of `slot` (0: none now).
    quint16 publicPort(const Slot &slot) const
    {
        auto it = m_mappings.find(slot.get());
        return it == m_mappings.end() ? 0 : it->second->socket->localPort();
    }
    int dropped() const { return m_dropped; }

    // Options::datagramHook for a node behind the router: datagrams to `lan` addresses go out of
    // its own socket (same network), the rest through the NAT.
    std::function<bool(const QByteArray &, const QHostAddress &, quint16)> insideHook(const Slot &slot,
                                                                                       QList<QHostAddress> lan)
    {
        return [this, slot, lan](const QByteArray &datagram, const QHostAddress &to, quint16 port) {
            if (lan.contains(to))
                return false;
            outbound(slot, datagram, to, port);
            return true;
        };
    }

    void newMappings() { m_mappings.clear(); }
    void setBlackout(bool on) { m_blackout = on; }

private:
    struct Mapping {
        Slot slot;
        std::unique_ptr<QUdpSocket> socket;
        QSet<QPair<QString, quint16>> allowed; // where it sent to
        qint64 lastActivity = 0;
    };

    Mapping &mappingFor(const Slot &slot)
    {
        const qint64 now = m_clock.elapsed();
        auto it = m_mappings.find(slot.get());
        if (it != m_mappings.end() && now - it->second->lastActivity > m_timeoutMs)
            m_mappings.erase(it), it = m_mappings.end(); // expired: the next one gets a new port
        if (it == m_mappings.end()) {
            auto m = std::make_unique<Mapping>();
            m->slot = slot;
            m->socket = std::make_unique<QUdpSocket>();
            m->socket->bind(m_public, 0);
            Mapping *raw = m.get();
            QObject::connect(m->socket.get(), &QUdpSocket::readyRead, this, [this, raw] { inbound(*raw); });
            it = m_mappings.emplace(slot.get(), std::move(m)).first;
        }
        it->second->lastActivity = now;
        return *it->second;
    }

    void outbound(const Slot &slot, const QByteArray &datagram, const QHostAddress &to, quint16 port)
    {
        if (m_blackout)
            return;
        Mapping &m = mappingFor(slot);
        m.allowed.insert({to.toString(), port});
        if (to == m_public) {
            // Hairpin: to the inside node mapped at that public port, as from this mapping. Later,
            // not within the sender's own call (as the network would).
            const quint16 from = m.socket->localPort();
            for (auto &[key, target] : m_mappings) {
                if (target->socket->localPort() != port)
                    continue;
                if (m_filtering == Filtering::PortRestricted && !target->allowed.contains({m_public.toString(), from})) {
                    ++m_dropped;
                    return;
                }
                const Slot targetSlot = target->slot;
                const QHostAddress source = m_public;
                QMetaObject::invokeMethod(
                    this,
                    [targetSlot, datagram, source, from] {
                        if (*targetSlot)
                            (*targetSlot)->injectDatagram(datagram, source, from);
                    },
                    Qt::QueuedConnection);
                return;
            }
            ++m_dropped;
            return;
        }
        m.socket->writeDatagram(datagram, to, port);
    }

    void inbound(Mapping &m)
    {
        while (m.socket->hasPendingDatagrams()) {
            const QNetworkDatagram d = m.socket->receiveDatagram(65536);
            const QHostAddress sender(d.senderAddress().toIPv4Address());
            const quint16 senderPort = quint16(d.senderPort());
            if (m_blackout || m_clock.elapsed() - m.lastActivity > m_timeoutMs
                || (m_filtering == Filtering::PortRestricted && !m.allowed.contains({sender.toString(), senderPort}))) {
                ++m_dropped;
                continue;
            }
            m.lastActivity = m_clock.elapsed();
            if (*m.slot)
                (*m.slot)->injectDatagram(d.data(), sender, senderPort);
        }
    }

    QHostAddress m_public;
    Filtering m_filtering;
    int m_timeoutMs;
    QElapsedTimer m_clock;
    std::map<Node **, std::unique_ptr<Mapping>> m_mappings;
    bool m_blackout = false;
    int m_dropped = 0;
};

// What a node outside a NAT can reach: it drops datagrams to the private addresses behind the
// router (unroutable from its network) and connects TCP to none of those, nor to the router's
// public address (no port forwarding).
inline void outsideHooks(Node::Options &o, const QList<QHostAddress> &privateAddresses, const QHostAddress &router)
{
    o.datagramHook = [privateAddresses](const QByteArray &, const QHostAddress &to, quint16) {
        return privateAddresses.contains(to);
    };
    o.connectHook = [privateAddresses, router](const QHostAddress &to, quint16) {
        return !privateAddresses.contains(to) && to != router;
    };
}

} // namespace df::test
