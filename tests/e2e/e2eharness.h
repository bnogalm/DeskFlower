#pragma once

// Black-box end-to-end harness: real deskflower-service processes (console mode, no discovery, no
// session helper) driven over their local IPC exactly as the agent and the session helper drive
// them. A fake helper authenticates with DESKFLOWER_SESSION_TOKEN, reports one screen and plays the
// user's mouse and keyboard (edge, mm, mb, activity, clip); a fake agent pairs, pastes (clipUnseal,
// clipFetch) and answers the service's drag questions (dragCatch, dragHover, dropFolder, recycle).
// Everything goes through the same CBOR frames (ipc::Connection) as the real programs, so the
// service's own code (ServiceController, IPC, impersonation hooks) is what is tested.
//
// NatRouter puts one service behind a simulated router (port forward + masquerade) on 127.0.0.2/3,
// for UDP and for the TCP file streams. Every socket here, and every service (DESKFLOWER_BIND_ADDRESS),
// listens on loopback only: nothing is reachable from the network and no firewall prompt appears.

#include "ipc.h"

#include <QCborArray>
#include <QCborMap>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocalSocket>
#include <QMap>
#include <QNetworkDatagram>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <csignal>
#include <sys/prctl.h>
#endif

namespace df::e2e {

inline QString hex(const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex()); }

// The deskflower-service built with the tests: DESKFLOWER_SERVICE_EXE, else next to the test
// program (the build puts every executable in one folder), else the path CMake recorded.
inline QString serviceExecutable()
{
    const QString env = qEnvironmentVariable("DESKFLOWER_SERVICE_EXE");
    if (!env.isEmpty())
        return env;
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("deskflower-service.exe");
#else
    const QString name = QStringLiteral("deskflower-service");
#endif
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (QFileInfo::exists(beside))
        return beside;
#ifdef DF_SERVICE_EXE
    return QStringLiteral(DF_SERVICE_EXE);
#else
    return beside;
#endif
}

// A port free for both UDP and TCP right now (the service binds both to the same number). A
// concurrent run may still take it: the service then picks another one, which the state reports.
inline quint16 freePort()
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        QUdpSocket udp;
        if (!udp.bind(QHostAddress::LocalHost, 0))
            continue;
        QTcpServer tcp;
        if (tcp.listen(QHostAddress::LocalHost, udp.localPort()))
            return udp.localPort();
    }
    return 0;
}

// The services die with the test program, even when it crashes or is killed: no service is left
// running (Windows: a job that kills its processes when its last handle closes; Linux: the
// parent-death signal; elsewhere a crash may leave one behind).
inline void tieToThisProcess(QProcess &proc)
{
#ifdef Q_OS_WIN
    static const HANDLE job = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (h)
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return h;
    }();
    if (job && proc.processId()) {
        if (HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(proc.processId()))) {
            AssignProcessToJobObject(job, h);
            CloseHandle(h);
        }
    }
#else
    Q_UNUSED(proc); // Linux: PR_SET_PDEATHSIG in Service::start
#endif
}

// One IPC connection to a service, as an agent or as the session helper. Received messages are
// kept in order until a wait consumes them, so a test never misses an event that arrived before it
// started waiting. Handlers answer requests (dragCatch...) as they arrive, like a real agent.
class IpcClient : public QObject
{
public:
    explicit IpcClient(QString label) : m_label(std::move(label)) {}

