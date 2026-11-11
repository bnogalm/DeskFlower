// Linux X11 integration tests, run under Xvfb (tests/docker): the input backend (grabs held by
// other clients, early capture, local buttons), the XShm screen grabber, the clipboard as other
// X clients see it (xclip), the helper's paste of copied files, and dragging files (XDND) both
// ways. Skipped without an X display, and unless DESKFLOWER_PRIVATE_SESSION=1 says the display is
// the test run's own.

#include "clipboardbridge.h"
#include "filedragagent.h"
#include "inputbackend.h"
#include "ipc.h"
#include "linuxbackends.h"
#include "screengrabber.h"
#include "sessionclient.h"
#include "stubinputbackend.h"

#include <QApplication>
#include <QCborArray>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

// X11 headers last: their macros (None, Bool, Status...) clash with Qt.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

namespace {

constexpr unsigned long kNone = 0L;

// Another X client: holds grabs, injects input, and plays the other side of a drag.
struct XPeer {
    Display *dpy = nullptr;
    Window root = 0;

    XPeer()
    {
        dpy = XOpenDisplay(nullptr);
        if (dpy)
            root = DefaultRootWindow(dpy);
    }
    ~XPeer()
    {
        if (dpy)
            XCloseDisplay(dpy);
    }
    Atom atom(const char *name) const { return XInternAtom(dpy, name, False); }

    Time serverTime(Window w)
    {
        XSelectInput(dpy, w, PropertyChangeMask);
        XChangeProperty(dpy, w, atom("PEER_TIME"), XA_STRING, 8, PropModeAppend, nullptr, 0);
        XEvent ev;
        XWindowEvent(dpy, w, PropertyChangeMask, &ev);
        return ev.xproperty.time;
    }

    void sendClient(Window to, Atom type, long l0, long l1, long l2, long l3, long l4)
    {
        XEvent ev = {};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = to;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = l0;
        ev.xclient.data.l[1] = l1;
        ev.xclient.data.l[2] = l2;
        ev.xclient.data.l[3] = l3;
        ev.xclient.data.l[4] = l4;
        XSendEvent(dpy, to, False, NoEventMask, &ev);
        XFlush(dpy);
    }

    // Lets the application's event loop run while handling this client's events, until `done`.
    bool pumpUntil(const std::function<void(XEvent &)> &handle, const std::function<bool()> &done, int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        while (!done()) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                handle(ev);
            }
            if (t.elapsed() > timeoutMs)
                return false;
            QTest::qWait(10);
        }
        return true;
    }

    long xdndVersion(Window w) const
    {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char *data = nullptr;
        long v = 0;
        if (XGetWindowProperty(dpy, w, atom("XdndAware"), 0, 1, False, AnyPropertyType, &type, &format, &count, &after,
                               &data)
                == Success
            && data && count == 1)
            v = *reinterpret_cast<long *>(data);
        if (data)
            XFree(data);
        return v;
    }

    QByteArray readProperty(Window w, Atom property)
    {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char *data = nullptr;
        QByteArray out;
        if (XGetWindowProperty(dpy, w, property, 0, 1 << 20, True, AnyPropertyType, &type, &format, &count, &after, &data)
                == Success
            && data)
            out = QByteArray(reinterpret_cast<const char *>(data), int(count));
        if (data)
            XFree(data);
        return out;
    }
};

// Runs an X client to its end while this process keeps serving the selection.
QByteArray run(const QString &program, const QStringList &args, int *exitCode = nullptr)
{
    QProcess p;
    p.start(program, args);
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && t.elapsed() < 15000)
        QTest::qWait(20);
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished();
    }
    if (exitCode)
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return p.readAllStandardOutput();
}

QByteArray xclip(const QString &target, int *exitCode = nullptr)
{
    return run(QStringLiteral("xclip"), {QStringLiteral("-o"), QStringLiteral("-selection"), QStringLiteral("clipboard"),
                                         QStringLiteral("-t"), target},
               exitCode);
}

// The service side of the helper's IPC, for SessionClient.
struct FakeService {
    QLocalServer server;
    QList<df::ipc::Connection *> connections;
    QList<QCborMap> received;
    df::ipc::Connection *helper = nullptr;
    std::function<void(df::ipc::Connection *, const QCborMap &)> onRequest;

