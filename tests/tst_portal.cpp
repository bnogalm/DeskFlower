// The Wayland helper's xdg-desktop-portal session (PortalSession) and clipboard (PortalClipboard)
// against a fake portal on the D-Bus session bus (tests/docker runs the suite in a private one).
// The fake runs on a thread and bus connection of its own, as the real portal is another process:
// the helper's blocking calls must be answered meanwhile. Skipped without a session bus, and unless
// DESKFLOWER_PRIVATE_SESSION=1 says the bus is private to the test run.

#include "clipboardbridge.h"
#include "portalclipboard.h"
#include "portalsession.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QMutex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");

struct IntPair {
    int a = 0;
    int b = 0;
};
QDBusArgument &operator<<(QDBusArgument &arg, const IntPair &p)
{
    arg.beginStructure();
    arg << p.a << p.b;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, IntPair &p)
{
    arg.beginStructure();
    arg >> p.a >> p.b;
    arg.endStructure();
    return arg;
}

struct PwStream {
    uint node = 0;
    QVariantMap props;
};
QDBusArgument &operator<<(QDBusArgument &arg, const PwStream &s)
{
    arg.beginStructure();
    arg << s.node << s.props;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PwStream &s)
{
    arg.beginStructure();
    arg >> s.node >> s.props;
    arg.endStructure();
    return arg;
}

} // namespace

Q_DECLARE_METATYPE(IntPair)
Q_DECLARE_METATYPE(PwStream)

namespace {

// What the fake saw and how it answers; shared between the test and the portal thread.
struct PortalState {
    QMutex mutex;
    QStringList calls;                       // method names, in order
    QHash<QString, QVariantMap> options;     // the last options of each method
    QHash<QString, uint> responses;          // Response code per method (default 0)
    QString restoreToken = QStringLiteral("token-1");
    uint devices = 3;                        // what Start grants (keyboard | pointer)
    QString session;
    QStringList announced;                   // SetSelection mime_types
    QHash<uint, QByteArray> written;         // SelectionWrite serial → bytes the helper wrote
    QHash<uint, bool> writeDone;             // SelectionWriteDone serial → success
    QHash<QString, QByteArray> offered;      // what SelectionRead serves

    void record(const QString &method, const QVariantMap &opts = {})
    {
        QMutexLocker lock(&mutex);
        calls.append(method);
        options.insert(method, opts);
    }
};

PortalState g_state;


class Host : public QObject
{
    Q_OBJECT
public:
    QDBusConnection bus{QString()};