    bool open(const QString &serverName, int timeoutMs, QString *error)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            m_socket = std::make_unique<QLocalSocket>();
            m_socket->connectToServer(serverName);
            if (m_socket->waitForConnected(500)) {
                m_conn = std::make_unique<ipc::Connection>(m_socket.get());
                connect(m_conn.get(), &ipc::Connection::messageReceived, this, &IpcClient::onMessage);
                return true;
            }
            QTest::qWait(100);
        }
        *error = QStringLiteral("%1: cannot connect to %2: %3").arg(m_label, serverName, m_socket->errorString());
        return false;
    }
    void close()
    {
        m_conn.reset();
        if (m_socket)
            m_socket->abort();
        m_socket.reset();
        m_inbox.clear();
        m_state = QCborMap(); // a restarted service has another port (and must not look connected)
    }
    bool isConnected() const { return m_socket && m_socket->state() == QLocalSocket::ConnectedState; }

    void send(const QString &cmd, QCborMap args = {})
    {
        if (!m_conn)
            return;
        args.insert(QStringLiteral("cmd"), cmd);
        m_conn->send(args);
        m_socket->flush();
    }

    using Predicate = std::function<bool(const QCborMap &)>;
    // The first unconsumed event `ev` (matching `pred`), waited for up to timeoutMs, and consumed.
    std::optional<QCborMap> waitEvent(const QString &ev, int timeoutMs, const Predicate &pred = {})
    {
        std::optional<QCborMap> found;
        QTest::qWaitFor(
            [&] {
                for (qsizetype i = 0; i < m_inbox.size(); ++i) {
                    const QCborMap &m = m_inbox.at(i);
                    if (m.value(QStringLiteral("ev")).toString() == ev && (!pred || pred(m))) {
                        found = m;
                        m_inbox.removeAt(i);
                        return true;
                    }
                }
                return false;
            },
            timeoutMs);
        return found;
    }
    // Every unconsumed event `ev`, consumed.
    QList<QCborMap> takeAll(const QString &ev)
    {
        QList<QCborMap> out;
        for (qsizetype i = 0; i < m_inbox.size();) {
            if (m_inbox.at(i).value(QStringLiteral("ev")).toString() == ev)
                out.append(m_inbox.takeAt(i));
            else
                ++i;
        }
        return out;
    }
    void discard(const QString &ev) { takeAll(ev); }
    // Requests of this kind are answered by `handler` as they arrive (not kept in the inbox).
    void onRequest(const QString &ev, std::function<void(const QCborMap &)> handler)
    {
        if (handler)
            m_handlers.insert(ev, std::move(handler));
        else
            m_handlers.remove(ev);
    }
    void reply(const QCborMap &request, QCborMap answer)
    {
        answer.insert(QStringLiteral("rid"), request.value(QStringLiteral("rid")));
        send(QStringLiteral("agentReply"), answer);
    }

    QCborMap lastState() const { return m_state; }
    // How many states arrived so far (to tell a fresh one from an old one).
    quint64 stateCount() const { return m_states; }
    // What it received lately, for failure messages.
    QString recent() const { return m_recent.join(QLatin1Char(' ')); }

private:
    void onMessage(const QCborMap &m)
    {
        const QString ev = m.value(QStringLiteral("ev")).toString();
        if (ev != QLatin1String("thumb") && ev != QLatin1String("ping") && ev != QLatin1String("mabs")) {
            m_recent.append(ev);
            if (m_recent.size() > 40)
                m_recent.removeFirst();
        }
        if (ev == QLatin1String("state")) {
            m_state = m; // states are not queued: tests look at the latest one
            ++m_states;
            return;
        }
        if (ev == QLatin1String("thumb") || ev == QLatin1String("ping") || ev == QLatin1String("mabs"))
            return; // frequent and irrelevant here
        if (const auto handler = m_handlers.constFind(ev); handler != m_handlers.constEnd()) {
            (*handler)(m);
            return;
        }
        m_inbox.append(m);
    }

    QString m_label;
    std::unique_ptr<QLocalSocket> m_socket;
    std::unique_ptr<ipc::Connection> m_conn;
    QList<QCborMap> m_inbox;
    QHash<QString, std::function<void(const QCborMap &)>> m_handlers;
    QCborMap m_state;
    quint64 m_states = 0;
    QStringList m_recent;
};

// One deskflower-service process with its own instance name, configuration directory and home
// (shared and received files), plus its fake agent and helper connections.
class Service
{
public:
    static constexpr int kScreenWidth = 1920;
    static constexpr int kScreenHeight = 1080;

    Service(const QString &label, const QString &root)
        : m_label(label),
          m_dir(QDir(root).filePath(label)),
          // Unique per run (pipe/socket names are machine-wide): parallel runs never meet.
          m_instance(QStringLiteral("e2e%1%2%3")
                         .arg(QCoreApplication::applicationPid())
                         .arg(QRandomGenerator::global()->bounded(100000))
                         .arg(label)),
          agent(label + QStringLiteral(".agent")),
          helper(label + QStringLiteral(".helper"))
    {
        QDir().mkpath(configDir());
        QDir().mkpath(home());
    }
    ~Service() { kill(); }