    explicit FakeService(const QString &name)
    {
        QLocalServer::removeServer(name);
        server.listen(name);
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            while (QLocalSocket *s = server.nextPendingConnection()) {
                auto *c = new df::ipc::Connection(s, s);
                connections.append(c);
                QObject::connect(c, &df::ipc::Connection::messageReceived, c, [this, c](const QCborMap &m) {
                    received.append(m);
                    const QString cmd = m.value(QStringLiteral("cmd")).toString();
                    if (cmd == QLatin1String("hello"))
                        helper = c;
                    else if (c != helper && onRequest)
                        onRequest(c, m);
                });
            }
        });
    }
    QStringList notices() const
    {
        QStringList out;
        for (const QCborMap &m : received) {
            if (m.value(QStringLiteral("cmd")).toString() == QLatin1String("notice"))
                out.append(m.value(QStringLiteral("text")).toString());
        }
        return out;
    }
    int count(const QString &cmd) const
    {
        int n = 0;
        for (const QCborMap &m : received)
            n += m.value(QStringLiteral("cmd")).toString() == cmd;
        return n;
    }
};

QCborMap ipcEvent(const QString &ev)
{
    QCborMap m;
    m.insert(QStringLiteral("ev"), ev);
    return m;
}

} // namespace

class TestX11 : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // It grabs the pointer and the keyboard, injects input, writes the clipboard and drags
        // over windows: only on a display of its own (Xvfb in the Docker run and the CI, which say
        // so), never on somebody's desktop.
        if (qEnvironmentVariable("DESKFLOWER_PRIVATE_SESSION") != QLatin1String("1")
            && qEnvironmentVariable("DESKFLOWER_DESKTOP_TESTS") != QLatin1String("1"))
            QSKIP("Needs a private X display (DESKFLOWER_PRIVATE_SESSION=1 under Xvfb: tests/docker/run.sh)");
        if (qEnvironmentVariableIsEmpty("DISPLAY") || !m_peer.dpy)
            QSKIP("No X display (run under Xvfb: tests/docker/run.sh)");
    }

    // --- Input backend -------------------------------------------------------------------------

    void grabHeldElsewhereEndsTheCapture()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr));
        QVERIFY(input->start());
        QVERIFY(input->problem().isEmpty());
        // Another client (a menu, a drag) holds the pointer.
        QCOMPARE(XGrabPointer(m_peer.dpy, m_peer.root, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, kNone,
                              kNone, CurrentTime),
                 GrabSuccess);
        XFlush(m_peer.dpy);
        QSignalSpy lost(input.get(), &InputBackend::captureLost);
        input->setCapture(true, QPoint(10, 10));
        QTRY_COMPARE_WITH_TIMEOUT(lost.size(), 1, 3000);
        QVERIFY(!input->isCapturing());
        // No half capture: the keyboard it got meanwhile was given back.
        QCOMPARE(XGrabKeyboard(m_peer.dpy, m_peer.root, False, GrabModeAsync, GrabModeAsync, CurrentTime), GrabSuccess);
        XUngrabKeyboard(m_peer.dpy, CurrentTime);
        XUngrabPointer(m_peer.dpy, CurrentTime);
        XFlush(m_peer.dpy);
    }

    void keyboardGrabIsRetried()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr));
        QVERIFY(input->start());
        QCOMPARE(XGrabKeyboard(m_peer.dpy, m_peer.root, False, GrabModeAsync, GrabModeAsync, CurrentTime), GrabSuccess);
        XFlush(m_peer.dpy);
        QSignalSpy lost(input.get(), &InputBackend::captureLost);
        input->setCapture(true, QPoint(10, 10));
        QVERIFY(input->isCapturing()); // pending: the watchdog may still end it
        QTest::qWait(150);
        XUngrabKeyboard(m_peer.dpy, CurrentTime); // the other client lets go
        XFlush(m_peer.dpy);
        QTest::qWait(300);
        QCOMPARE(lost.size(), 0);
        QVERIFY(input->isCapturing());
        // Both grabs are ours now.
        QCOMPARE(XGrabKeyboard(m_peer.dpy, m_peer.root, False, GrabModeAsync, GrabModeAsync, CurrentTime),
                 AlreadyGrabbed);
        input->setCapture(false, QPoint(20, 20));
        QVERIFY(!input->isCapturing());
        QTRY_COMPARE(XGrabKeyboard(m_peer.dpy, m_peer.root, False, GrabModeAsync, GrabModeAsync, CurrentTime),
                     GrabSuccess);
        XUngrabKeyboard(m_peer.dpy, CurrentTime);
        XFlush(m_peer.dpy);
    }

    void injectedInputIsNotLocalActivity()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr));
        QVERIFY(input->start());
        QSignalSpy activity(input.get(), &InputBackend::localPasteGesture);
        XTestFakeButtonEvent(m_peer.dpy, 3, True, CurrentTime);
        XTestFakeButtonEvent(m_peer.dpy, 3, False, CurrentTime);
        XFlush(m_peer.dpy);
        QTest::qWait(300);
        QCOMPARE(activity.size(), 0);
        QVERIFY(!input->leftButtonHeld());
    }

    void localLeftButtonAndItsRelease()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr, X11BackendOptions{true}));
        QVERIFY(input->start());
        QSignalSpy activity(input.get(), &InputBackend::localPasteGesture);
        QSignalSpy released(input.get(), &InputBackend::localRelease);
        XTestFakeMotionEvent(m_peer.dpy, -1, 500, 300, CurrentTime);
        XTestFakeButtonEvent(m_peer.dpy, 1, True, CurrentTime);
        XFlush(m_peer.dpy);
        QTRY_VERIFY(input->leftButtonHeld());
        QVERIFY(activity.size() >= 1);
        input->setReportReleases(true); // a file drag is watched (dragWatch)
        XTestFakeButtonEvent(m_peer.dpy, 1, False, CurrentTime);
        XFlush(m_peer.dpy);
        QTRY_COMPARE(released.size(), 1);
        QCOMPARE(released.at(0).at(0).toPoint(), QPoint(500, 300));
        QVERIFY(!input->leftButtonHeld());
    }

    // Paste evidence: a local Ctrl+V is a paste gesture, a plain letter (or Ctrl alone) is not.
    // XTest stands in for the local keyboard here (xtestIsLocal). X11 key codes are evdev + 8:
    // Left Control 37, V 55, A 38.
    void localPasteChordIsAGesture()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr, X11BackendOptions{true}));
        QVERIFY(input->start());
        QVERIFY(input->reportsLocalPresses());
        QSignalSpy gestures(input.get(), &InputBackend::localPasteGesture);
        const auto tap = [this](unsigned int keycode) {
            XTestFakeKeyEvent(m_peer.dpy, keycode, True, CurrentTime);
            XTestFakeKeyEvent(m_peer.dpy, keycode, False, CurrentTime);
        };
        tap(38); // A
        XTestFakeKeyEvent(m_peer.dpy, 37, True, CurrentTime); // Control held
        XFlush(m_peer.dpy);
        QTest::qWait(300);
        QCOMPARE(gestures.size(), 0);
        tap(55); // V, with Control held
        XTestFakeKeyEvent(m_peer.dpy, 37, False, CurrentTime);
        XFlush(m_peer.dpy);
        QTRY_COMPARE(gestures.size(), 1);
        tap(55); // V alone
        XFlush(m_peer.dpy);
        QTest::qWait(300);
        QCOMPARE(gestures.size(), 1);
    }

    void earlyCaptureAtAKnownEdge()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr, X11BackendOptions{true}));
        QVERIFY(input->start());
        const int w = DisplayWidth(m_peer.dpy, DefaultScreen(m_peer.dpy));
        const int h = DisplayHeight(m_peer.dpy, DefaultScreen(m_peer.dpy));
        input->setReportEdges(true);
        input->setEdgeMap({InputBackend::EdgeSpan{QRect(0, 0, w, h), 1, 0, h / 2}});
        QSignalSpy edges(input.get(), &InputBackend::edgeHit);
        // Outside the span: reported, not captured.
        XTestFakeMotionEvent(m_peer.dpy, -1, w - 1, h - 10, CurrentTime);
        XFlush(m_peer.dpy);
        QTRY_VERIFY(edges.size() >= 1);
        QCOMPARE(edges.last().at(2).toBool(), false);
        QVERIFY(!input->isCapturing());
        // Within it: captured at once.
        XTestFakeMotionEvent(m_peer.dpy, -1, w - 1, 10, CurrentTime);
        XFlush(m_peer.dpy);
        QTRY_VERIFY(edges.size() >= 2 && edges.last().at(2).toBool());
        QCOMPARE(edges.last().at(0).toInt(), 1);
        QTRY_VERIFY(input->isCapturing());
        input->setCapture(false, QPoint(w / 2, h / 2), 1);
        QVERIFY(!input->isCapturing());
        // Back through that edge: no early capture for a moment.
        XTestFakeMotionEvent(m_peer.dpy, -1, w - 1, 20, CurrentTime);
        XFlush(m_peer.dpy);
        QTest::qWait(100);
        QVERIFY(!input->isCapturing());
    }

    void cursorHidingIsBalanced()
    {
        std::unique_ptr<InputBackend> input(createX11InputBackend(nullptr));
        QVERIFY(input->start());
        input->setCursorHidden(true);
        input->setCursorHidden(true); // one hide outstanding, whatever the calls
        input->setCursorHidden(false);
        input->setCursorHidden(false);
        input->setCursorHidden(true);
        input.reset(); // shown again on the way out
        XSync(m_peer.dpy, False);
    }

    // --- Screen capture ------------------------------------------------------------------------

    void xshmGrabberCapturesChanges()
    {
        std::unique_ptr<ScreenGrabber> grabber = createX11ScreenGrabber();
        const int w = DisplayWidth(m_peer.dpy, DefaultScreen(m_peer.dpy));
        const int h = DisplayHeight(m_peer.dpy, DefaultScreen(m_peer.dpy));
        QImage image;
        QCOMPARE(grabber->grab(&image, 50), ScreenGrabber::Result::Changed);
        QCOMPARE(image.size(), QSize(w, h));
        QCOMPARE(grabber->grab(&image, 50), ScreenGrabber::Result::Unchanged);
        // Something appears on the screen.
        const Window window = XCreateSimpleWindow(m_peer.dpy, m_peer.root, 10, 10, 100, 100, 0, 0,
                                                  WhitePixel(m_peer.dpy, DefaultScreen(m_peer.dpy)));
        XSetWindowAttributes attrs = {};
        attrs.override_redirect = True;
        XChangeWindowAttributes(m_peer.dpy, window, CWOverrideRedirect, &attrs);
        XMapRaised(m_peer.dpy, window);
        XSync(m_peer.dpy, False);
        QImage changed;
        QCOMPARE(grabber->grab(&changed, 500), ScreenGrabber::Result::Changed);
        QCOMPARE(QColor(changed.pixel(50, 50)), QColor(Qt::white));
        // Another screen rectangle: a complete image of it.
        grabber->setScreen(QRect(0, 0, 200, 100));
        QCOMPARE(grabber->grab(&image, 50), ScreenGrabber::Result::Changed);
        QCOMPARE(image.size(), QSize(200, 100));
        XDestroyWindow(m_peer.dpy, window);
        XSync(m_peer.dpy, False);
    }

    // --- Clipboard as other X clients see it ---------------------------------------------------

    void sealedFormatsAreDecryptedOnlyWhenPasted()
    {
        ClipboardBridge bridge;
        int unseals = 0;
        bridge.applySealed({ClipboardBridge::SealedFormat{QStringLiteral("text/plain"), 11}}, {}, 0,
                           [&](const QString &mime, QString *) {
                               ++unseals;
                               return mime == QLatin1String("text/plain") ? QByteArray("secret text") : QByteArray();
                           },
                           nullptr);
        QTest::qWait(100);
        // A clipboard manager listing the formats learns nothing.
        const QByteArray targets = xclip(QStringLiteral("TARGETS"));
        QVERIFY2(targets.contains("text/plain"), targets.constData());
        QVERIFY2(!targets.contains("x-kde-passwordManagerHint"), targets.constData()); // text only: unmarked
        QCOMPARE(unseals, 0);
        // The paste decrypts that format.
        QCOMPARE(xclip(QStringLiteral("text/plain")), QByteArray("secret text"));
        QCOMPARE(unseals, 1);
    }

    void copiedFilesSpeakToFileManagers()
    {
        QTemporaryDir dir;
        const QString a = dir.filePath(QStringLiteral("a.txt")), b = dir.filePath(QStringLiteral("sub folder"));
        QVERIFY(QFile(a).open(QIODevice::WriteOnly));
        QDir().mkpath(b);
        const QByteArray expectedGnome =
            "copy\n" + QUrl::fromLocalFile(a).toEncoded() + "\n" + QUrl::fromLocalFile(b).toEncoded();

        // Files received (already local): offered with the GNOME and KDE formats too.
        ClipboardBridge bridge;
        bridge.apply({}, {a, b});
        QTest::qWait(100);
        QCOMPARE(xclip(QStringLiteral("x-special/gnome-copied-files")), expectedGnome);
        QCOMPARE(xclip(QStringLiteral("application/x-kde-cutselection")), QByteArray("0"));

        // Sealed files: transferred when a file manager reads them, once.
        int fetches = 0;
        bridge.applySealed({}, {}, 2, nullptr, [&](QString *) {
            ++fetches;
            return QStringList{a, b};
        });
        QTest::qWait(100);
        const QByteArray targets = xclip(QStringLiteral("TARGETS"));
        QVERIFY(targets.contains("x-special/gnome-copied-files") && targets.contains("text/uri-list"));
        // Clipboard managers are asked not to record it (their reads must not start a transfer).
        QVERIFY2(targets.contains("x-kde-passwordManagerHint"), targets.constData());
        QCOMPARE(xclip(QStringLiteral("x-kde-passwordManagerHint")), QByteArray("secret"));
        QCOMPARE(fetches, 0);
        QCOMPARE(xclip(QStringLiteral("x-special/gnome-copied-files")), expectedGnome);
        QCOMPARE(xclip(QStringLiteral("text/uri-list")),
                 QUrl::fromLocalFile(a).toEncoded() + "\r\n" + QUrl::fromLocalFile(b).toEncoded() + "\r\n");
        QCOMPARE(fetches, 1);
    }

    // A read refused as no paste (kNotAPaste) is not remembered: the paste that follows gets the
    // files.
    void notAPasteIsNotRemembered()
    {
        QTemporaryDir dir;
        const QString a = dir.filePath(QStringLiteral("a.txt"));
        QVERIFY(QFile(a).open(QIODevice::WriteOnly));
        ClipboardBridge bridge;
        int fetches = 0;
        bridge.applySealed({}, {}, 1, nullptr, [&](QString *error) {
            if (++fetches == 1) {
                *error = QString::fromLatin1(ClipboardBridge::kNotAPaste);
                return QStringList();
            }
            return QStringList{a};
        });
        QTest::qWait(100);
        QVERIFY(xclip(QStringLiteral("text/uri-list")).isEmpty());
        QCOMPARE(xclip(QStringLiteral("text/uri-list")), QUrl::fromLocalFile(a).toEncoded() + "\r\n");
        QCOMPARE(fetches, 2);
    }

    // The helper does not hold a paste for a long transfer: the application gets nothing at once,
    // the user is told to paste again, and the paste after the transfer gets the files.
    void pasteOfALongTransferAsksToPasteAgain()
    {
        const QString instance = QStringLiteral("tst-x11-%1").arg(QCoreApplication::applicationPid());
        FakeService service(df::ipc::serverName(instance));
        QTemporaryDir dir;
        const QString received = dir.filePath(QStringLiteral("received.bin"));
        QVERIFY(QFile(received).open(QIODevice::WriteOnly));
        df::ipc::Connection *fetcher = nullptr;
        service.onRequest = [&](df::ipc::Connection *c, const QCborMap &m) {
            if (m.value(QStringLiteral("cmd")).toString() == QLatin1String("clipFetch"))
                fetcher = c; // answered later: the transfer takes a while
        };
        StubInputBackend input(QStringLiteral("test"));
        auto client = std::make_unique<SessionClient>(instance, QByteArray("token"), &input);
        QTRY_VERIFY(service.helper);
        QCborMap config = ipcEvent(QStringLiteral("clipConfig"));
        config.insert(QStringLiteral("on"), true);
        service.helper->send(config);
        QCborMap set = ipcEvent(QStringLiteral("clipSet"));
        set.insert(QStringLiteral("kid"), QByteArray(16, 'k'));
        set.insert(QStringLiteral("sealed"), QCborArray());
        set.insert(QStringLiteral("files"), QCborArray());
        set.insert(QStringLiteral("pendingFiles"), 1);
        service.helper->send(set);
        QTest::qWait(200);

        QElapsedTimer pasted;
        pasted.start();
        int exitCode = 0;
        const QByteArray first = xclip(QStringLiteral("text/uri-list"), &exitCode);
        QVERIFY(first.isEmpty());
        QVERIFY2(pasted.elapsed() < 4500, "the paste must not wait for the whole transfer");
        QVERIFY(fetcher);
        QTRY_COMPARE(service.notices().size(), 1);
        QVERIFY2(service.notices().constFirst().contains(QLatin1String("paste again")),
                 qPrintable(service.notices().constFirst()));

        // The transfer ends: the user is told, and the next paste gets the files at once.
        QCborMap done = ipcEvent(QStringLiteral("clipFetched"));
        done.insert(QStringLiteral("ok"), true);
        done.insert(QStringLiteral("files"), QCborArray{received});
        fetcher->send(done);
        QTRY_COMPARE(service.notices().size(), 2);
        QCOMPARE(xclip(QStringLiteral("text/uri-list")), QUrl::fromLocalFile(received).toEncoded() + "\r\n");
        QCOMPARE(service.count(QStringLiteral("clipFetch")), 1);
        client.reset();
    }

    void quickTransfersCompleteWithinThePaste()
    {
        const QString instance = QStringLiteral("tst-x11q-%1").arg(QCoreApplication::applicationPid());
        FakeService service(df::ipc::serverName(instance));
        QTemporaryDir dir;
        const QString received = dir.filePath(QStringLiteral("small.txt"));
        QVERIFY(QFile(received).open(QIODevice::WriteOnly));
        service.onRequest = [&](df::ipc::Connection *c, const QCborMap &m) {
            if (m.value(QStringLiteral("cmd")).toString() != QLatin1String("clipFetch"))
                return;
            QTimer::singleShot(300, c, [c, received] {
                QCborMap done = ipcEvent(QStringLiteral("clipFetched"));
                done.insert(QStringLiteral("ok"), true);
                done.insert(QStringLiteral("files"), QCborArray{received});
                c->send(done);
            });
        };
        StubInputBackend input(QStringLiteral("test"));
        auto client = std::make_unique<SessionClient>(instance, QByteArray("token"), &input);
        QTRY_VERIFY(service.helper);
        QCborMap set = ipcEvent(QStringLiteral("clipSet"));
        set.insert(QStringLiteral("kid"), QByteArray(16, 'k'));
        set.insert(QStringLiteral("sealed"), QCborArray());
        set.insert(QStringLiteral("files"), QCborArray());
        set.insert(QStringLiteral("pendingFiles"), 1);
        service.helper->send(set);
        QTest::qWait(200);
        QCOMPARE(xclip(QStringLiteral("text/uri-list")), QUrl::fromLocalFile(received).toEncoded() + "\r\n");
        QCOMPARE(service.notices().size(), 0);
        client.reset();
    }

    // --- Dragging files (XDND) -----------------------------------------------------------------

    // A drag in progress elsewhere on the screen enters the catcher, which reads its files and
    // cancels it with Escape.
    void catchReadsTheFilesOfADrag()
    {
        FileDragAgent agent;
        const Window source = XCreateWindow(m_peer.dpy, m_peer.root, -10, -10, 1, 1, 0, 0, InputOnly, CopyFromParent, 0,
                                            nullptr);
        const Time owned = m_peer.serverTime(source);
        XSetSelectionOwner(m_peer.dpy, m_peer.atom("XdndSelection"), source, owned);
        // Escape is watched for on the raw key events.
        int xiOpcode = 0, event = 0, error = 0;
        QVERIFY(XQueryExtension(m_peer.dpy, "XInputExtension", &xiOpcode, &event, &error));
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
        XISetMask(bits, XI_RawKeyPress);
        XIEventMask mask{XIAllMasterDevices, int(sizeof(bits)), bits};
        XISelectEvents(m_peer.dpy, m_peer.root, &mask, 1);
        XFlush(m_peer.dpy);

        const QPoint at(600, 400);
        QStringList caught;
        bool answered = false;
        agent.catchFiles(at, [&](const QStringList &paths) {
            caught = paths;
            answered = true;
        });
        Window catcher = 0;
        bool accepted = false, escape = false;
        const KeyCode escapeCode = XKeysymToKeycode(m_peer.dpy, XK_Escape);
        auto handle = [&](XEvent &ev) {
            if (ev.type == ClientMessage && ev.xclient.message_type == m_peer.atom("XdndStatus"))
                accepted = ev.xclient.data.l[1] & 1;
            if (ev.type == SelectionRequest) {
                const XSelectionRequestEvent &req = ev.xselectionrequest;
                const QByteArray list = "file:///tmp/a.txt\r\nfile:///tmp/some%20folder\r\n";
                XChangeProperty(m_peer.dpy, req.requestor, req.property, req.target, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(list.constData()), int(list.size()));
                XEvent reply = {};
                reply.xselection.type = SelectionNotify;
                reply.xselection.requestor = req.requestor;
                reply.xselection.selection = req.selection;
                reply.xselection.target = req.target;
                reply.xselection.property = req.property;
                reply.xselection.time = req.time;
                XSendEvent(m_peer.dpy, req.requestor, False, NoEventMask, &reply);
                XFlush(m_peer.dpy);
            }
            if (ev.type == GenericEvent && ev.xcookie.extension == xiOpcode && XGetEventData(m_peer.dpy, &ev.xcookie)) {
                if (ev.xcookie.evtype == XI_RawKeyPress
                    && static_cast<XIRawEvent *>(ev.xcookie.data)->detail == int(escapeCode))
                    escape = true;
                XFreeEventData(m_peer.dpy, &ev.xcookie);
            }
        };
        // The drag source finds the XDND-aware window under the cursor and enters it.
        QVERIFY(m_peer.pumpUntil(handle, [&] {
            Window child = 0;
            int x = 0, y = 0;
            XTranslateCoordinates(m_peer.dpy, m_peer.root, m_peer.root, at.x(), at.y(), &x, &y, &child);
            if (child && m_peer.xdndVersion(child) >= 3)
                catcher = child;
            return catcher != 0;
        }, 2000));
        m_peer.sendClient(catcher, m_peer.atom("XdndEnter"), long(source), 5L << 24, long(m_peer.atom("text/uri-list")),
                          0, 0);
        m_peer.sendClient(catcher, m_peer.atom("XdndPosition"), long(source), 0, long((at.x() << 16) | at.y()),
                          long(owned), long(m_peer.atom("XdndActionCopy")));
        QVERIFY(m_peer.pumpUntil(handle, [&] { return answered && escape; }, 3000));
        QVERIFY(accepted);
        QCOMPARE(caught, (QStringList{QStringLiteral("/tmp/a.txt"), QStringLiteral("/tmp/some folder")}));
        XDestroyWindow(m_peer.dpy, source);
        XSync(m_peer.dpy, False);
    }

    void catchWithoutADragFindsNothing()
    {
        FileDragAgent agent;
        bool answered = false;
        QStringList caught{QStringLiteral("x")};
        agent.catchFiles(QPoint(300, 300), [&](const QStringList &paths) {
            caught = paths;
            answered = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(answered, FileDragAgent::kCatchTimeoutMs + 1000);
        QVERIFY(caught.isEmpty());
    }

    void hoverDropsTheOriginalsHere()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("own file.txt"));
        QVERIFY(QFile(file).open(QIODevice::WriteOnly));
        FileDragAgent::HoverSpec spec;
        spec.own = true;
        spec.paths = {file};
        spec.count = 1;
        const DropResult r = dropOn(spec, nullptr);
        QVERIFY(r.done);
        QCOMPARE(r.dropped, true);
        QCOMPARE(r.confirmed, false); // the originals: moved by the target, nothing to confirm
        QCOMPARE(r.data, QUrl::fromLocalFile(file).toEncoded() + "\r\n");
        QCOMPARE(r.action, m_peer.atom("XdndActionMove"));
    }

    void hoverTransfersRemoteFilesBeforeTheDrop()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("from elsewhere.bin"));
        QVERIFY(QFile(file).open(QIODevice::WriteOnly));
        FileDragAgent::HoverSpec spec;
        spec.files = {ClipboardBridge::ListedFile{QStringLiteral("from elsewhere.bin"), 0, false}};
        spec.count = 1;
        std::atomic<int> fetches{0};
        const DropResult r = dropOn(spec, [&](QString *) {
            ++fetches;
            QThread::msleep(300); // the transfer
            return QStringList{file};
        });
        QVERIFY(r.done);
        QCOMPARE(fetches.load(), 1);
        QVERIFY2(r.dropAfterMs >= 250, "the drop waits for the transfer");
        QCOMPARE(r.dropped, true);
        QCOMPARE(r.confirmed, true); // the target reported the move: the originals may go
        QCOMPARE(r.data, QUrl::fromLocalFile(file).toEncoded() + "\r\n");
        QCOMPARE(r.action, m_peer.atom("XdndActionMove"));
    }

    // A target that copied the transferred files (XdndFinished with XdndActionCopy) took them, but
    // did not confirm a move: the originals on the other device stay.
    void hoverCopiedByTheTargetIsNotAMove()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("copied.bin"));
        QVERIFY(QFile(file).open(QIODevice::WriteOnly));
        FileDragAgent::HoverSpec spec;
        spec.files = {ClipboardBridge::ListedFile{QStringLiteral("copied.bin"), 0, false}};
        spec.count = 1;
        const DropResult r = dropOn(spec, [&](QString *) { return QStringList{file}; }, true, "XdndActionCopy");
        QVERIFY(r.done);
        QCOMPARE(r.dropped, true);
        QCOMPARE(r.confirmed, false);
    }

    // The service stops saying it is there (dragAlive) once it did: the drag here ends by itself.
    void hoverEndsWhenTheServiceFallsSilent()
    {
        FileDragAgent agent;
        FileDragAgent::HoverSpec spec;
        spec.own = true;
        spec.paths = {QStringLiteral("/tmp/x")};
        spec.count = 1;
        spec.heartbeat = true;
        bool done = false;
        FileDragAgent::HoverResult result;
        result.dropped = true;
        agent.hover(spec, nullptr, [&](const FileDragAgent::HoverResult &r) {
            done = true;
            result = r;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, FileDragAgent::kAliveTimeoutMs + 3000);
        QVERIFY(!result.dropped);
    }

    void hoverOverAWindowThatRefusesDropsNothing()
    {
        FileDragAgent::HoverSpec spec;
        spec.own = true;
        spec.paths = {QStringLiteral("/tmp/x")};
        spec.count = 1;
        const DropResult r = dropOn(spec, nullptr, false);
        QVERIFY(r.done);
        QCOMPARE(r.dropped, false);
        QVERIFY(r.data.isEmpty());
    }

private:
    struct DropResult {
        bool done = false;
        bool dropped = false;
        bool confirmed = false;
        QByteArray data;
        Atom action = 0;
        qint64 dropAfterMs = -1;
    };

    // A drop target window covering the screen takes (or refuses) the drag shown by the agent.
    // performed: the action it reports doing (XdndFinished); by default the one it was offered.
    DropResult dropOn(const FileDragAgent::HoverSpec &spec, ClipboardBridge::FetchFiles fetch, bool accept = true,
                      const char *performed = nullptr)
    {
        DropResult r;
        const int w = DisplayWidth(m_peer.dpy, DefaultScreen(m_peer.dpy));
        const int h = DisplayHeight(m_peer.dpy, DefaultScreen(m_peer.dpy));
        XSetWindowAttributes attrs = {};
        attrs.override_redirect = True;
        attrs.event_mask = PropertyChangeMask;
        const Window target = XCreateWindow(m_peer.dpy, m_peer.root, 0, 0, unsigned(w), unsigned(h), 0, CopyFromParent,
                                            InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask, &attrs);
        const long version = 5;
        XChangeProperty(m_peer.dpy, target, m_peer.atom("XdndAware"), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&version), 1);
        XMapRaised(m_peer.dpy, target);
        XTestFakeMotionEvent(m_peer.dpy, -1, 300, 300, CurrentTime);
        XSync(m_peer.dpy, False);

        FileDragAgent agent;
        agent.hover(spec, std::move(fetch), [&](const FileDragAgent::HoverResult &result) {
            r.done = true;
            r.dropped = result.dropped;
            r.confirmed = result.confirmed;
        });
        Window source = 0;
        bool positioned = false, gotData = false;
        QElapsedTimer sinceEnd;
        auto handle = [&](XEvent &ev) {
            if (ev.type == ClientMessage && ev.xclient.window == target) {
                const Atom type = ev.xclient.message_type;
                if (type == m_peer.atom("XdndEnter")) {
                    source = Window(ev.xclient.data.l[0]);
                } else if (type == m_peer.atom("XdndPosition")) {
                    positioned = true;
                    r.action = Atom(ev.xclient.data.l[4]);
                    m_peer.sendClient(source, m_peer.atom("XdndStatus"), long(target), accept ? 1 : 0, 0, 0,
                                      accept ? long(r.action) : 0);
                } else if (type == m_peer.atom("XdndDrop")) {
                    r.dropAfterMs = sinceEnd.elapsed();
                    XConvertSelection(m_peer.dpy, m_peer.atom("XdndSelection"), m_peer.atom("text/uri-list"),
                                      m_peer.atom("DROPPED"), target, Time(ev.xclient.data.l[2]));
                    XFlush(m_peer.dpy);
                }
            } else if (ev.type == SelectionNotify && ev.xselection.requestor == target) {
                if (ev.xselection.property)
                    r.data = m_peer.readProperty(target, m_peer.atom("DROPPED"));
                gotData = true;
                m_peer.sendClient(source, m_peer.atom("XdndFinished"), long(target), r.data.isEmpty() ? 0 : 1,
                                  long(performed ? m_peer.atom(performed) : r.action), 0, 0);
            }
        };
        if (!m_peer.pumpUntil(handle, [&] { return positioned; }, 3000))
            qWarning("No XdndPosition came");
        sinceEnd.start();
        agent.endHover(true);
        m_peer.pumpUntil(handle, [&] { return r.done; }, 5000);
        Q_UNUSED(gotData)
        XDestroyWindow(m_peer.dpy, target);
        XSync(m_peer.dpy, False);
        return r;
    }

    XPeer m_peer;
};

int main(int argc, char *argv[])
{
    // The clipboard and drag tests need the real X11 platform, not the offscreen one of the
    // other tests.
    if (!qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    XInitThreads();
    QApplication app(argc, argv);
    TestX11 test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_x11.moc"