    // Emitted later (the method's reply goes first), on the request path the caller predicted.
    QDBusObjectPath respond(const QString &method, const QVariantMap &options, const QDBusMessage &msg,
                            QVariantMap results)
    {
        g_state.record(method, options);
        QString sender = msg.service().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                                 .arg(sender, options.value(QStringLiteral("handle_token")).toString());
        uint code = 0;
        {
            QMutexLocker lock(&g_state.mutex);
            code = g_state.responses.value(method, 0);
        }
        QTimer::singleShot(0, this, [this, path, code, results] {
            QDBusMessage signal = QDBusMessage::createSignal(path, QStringLiteral("org.freedesktop.portal.Request"),
                                                             QStringLiteral("Response"));
            signal << code << results;
            bus.send(signal);
        });
        return QDBusObjectPath(path);
    }

public slots:
    void emitOwnerChanged(const QStringList &mimeTypes, bool own)
    {
        QDBusMessage signal = QDBusMessage::createSignal(kPath, QStringLiteral("org.freedesktop.portal.Clipboard"),
                                                         QStringLiteral("SelectionOwnerChanged"));
        QVariantMap options;
        options.insert(QStringLiteral("mime_types"), mimeTypes);
        options.insert(QStringLiteral("session_is_owner"), own);
        signal << QVariant::fromValue(QDBusObjectPath(sessionPath())) << options;
        bus.send(signal);
    }
    void emitTransfer(const QString &mime, uint serial)
    {
        QDBusMessage signal = QDBusMessage::createSignal(kPath, QStringLiteral("org.freedesktop.portal.Clipboard"),
                                                         QStringLiteral("SelectionTransfer"));
        signal << QVariant::fromValue(QDBusObjectPath(sessionPath())) << mime << serial;
        bus.send(signal);
    }
    void emitClosed()
    {
        QDBusMessage signal = QDBusMessage::createSignal(sessionPath(), QStringLiteral("org.freedesktop.portal.Session"),
                                                         QStringLiteral("Closed"));
        signal << QVariantMap();
        bus.send(signal);
    }

private:
    static QString sessionPath()
    {
        QMutexLocker lock(&g_state.mutex);
        return g_state.session;
    }
};

class RemoteDesktopAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.RemoteDesktop")
public:
    explicit RemoteDesktopAdaptor(Host *host) : QDBusAbstractAdaptor(host), m_host(host) {}

public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options, const QDBusMessage &msg)
    {
        QString sender = msg.service().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        const QString session = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2")
                                    .arg(sender, options.value(QStringLiteral("session_handle_token")).toString());
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.session = session;
        }
        QVariantMap results;
        results.insert(QStringLiteral("session_handle"), session);
        return m_host->respond(QStringLiteral("CreateSession"), options, msg, results);
    }
    QDBusObjectPath SelectDevices(const QDBusObjectPath &, const QVariantMap &options, const QDBusMessage &msg)
    {
        return m_host->respond(QStringLiteral("SelectDevices"), options, msg, {});
    }
    QDBusObjectPath Start(const QDBusObjectPath &, const QString &, const QVariantMap &options, const QDBusMessage &msg)
    {
        QVariantMap results;
        results.insert(QStringLiteral("clipboard_enabled"), true);
        {
            QMutexLocker lock(&g_state.mutex);
            results.insert(QStringLiteral("devices"), g_state.devices);
            results.insert(QStringLiteral("restore_token"), g_state.restoreToken);
        }
        PwStream stream;
        stream.node = 42;
        stream.props.insert(QStringLiteral("position"), QVariant::fromValue(IntPair{0, 0}));
        stream.props.insert(QStringLiteral("size"), QVariant::fromValue(IntPair{1280, 800}));
        results.insert(QStringLiteral("streams"), QVariant::fromValue(QList<PwStream>{stream}));
        return m_host->respond(QStringLiteral("Start"), options, msg, results);
    }
    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath &, const QVariantMap &options)
    {
        g_state.record(QStringLiteral("ConnectToEIS"), options);
        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        const QDBusUnixFileDescriptor out(fds[0]);
        ::close(fds[0]);
        ::close(fds[1]);
        return out;
    }

private:
    Host *m_host;
};

class ScreenCastAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
public:
    explicit ScreenCastAdaptor(Host *host) : QDBusAbstractAdaptor(host), m_host(host) {}

public slots:
    QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &options, const QDBusMessage &msg)
    {
        return m_host->respond(QStringLiteral("SelectSources"), options, msg, {});
    }

private:
    Host *m_host;
};

class ClipboardAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Clipboard")
public:
    explicit ClipboardAdaptor(Host *host) : QDBusAbstractAdaptor(host) {}