    QString label() const { return m_label; }
    QString configDir() const { return QDir(m_dir).filePath(QStringLiteral("config")); }
    QString home() const { return QDir(m_dir).filePath(QStringLiteral("home")); }
    QString logPath() const { return QDir(m_dir).filePath(QStringLiteral("service.log")); }
    QString receivedFolder() const { return QDir(home()).filePath(QStringLiteral("Received")); }

    // Starts the process (port 0: any) and connects the agent and the helper.
    bool start(quint16 port, QString *error)
    {
        m_proc = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DESKFLOWER_SESSION_TOKEN"), QString::fromLatin1(m_token));
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
        // Loopback only, also when the program is run by hand outside ctest.
        if (!env.contains(QStringLiteral("DESKFLOWER_BIND_ADDRESS")))
            env.insert(QStringLiteral("DESKFLOWER_BIND_ADDRESS"), QStringLiteral("127.0.0.1"));
        const QString rules = qEnvironmentVariable("DF_E2E_LOG_RULES");
        env.insert(QStringLiteral("QT_LOGGING_RULES"), rules.isEmpty() ? QStringLiteral("deskflower.*.info=true") : rules);
        m_proc->setProcessEnvironment(env);
        m_proc->setProcessChannelMode(QProcess::MergedChannels);
        m_proc->setStandardOutputFile(logPath(), QIODevice::Append);
#ifdef Q_OS_LINUX
        m_proc->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
#endif
        QFile log(logPath());
        if (log.open(QIODevice::Append))
            log.write(QStringLiteral("==== start %1 port %2\n").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)).arg(port).toUtf8());
        log.close();
        m_proc->start(serviceExecutable(),
                      {QStringLiteral("--instance"), m_instance, QStringLiteral("--config-dir"), configDir(),
                       QStringLiteral("--home"), home(), QStringLiteral("--port"), QString::number(port),
                       QStringLiteral("--no-discovery"), QStringLiteral("--no-session")});
        if (!m_proc->waitForStarted(10000)) {
            *error = QStringLiteral("%1: cannot start %2: %3").arg(m_label, serviceExecutable(), m_proc->errorString());
            return false;
        }
        tieToThisProcess(*m_proc);
        if (!agent.open(ipc::serverName(m_instance), 15000, error) || !helper.open(ipc::serverName(m_instance), 5000, error)) {
            *error += QStringLiteral(" (process %1)").arg(processStatus());
            return false;
        }
        const auto state = waitState([](const QCborMap &s) { return !s.isEmpty(); }, 5000);
        if (!state) {
            *error = QStringLiteral("%1: no state from the service").arg(m_label);
            return false;
        }
        const QCborMap self = state->value(QStringLiteral("self")).toMap();
        idHex = self.value(QStringLiteral("id")).toString();
        id = QByteArray::fromHex(idHex.toLatin1());
        dataPort = quint16(self.value(QStringLiteral("port")).toInteger());
        tcpPort = quint16(state->value(QStringLiteral("ports")).toMap().value(QStringLiteral("tcpInUse")).toInteger());
        version = self.value(QStringLiteral("version")).toString();
        // The agent's version: sealed clipboards are only sent to agents that render them. dnd: it
        // runs native drags of files from other devices (dragHover), as the Windows agent does.
        agent.send(QStringLiteral("agent"), QCborMap{{QStringLiteral("v"), version}, {QStringLiteral("dnd"), true}});
        // The helper: authenticated by the token, with one screen.
        helper.send(QStringLiteral("hello"),
                    QCborMap{{QStringLiteral("role"), QStringLiteral("session")},
                             {QStringLiteral("token"), QString::fromLatin1(m_token)}});
        QCborMap screen{{QStringLiteral("i"), QStringLiteral("S1")}, {QStringLiteral("n"), m_label},
                        {QStringLiteral("x"), 0}, {QStringLiteral("y"), 0},
                        {QStringLiteral("w"), kScreenWidth}, {QStringLiteral("h"), kScreenHeight},
                        {QStringLiteral("s"), 1.0}, {QStringLiteral("p"), true}};
        helper.send(QStringLiteral("localScreens"), QCborMap{{QStringLiteral("screens"), QCborArray{screen}}});
        if (!waitState([](const QCborMap &s) {
                return s.value(QStringLiteral("workspace")).toMap().value(QStringLiteral("session")).toBool();
            }, 5000)) {
            *error = QStringLiteral("%1: the fake session helper was not accepted").arg(m_label);
            return false;
        }
        return true;
    }

    // Asks the service to quit (as the agent's Quit does), then kills it if it does not.
    void stop()
    {
        if (!m_proc)
            return;
        if (m_proc->state() != QProcess::NotRunning) {
            agent.send(QStringLiteral("stopService"));
            if (!m_proc->waitForFinished(5000))
                m_proc->kill();
            m_proc->waitForFinished(5000);
        }
        agent.close();
        helper.close();
        m_proc.reset();
    }
    void kill()
    {
        if (m_proc && m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
            m_proc->waitForFinished(5000);
        }
        agent.close();
        helper.close();
        m_proc.reset();
    }
    bool isRunning() const { return m_proc && m_proc->state() == QProcess::Running; }

    // The first state received from now on that satisfies `pred`, asking for a fresh one regularly
    // (some changes, such as a peer's version arriving, are not pushed at once).
    std::optional<QCborMap> waitState(const std::function<bool(const QCborMap &)> &pred, int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        qint64 asked = -1000;
        const quint64 since = agent.stateCount();
        while (t.elapsed() < timeoutMs) {
            const QCborMap s = agent.lastState();
            if (agent.stateCount() > since && pred(s))
                return s;
            if (t.elapsed() - asked >= 300) {
                agent.send(QStringLiteral("getState"));
                asked = t.elapsed();
            }
            QTest::qWait(30);
        }
        const QCborMap s = agent.lastState();
        if (agent.stateCount() > since && pred(s))
            return s;
        return std::nullopt;
    }

    static QCborMap peerIn(const QCborMap &state, const QString &peerHex)
    {
        for (const QCborValue &v : state.value(QStringLiteral("peers")).toArray()) {
            if (v.toMap().value(QStringLiteral("id")).toString() == peerHex)
                return v.toMap();
        }
        return {};
    }
    QCborMap peer(const QString &peerHex) const { return peerIn(agent.lastState(), peerHex); }
    // Connected to `peerHex` and has its hello (version): what the clipboard and drags need.
    bool waitConnected(const QString &peerHex, int timeoutMs)
    {
        return waitState([&](const QCborMap &s) {
                   const QCborMap p = peerIn(s, peerHex);
                   return p.value(QStringLiteral("connected")).toBool()
                          && !p.value(QStringLiteral("version")).toString().isEmpty();
               }, timeoutMs)
            .has_value();
    }
    // In the latest state; refreshed first when asked to.
    QString activeDevice(bool refresh = false)
    {
        if (refresh)
            waitState([](const QCborMap &) { return true; }, 2000);
        return agent.lastState().value(QStringLiteral("workspace")).toMap().value(QStringLiteral("active")).toString();
    }

    QString processStatus() const
    {
        if (!m_proc)
            return QStringLiteral("not started");
        if (m_proc->state() == QProcess::Running)
            return QStringLiteral("running");
        return QStringLiteral("exited with code %1 (0x%2)")
            .arg(m_proc->exitCode())
            .arg(quint32(m_proc->exitCode()), 0, 16);
    }
    QString logTail(int lines = 25) const
    {
        QFile f(logPath());
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        const QStringList all = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        return all.mid(qMax(0, int(all.size()) - lines)).join(QLatin1Char('\n'));
    }
    QString diagnostics() const
    {
        return QStringLiteral("--- %1 (%2), agent got: %3; helper got: %4\n%5")
            .arg(m_label, processStatus(), agent.recent(), helper.recent(), logTail());
    }

    QByteArray id;
    QString idHex;
    quint16 dataPort = 0;
    quint16 tcpPort = 0; // the file streams (announced to peers)
    QString version;

