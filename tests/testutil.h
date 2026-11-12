#pragma once

#include "node.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace df::test {

// Runs a real event loop until `done` holds or `timeoutMs` passes (true if it held). Unlike
// QTest::qWaitFor, which sleeps 10 ms between rounds of events (more on macOS), it does not cap a
// transfer at one round trip per sleep: a stream moving 64 KB frames ran at about 6 MB/s there.
inline bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done())
            loop.quit();
    });
    poll.start(1);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return done();
}

// A directory junction (Windows; no rights needed), as a user could plant one; a symbolic link
// elsewhere.
inline bool makeJunction(const QString &link, const QString &target)
{
#ifdef Q_OS_WIN
    return QProcess::execute(QStringLiteral("cmd.exe"),
                             {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                              QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)})
           == 0;
#else
    return QFile::link(target, link);
#endif
}

#ifdef Q_OS_WIN
// A Windows shortcut (.lnk) to `target`, made by the shell as Explorer would.
inline bool makeShortcut(const QString &lnk, const QString &target)
{
    const QString script =
        QStringLiteral("$s = (New-Object -ComObject WScript.Shell).CreateShortcut('%1'); $s.TargetPath = '%2'; $s.Save()")
            .arg(QDir::toNativeSeparators(lnk), QDir::toNativeSeparators(target));
    return QProcess::execute(QStringLiteral("powershell.exe"),
                             {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
                              script})
               == 0
           && QFileInfo::exists(lnk);
}
#endif

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

// A node bound to its own loopback address (127.0.0.x: every one reaches every other), so that
// tests can tell nodes apart by address and put simulated routers between them (natproxy.h).
// `tweak` adjusts the options (hooks); `port` 0: any.
inline std::unique_ptr<Node> makeNodeAt(const QString &dir, const QString &address,
                                        const std::function<void(Node::Options &)> &tweak = {}, quint16 port = 0)
{
    Node::Options o;
    o.configDir = dir;
    o.dataPort = port;
    o.enableDiscovery = false;
    o.bindAddress = QHostAddress(address);
    if (tweak)
        tweak(o);
    auto node = std::make_unique<Node>(o);
    QString error;
    if (!node->start(&error))
        qWarning() << error;
    return node;
}

inline std::optional<PeerView> viewOf(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return p;
    }
    return std::nullopt;
}

inline bool knows(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return true;
    }
    return false;
}

// `node` is connected to `id` and has its hello (name, version, ports): what the connection is
// used for (the clipboard refuses a peer whose version it does not know yet) may rely on it.
inline bool connectedWithHello(const Node &node, const QByteArray &id)
{
    for (const PeerView &p : node.peers()) {
        if (p.id == id)
            return p.connected && !p.version.isEmpty();
    }
    return false;
}

// Pairs a with b over localhost (a asks, b shows the PIN) and waits until both are connected and
// have each other's hello (the version it carries gates the clipboard). levelA: what b becomes
// on a; levelB: what a becomes on b, chosen while b shows the PIN (as its user would); not given:
// nothing is chosen on that side (the Node's default applies: on b, a's suggestion). Returns
// false on failure.
// bAddress, bPort: where a reaches b (the address b is bound to, or a router in front of it;
// port 0: b's own).
inline bool pairNodes(Node &a, Node &b, std::optional<TrustLevel> levelA = std::nullopt,
                      std::optional<TrustLevel> levelB = std::nullopt,
                      const QHostAddress &bAddress = QHostAddress(QHostAddress::LocalHost), quint16 bPort = 0)
{
    b.setExploring(true);
    a.addManualPeer(bAddress, bPort ? bPort : b.dataPort());
    if (!QTest::qWaitFor([&] { return knows(a, b.id()); }, 5000))
        return false;
    QSignalSpy pinShown(&b, &Node::pairingPinGenerated);
    QSignalSpy pinAsked(&a, &Node::pairingPinRequired);
    const bool requested = levelA ? a.requestPairing(b.id(), *levelA) : a.requestPairing(b.id());
    if (!requested || !QTest::qWaitFor([&] { return pinShown.size() && pinAsked.size(); }, 5000))
        return false;
    if (levelB && !b.setPairTrust(a.id(), *levelB))
        return false;
    if (!a.submitPin(b.id(), pinShown.first().at(2).toString()))
        return false;
    return QTest::qWaitFor([&] { return connectedWithHello(a, b.id()) && connectedWithHello(b, a.id()); }, 10000);
}

} // namespace df::test