public slots:
    void RequestClipboard(const QDBusObjectPath &, const QVariantMap &options)
    {
        g_state.record(QStringLiteral("RequestClipboard"), options);
    }
    void SetSelection(const QDBusObjectPath &, const QVariantMap &options)
    {
        g_state.record(QStringLiteral("SetSelection"), options);
        QMutexLocker lock(&g_state.mutex);
        g_state.announced = options.value(QStringLiteral("mime_types")).toStringList();
    }
    QDBusUnixFileDescriptor SelectionWrite(const QDBusObjectPath &, uint serial)
    {
        g_state.record(QStringLiteral("SelectionWrite"));
        int fds[2];
        if (pipe(fds) != 0)
            return {};
        const QDBusUnixFileDescriptor out(fds[1]);
        ::close(fds[1]);
        std::thread([fd = fds[0], serial] {
            QByteArray data;
            char buffer[4096];
            ssize_t n = 0;
            while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
                data.append(buffer, int(n));
            ::close(fd);
            QMutexLocker lock(&g_state.mutex);
            g_state.written.insert(serial, data);
        }).detach();
        return out;
    }
    void SelectionWriteDone(const QDBusObjectPath &, uint serial, bool success)
    {
        g_state.record(QStringLiteral("SelectionWriteDone"));
        QMutexLocker lock(&g_state.mutex);
        g_state.writeDone.insert(serial, success);
    }
    QDBusUnixFileDescriptor SelectionRead(const QDBusObjectPath &, const QString &mime)
    {
        g_state.record(QStringLiteral("SelectionRead:") + mime);
        QByteArray data;
        {
            QMutexLocker lock(&g_state.mutex);
            data = g_state.offered.value(mime);
        }
        int fds[2];
        if (pipe(fds) != 0)
            return {};
        if (!data.isEmpty() && ::write(fds[1], data.constData(), size_t(data.size())) < 0)
            qWarning("fake portal: write failed");
        ::close(fds[1]);
        const QDBusUnixFileDescriptor out(fds[0]);
        ::close(fds[0]);
        return out;
    }
};

// The fake portal's thread: its own bus connection, which owns org.freedesktop.portal.Desktop.
class PortalThread : public QThread
{
public:
    Host *host = nullptr;
    bool ok = false;
    QMutex ready;

protected:
    void run() override
    {
        qDBusRegisterMetaType<IntPair>();
        qDBusRegisterMetaType<PwStream>();
        qDBusRegisterMetaType<QList<PwStream>>();
        Host h;
        h.bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-portal"));
        new RemoteDesktopAdaptor(&h);
        new ScreenCastAdaptor(&h);
        new ClipboardAdaptor(&h);
        ok = h.bus.isConnected() && h.bus.registerObject(kPath, &h) && h.bus.registerService(kService);
        host = &h;
        exec();
        host = nullptr;
        h.bus.unregisterService(kService);
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-portal"));
    }
};

void resetState()
{
    QMutexLocker lock(&g_state.mutex);
    g_state.calls.clear();
    g_state.options.clear();
    g_state.responses.clear();
    g_state.restoreToken = QStringLiteral("token-1");
    g_state.devices = 3;
    g_state.announced.clear();
    g_state.written.clear();
    g_state.writeDone.clear();
    g_state.offered.clear();
}

QStringList calls()
{
    QMutexLocker lock(&g_state.mutex);
    return g_state.calls;
}

QVariantMap optionsOf(const QString &method)
{
    QMutexLocker lock(&g_state.mutex);
    return g_state.options.value(method);
}

} // namespace