private:
    QString m_label;
    QString m_dir;
    QString m_instance;
    QByteArray m_token = QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    std::unique_ptr<QProcess> m_proc;

public:
    IpcClient agent;
    IpcClient helper;
};

// TCP through the router: connections accepted on `listenOn:port` are joined to 127.0.0.1:port,
// made from `from` (the address the other side sees). Bytes are copied both ways until either side
// closes.
class TcpForward : public QObject
{
public:
    bool listen(const QHostAddress &listenOn, quint16 port, const QHostAddress &from, QString *error)
    {
        m_from = from;
        m_port = port;
        if (!m_server.listen(listenOn, port)) {
            *error = QStringLiteral("NAT: cannot listen on %1:%2: %3").arg(listenOn.toString()).arg(port).arg(m_server.errorString());
            return false;
        }
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *in = m_server.nextPendingConnection())
                join(in);
        });
        return true;
    }
    quint16 port() const { return m_port; }

private:
    void join(QTcpSocket *in)
    {
        in->setParent(this);
        auto *out = new QTcpSocket(this);
        out->bind(m_from, 0);
        auto joined = std::make_shared<bool>(false);
        // What one side received goes to the other one once that one is connected.
        auto flush = [](QTcpSocket *src, QTcpSocket *dst) {
            if (dst->state() == QAbstractSocket::ConnectedState && src->bytesAvailable() > 0)
                dst->write(src->readAll());
        };
        auto reap = [in, out] {
            if (in->state() == QAbstractSocket::UnconnectedState && out->state() == QAbstractSocket::UnconnectedState) {
                in->deleteLater();
                out->deleteLater();
            }
        };
        for (const auto &[src, dst] : {std::pair{in, out}, std::pair{out, in}}) {
            connect(src, &QTcpSocket::readyRead, this, [=] { flush(src, dst); });
            // A side that closes: the other one gets the rest, then is closed after sending it.
            connect(src, &QTcpSocket::disconnected, this, [=] {
                flush(src, dst);
                if (dst->state() == QAbstractSocket::ConnectedState)
                    dst->disconnectFromHost();
                else
                    dst->abort();
                reap();
            });
        }
        connect(out, &QTcpSocket::connected, this, [=] {
            *joined = true;
            flush(in, out);
            if (in->state() != QAbstractSocket::ConnectedState)
                out->disconnectFromHost();
        });
        connect(out, &QTcpSocket::errorOccurred, this, [=] {
            if (!*joined) { // nobody listens there: refused, as the real destination would
                in->abort();
                out->abort();
                reap();
            }
        });
        out->connectToHost(QHostAddress::LocalHost, m_port);
    }

    QTcpServer m_server;
    QHostAddress m_from;
    quint16 m_port = 0;
};

