#pragma once

#include "node.h"

#include <QSignalSpy>
#include <QTest>
#include <memory>

namespace df::test {

inline std::unique_ptr<Node> makeNode(const QString &dir)
{
    Node::Options o;
    o.configDir = dir;
    o.dataPort = 0;
    o.enableDiscovery = false;
    auto node = std::make_unique<Node>(o);
    QString error;
    if (!node->start(&error))
        qWarning() << error;
    return node;
}

inline bool knows(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return true;
    }
    return false;
}

// Pairs a with b over localhost and waits until both are connected. Returns false on failure.
inline bool pairNodes(Node &a, Node &b)
{
    b.setExploring(true);
    a.addManualPeer(QHostAddress::LocalHost, b.dataPort());
    if (!QTest::qWaitFor([&] { return knows(a, b.id()); }, 5000))
        return false;
    QSignalSpy pinShown(&b, &Node::pairingPinGenerated);
    QSignalSpy pinAsked(&a, &Node::pairingPinRequired);
    if (!a.requestPairing(b.id()) || !QTest::qWaitFor([&] { return pinShown.size() && pinAsked.size(); }, 5000))
        return false;
    if (!a.submitPin(b.id(), pinShown.first().at(2).toString()))
        return false;
    return QTest::qWaitFor([&] { return a.isConnected(b.id()) && b.isConnected(a.id()); }, 10000);
}

} // namespace df::test