class TestPortal : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The fake claims the portal's name on the session bus: only on a private bus (the Docker
        // run and the CI say so), never in somebody's desktop session.
        if (qEnvironmentVariable("DESKFLOWER_PRIVATE_SESSION") != QLatin1String("1")
            && qEnvironmentVariable("DESKFLOWER_DESKTOP_TESTS") != QLatin1String("1"))
            QSKIP("Needs a private session bus (DESKFLOWER_PRIVATE_SESSION=1: tests/docker/run.sh)");
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("No D-Bus session bus (run under dbus-run-session: tests/docker/run.sh)");
        if (QDBusConnection::sessionBus().interface()->isServiceRegistered(kService))
            QSKIP("A real desktop portal runs on this bus");
        m_thread.start();
        QTRY_VERIFY(m_thread.host);
        QVERIFY(m_thread.ok);
    }

    void cleanupTestCase()
    {
        m_thread.quit();
        m_thread.wait();
    }

    void init() { resetState(); }

    void sessionAsksOnceThenRestores()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        {
            PortalSession portal(options);
            QSignalSpy started(&portal, &PortalSession::started);
            portal.start();
            QTRY_COMPARE(started.size(), 1);
            QCOMPARE(portal.state(), PortalSession::State::Started);
            QVERIFY(portal.problem().isEmpty());
            // Keyboard and pointer, remembered until revoked; nothing to restore the first time.
            const QVariantMap devices = optionsOf(QStringLiteral("SelectDevices"));
            QCOMPARE(devices.value(QStringLiteral("types")).toUInt(), 3u);
            QCOMPARE(devices.value(QStringLiteral("persist_mode")).toUInt(), 2u);
            QVERIFY(!devices.contains(QStringLiteral("restore_token")));
            // Every monitor, on the same session.
            const QVariantMap sources = optionsOf(QStringLiteral("SelectSources"));
            QCOMPARE(sources.value(QStringLiteral("types")).toUInt(), 1u);
            QVERIFY(sources.value(QStringLiteral("multiple")).toBool());
            // The clipboard is requested before Start, as the portal requires.
            const QStringList c = calls();
            QVERIFY(c.indexOf(QStringLiteral("RequestClipboard")) >= 0);
            QVERIFY(c.indexOf(QStringLiteral("RequestClipboard")) < c.indexOf(QStringLiteral("Start")));
            QVERIFY(portal.clipboardEnabled());
            QCOMPARE(portal.streams().size(), 1);
            QCOMPARE(portal.streams().at(0).node, 42u);
            QCOMPARE(portal.streams().at(0).geometry, QRect(0, 0, 1280, 800));
            QVERIFY(portal.inputGranted());
            const int fd = portal.connectToEis();
            QVERIFY(fd >= 0);
            ::close(fd);
        }
        // The token is kept, private to the user...
        QFile token(options.tokenFile);
        QVERIFY(token.open(QIODevice::ReadOnly));
        QCOMPARE(token.readAll(), QByteArray("token-1"));
        QCOMPARE(token.permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup), QFileDevice::Permissions());
        // ...and the next session restores the permission with it (the portal hands out a new one).
        resetState();
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.restoreToken = QStringLiteral("token-2");
        }
        PortalSession again(options);
        QSignalSpy started(&again, &PortalSession::started);
        again.start();
        QTRY_COMPARE(started.size(), 1);
        QCOMPARE(optionsOf(QStringLiteral("SelectDevices")).value(QStringLiteral("restore_token")).toString(),
                 QStringLiteral("token-1"));
        token.close();
        QVERIFY(token.open(QIODevice::ReadOnly));
        QCOMPARE(token.readAll(), QByteArray("token-2"));
    }

    void refusalIsReported()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.responses.insert(QStringLiteral("Start"), 1); // the user said no
        }
        PortalSession portal(options);
        QSignalSpy failed(&portal, &PortalSession::failed);
        portal.start();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(portal.state(), PortalSession::State::Failed);
        QVERIFY(!portal.problem().isEmpty());
        QVERIFY(portal.problem().contains(QLatin1String("refused")));
        // The helper stays (asking again would only bring the dialog back).
        QVERIFY(portal.refused());
        QVERIFY(!portal.worthRestarting());
        QCOMPARE(portal.connectToEis(), -1);
        QVERIFY(!QFile::exists(options.tokenFile));
    }

    void closedSessionIsReported()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        PortalSession portal(options);
        QSignalSpy started(&portal, &PortalSession::started);
        QSignalSpy closed(&portal, &PortalSession::closed);
        portal.start();
        QTRY_COMPARE(started.size(), 1);
        QMetaObject::invokeMethod(m_thread.host, "emitClosed", Qt::QueuedConnection);
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(portal.state(), PortalSession::State::Closed);
        QVERIFY(!portal.problem().isEmpty());
        // The helper starts over (the restore token brings the session back without a dialog).
        QVERIFY(portal.worthRestarting());
        QVERIFY(!portal.refused());
    }

    // A portal that fails (not the user) is worth another try too.
    void portalErrorIsWorthRestarting()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.responses.insert(QStringLiteral("SelectDevices"), 2); // ended in another way
        }
        PortalSession portal(options);
        QSignalSpy failed(&portal, &PortalSession::failed);
        portal.start();
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!portal.refused());
        QVERIFY(portal.worthRestarting());
    }

    // The user unticked the keyboard and mouse: the clipboard and the screens still work.
    void sessionWithoutInputKeepsTheRest()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.devices = 0;
        }
        PortalSession portal(options);
        QSignalSpy started(&portal, &PortalSession::started);
        QSignalSpy failed(&portal, &PortalSession::failed);
        portal.start();
        QTRY_COMPARE(started.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(portal.state(), PortalSession::State::Started);
        QVERIFY(!portal.inputGranted());
        QVERIFY(portal.clipboardEnabled());
        QCOMPARE(portal.streams().size(), 1);
        QCOMPARE(portal.connectToEis(), -1);
        QVERIFY(!calls().contains(QStringLiteral("ConnectToEIS")));
    }

    // What another application copies reaches ClipboardBridge, read through the portal.
    void clipboardOfOtherApplicationsIsRead()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        PortalSession portal(options);
        portal.start();
        QTRY_COMPARE(portal.state(), PortalSession::State::Started);
        ClipboardBridge bridge;
        auto *clipboard = new PortalClipboard(&portal);
        bridge.setBackend(clipboard);
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.offered.insert(QStringLiteral("text/plain;charset=utf-8"), "copied on Wayland");
            g_state.offered.insert(QStringLiteral("text/html"), "<b>copied</b>");
        }
        QMetaObject::invokeMethod(m_thread.host, "emitOwnerChanged", Qt::QueuedConnection,
                                  Q_ARG(QStringList, (QStringList{QStringLiteral("text/plain;charset=utf-8"),
                                                                  QStringLiteral("text/html"),
                                                                  QStringLiteral("application/x-other")})),
                                  Q_ARG(bool, false));
        QTRY_COMPARE(changed.size(), 1);
        const QCborMap formats = changed.at(0).at(0).value<QCborMap>();
        QCOMPARE(formats.value(QStringLiteral("text/plain")).toByteArray(), QByteArray("copied on Wayland"));
        QCOMPARE(formats.value(QStringLiteral("text/html")).toByteArray(), QByteArray("<b>copied</b>"));
        // Only the formats shared were read.
        QVERIFY(!calls().contains(QStringLiteral("SelectionRead:application/x-other")));
        QVERIFY(!clipboard->ownsClipboard());
    }

    // Sealed content is announced by format and decrypted only when an application pastes it.
    void sealedContentIsRenderedOnTransfer()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        PortalSession portal(options);
        ClipboardBridge bridge;
        bridge.setBackend(new PortalClipboard(&portal));
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        int unseals = 0;
        // Set before the session runs: announced once it does.
        bridge.applySealed({ClipboardBridge::SealedFormat{QStringLiteral("text/plain"), 6}}, {}, 0,
                           [&](const QString &, QString *) {
                               ++unseals;
                               return QByteArray("secret");
                           },
                           nullptr);
        portal.start();
        QTRY_VERIFY(calls().contains(QStringLiteral("SetSelection")));
        QStringList announced;
        {
            QMutexLocker lock(&g_state.mutex);
            announced = g_state.announced;
        }
        QVERIFY(announced.contains(QStringLiteral("text/plain;charset=utf-8")));
        QCOMPARE(unseals, 0);
        // An application pastes.
        QMetaObject::invokeMethod(m_thread.host, "emitTransfer", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("text/plain;charset=utf-8")), Q_ARG(uint, 7u));
        QTRY_VERIFY([] {
            QMutexLocker lock(&g_state.mutex);
            return g_state.writeDone.contains(7) && g_state.written.contains(7);
        }());
        {
            QMutexLocker lock(&g_state.mutex);
            QCOMPARE(g_state.written.value(7), QByteArray("secret"));
            QVERIFY(g_state.writeDone.value(7));
        }
        QCOMPARE(unseals, 1);
        // Our own selection coming back is not new content.
        QMetaObject::invokeMethod(m_thread.host, "emitOwnerChanged", Qt::QueuedConnection,
                                  Q_ARG(QStringList, announced), Q_ARG(bool, true));
        QTest::qWait(300);
        QCOMPARE(changed.size(), 0);
    }

    // Copied files are announced with KDE's password manager hint, so clipboard managers leave
    // them alone; announcing transfers nothing.
    void copiedFilesCarryThePasswordManagerHint()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        PortalSession portal(options);
        ClipboardBridge bridge;
        bridge.setBackend(new PortalClipboard(&portal));
        bridge.setEnabled(true);
        int fetches = 0;
        bridge.applySealed({}, {}, 1, nullptr, [&](QString *) {
            ++fetches;
            return QStringList();
        });
        portal.start();
        QTRY_VERIFY(calls().contains(QStringLiteral("SetSelection")));
        QStringList announced;
        {
            QMutexLocker lock(&g_state.mutex);
            announced = g_state.announced;
        }
        QVERIFY(announced.contains(QStringLiteral("text/uri-list")));
        QVERIFY(announced.contains(QStringLiteral("x-kde-passwordManagerHint")));
        QCOMPARE(fetches, 0);
    }

    // Another application copies while a sealed format is being decrypted for a paste (the render
    // waits for the service in a nested event loop, where D-Bus signals keep coming): the content
    // being rendered stays alive until the paste is served, then the new content is taken.
    void copyElsewhereDuringASealedPaste()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        PortalSession portal(options);
        portal.start();
        QTRY_COMPARE(portal.state(), PortalSession::State::Started);
        ClipboardBridge bridge;
        auto *clipboard = new PortalClipboard(&portal);
        bridge.setBackend(clipboard);
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        {
            QMutexLocker lock(&g_state.mutex);
            g_state.offered.insert(QStringLiteral("text/plain;charset=utf-8"), "copied meanwhile");
        }
        bool replacedDuringRender = false;
        bridge.applySealed({ClipboardBridge::SealedFormat{QStringLiteral("text/plain"), 6}}, {}, 0,
                           [&](const QString &, QString *) {
                               QMetaObject::invokeMethod(
                                   m_thread.host, "emitOwnerChanged", Qt::QueuedConnection,
                                   Q_ARG(QStringList, QStringList{QStringLiteral("text/plain;charset=utf-8")}),
                                   Q_ARG(bool, false));
                               replacedDuringRender = QTest::qWaitFor([&] { return !clipboard->ownsClipboard(); }, 5000);
                               return QByteArray("secret");
                           },
                           nullptr);
        QTRY_VERIFY(calls().contains(QStringLiteral("SetSelection")));
        QMetaObject::invokeMethod(m_thread.host, "emitTransfer", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("text/plain;charset=utf-8")), Q_ARG(uint, 11u));
        QTRY_VERIFY([] {
            QMutexLocker lock(&g_state.mutex);
            return g_state.writeDone.contains(11) && g_state.written.contains(11);
        }());
        QVERIFY(replacedDuringRender);
        {
            QMutexLocker lock(&g_state.mutex);
            QCOMPARE(g_state.written.value(11), QByteArray("secret"));
        }
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(changed.at(0).at(0).value<QCborMap>().value(QStringLiteral("text/plain")).toByteArray(),
                 QByteArray("copied meanwhile"));
    }

    void plainContentIsServedToo()
    {
        QTemporaryDir dir;
        PortalSession::Options options;
        options.tokenFile = dir.filePath(QStringLiteral("token"));
        PortalSession portal(options);
        portal.start();
        QTRY_COMPARE(portal.state(), PortalSession::State::Started);
        ClipboardBridge bridge;
        bridge.setBackend(new PortalClipboard(&portal));
        QCborMap formats;
        formats.insert(QStringLiteral("text/plain"), QByteArray("from the phone"));
        bridge.apply(formats, {});
        QTRY_VERIFY(calls().contains(QStringLiteral("SetSelection")));
        QMetaObject::invokeMethod(m_thread.host, "emitTransfer", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("text/plain;charset=utf-8")), Q_ARG(uint, 9u));
        QTRY_VERIFY([] {
            QMutexLocker lock(&g_state.mutex);
            return g_state.writeDone.contains(9) && g_state.written.contains(9);
        }());
        QMutexLocker lock(&g_state.mutex);
        QCOMPARE(g_state.written.value(9), QByteArray("from the phone"));
    }

private:
    PortalThread m_thread;
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv); // QMimeData images and the clipboard bridge
    TestPortal test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_portal.moc"