// A home router in front of one device: datagrams to the WAN address are forwarded to the device
// (port forward), and the device sees every outside peer as coming from the router's LAN address
// with a per-peer mapped port (masquerade), which is what it learns, stores and shows. reset()
// drops every mapping, as a router restart does.
//
// TCP (the file streams, on the port each device announces): forwardTcp(port) is the port forward
// of the device's TCP port on the WAN address; masqueradeTcp(port) lets the device reach an outside
// peer's TCP port where it sees that peer, the LAN address. Connections arrive from the router.
class NatRouter : public QObject
{
public:
    // Whether the loopback addresses used here can be bound (macOS only has 127.0.0.1 by default:
    // `sudo ifconfig lo0 alias 127.0.0.2 up` and `... 127.0.0.3 up` enable the NAT tests).
    static bool available()
    {
        QUdpSocket a, b;
        return a.bind(QHostAddress(QStringLiteral("127.0.0.2")), 0) && b.bind(QHostAddress(QStringLiteral("127.0.0.3")), 0);
    }

    bool start(quint16 insidePort, QString *error)
    {
        m_inside = insidePort;
        m_wan.close();
        if (!m_wan.bind(m_wanAddress, 0)) {
            *error = QStringLiteral("NAT: cannot bind %1: %2").arg(m_wanAddress.toString(), m_wan.errorString());
            return false;
        }
        connect(&m_wan, &QUdpSocket::readyRead, this, &NatRouter::fromWan, Qt::UniqueConnection);
        return true;
    }
    void setInsidePort(quint16 port) { m_inside = port; }
    QHostAddress wanAddress() const { return m_wanAddress; }
    quint16 wanPort() const { return m_wan.localPort(); }
    QHostAddress lanAddress() const { return m_lanAddress; }
    void reset()
    {
        m_maps.clear();
    }
    int mappings() const { return int(m_maps.size()); }

    bool forwardTcp(quint16 port, QString *error) { return tcp(m_tcpIn, m_wanAddress, port, m_lanAddress, error); }
    bool masqueradeTcp(quint16 port, QString *error) { return tcp(m_tcpOut, m_lanAddress, port, m_wanAddress, error); }

private:
    struct Mapping {
        QHostAddress outside;
        quint16 outsidePort = 0;
        std::unique_ptr<QUdpSocket> lan;
    };
    static QString key(const QHostAddress &a, quint16 p) { return a.toString() + QLatin1Char(':') + QString::number(p); }

    bool tcp(QHash<quint16, std::shared_ptr<TcpForward>> &table, const QHostAddress &listenOn, quint16 port,
             const QHostAddress &from, QString *error)
    {
        if (!port) {
            *error = QStringLiteral("NAT: no TCP port to forward");
            return false;
        }
        if (table.contains(port))
            return true;
        auto f = std::make_shared<TcpForward>();
        if (!f->listen(listenOn, port, from, error))
            return false;
        table.insert(port, f);
        return true;
    }

    void fromWan()
    {
        while (m_wan.hasPendingDatagrams()) {
            const QNetworkDatagram d = m_wan.receiveDatagram();
            const QString k = key(d.senderAddress(), quint16(d.senderPort()));
            auto it = m_maps.find(k);
            if (it == m_maps.end()) {
                auto m = std::make_shared<Mapping>();
                m->outside = d.senderAddress();
                m->outsidePort = quint16(d.senderPort());
                m->lan = std::make_unique<QUdpSocket>();
                if (!m->lan->bind(m_lanAddress, 0))
                    continue;
                QUdpSocket *lan = m->lan.get();
                std::weak_ptr<Mapping> weak = m;
                connect(lan, &QUdpSocket::readyRead, this, [this, lan, weak] {
                    while (lan->hasPendingDatagrams()) {
                        const QNetworkDatagram back = lan->receiveDatagram();
                        const auto mapping = weak.lock();
                        // Only the inside device's own answers go out (a port-restricted NAT).
                        if (mapping && back.senderPort() == m_inside)
                            m_wan.writeDatagram(back.data(), mapping->outside, mapping->outsidePort);
                    }
                });
                it = m_maps.insert(k, m);
            }
            (*it)->lan->writeDatagram(d.data(), QHostAddress::LocalHost, m_inside);
        }
    }

    const QHostAddress m_wanAddress{QStringLiteral("127.0.0.2")};
    const QHostAddress m_lanAddress{QStringLiteral("127.0.0.3")};
    QUdpSocket m_wan;
    quint16 m_inside = 0;
    QHash<QString, std::shared_ptr<Mapping>> m_maps;
    QHash<quint16, std::shared_ptr<TcpForward>> m_tcpIn, m_tcpOut;
};

// --- Files ----------------------------------------------------------------------------------------

inline bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

inline QByteArray randomBytes(int size, quint32 seed)
{
    QRandomGenerator gen(seed);
    QByteArray b(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        b[i] = char(gen.bounded(256));
    return b;
}

// What a tree holds: relative path -> "dir" or "<size>:<sha256>". A file or folder itself is
// listed under its own name.
inline QMap<QString, QString> snapshot(const QString &path)
{
    QMap<QString, QString> out;
    const QFileInfo top(path);
    auto describe = [](const QFileInfo &fi) {
        if (fi.isDir())
            return QStringLiteral("dir");
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly))
            return QStringLiteral("unreadable");
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(&f);
        return QStringLiteral("%1:%2").arg(fi.size()).arg(QString::fromLatin1(h.result().toHex()));
    };
    out.insert(top.fileName(), describe(top));
    if (top.isDir()) {
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            out.insert(top.fileName() + QLatin1Char('/') + QDir(path).relativeFilePath(it.filePath()),
                       describe(it.fileInfo()));
        }
    }
    return out;
}

inline QMap<QString, QString> snapshot(const QStringList &paths)
{
    QMap<QString, QString> out;
    for (const QString &p : paths)
        out.insert(snapshot(QDir::fromNativeSeparators(p)));
    return out;
}

// Lines that differ between two snapshots, for failure messages.
inline QString snapshotDiff(const QMap<QString, QString> &want, const QMap<QString, QString> &got)
{
    QStringList lines;
    for (auto it = want.constBegin(); it != want.constEnd(); ++it) {
        if (!got.contains(it.key()))
            lines << QStringLiteral("missing: %1").arg(it.key());
        else if (got.value(it.key()) != it.value())
            lines << QStringLiteral("differs: %1 (%2 vs %3)").arg(it.key(), it.value(), got.value(it.key()));
    }
    for (auto it = got.constBegin(); it != got.constEnd(); ++it) {
        if (!want.contains(it.key()))
            lines << QStringLiteral("unexpected: %1").arg(it.key());
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace df::e2e
