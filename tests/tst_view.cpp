#include "framecodec.h"
#include "inputcontroller.h"
#include "remoteview.h"
#include "securestream.h"
#include "store.h"
#include "testutil.h"
#include "workspace.h"

#include <QCborArray>
#include <QCborValue>
#include <QElapsedTimer>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace df;
using namespace df::view;

namespace {
QImage desktop(QSize size, QColor background)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(background);
    QPainter p(&img);
    p.fillRect(QRect(100, 100, 300, 200), Qt::white);
    // Shapes, not text: fonts need a GUI application and these tests run without one.
    p.setPen(QPen(Qt::black, 3));
    p.drawLine(110, 110, 390, 290);
    p.drawEllipse(QRect(180, 140, 120, 120));
    return img;
}

// Mean absolute difference per channel: JPEG is lossy.
double difference(const QImage &a, const QImage &b)
{
    double total = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            total += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    }
    return total / (3.0 * a.width() * a.height());
}

// A frame as the helper hands it to the source, of screen `index` and capture `generation`.
QCborMap toFrame(quint32 seq, QSize size, bool full, const QList<Tile> &tiles, int index = 0, quint32 generation = 0)
{
    QCborArray list;
    for (const Tile &t : tiles) {
        QCborMap m;
        m.insert(QStringLiteral("x"), t.rect.x());
        m.insert(QStringLiteral("y"), t.rect.y());
        m.insert(QStringLiteral("w"), t.rect.width());
        m.insert(QStringLiteral("h"), t.rect.height());
        m.insert(QStringLiteral("d"), t.jpeg);
        list.append(m);
    }
    QCborMap f;
    f.insert(QStringLiteral("i"), index);
    f.insert(QStringLiteral("g"), qint64(generation));
    f.insert(QStringLiteral("s"), qint64(seq));
    f.insert(QStringLiteral("w"), size.width());
    f.insert(QStringLiteral("h"), size.height());
    f.insert(QStringLiteral("full"), full);
    f.insert(QStringLiteral("tiles"), list);
    return f;
}

QList<Tile> fromFrame(const QCborMap &f)
{
    QList<Tile> tiles;
    for (const QCborValue &v : f.value(QStringLiteral("tiles")).toArray()) {
        const QCborMap m = v.toMap();
        tiles.append(Tile{QRect(int(m.value("x").toInteger()), int(m.value("y").toInteger()),
                                int(m.value("w").toInteger()), int(m.value("h").toInteger())),
                          m.value("d").toByteArray()});
    }
    return tiles;
}

// The displayed screens carried by a controller signal.
QList<ViewScreen> shownIn(const QList<QVariant> &args)
{
    return args.at(1).value<QList<ViewScreen>>();
}

QCborMap lastEvent(const QSignalSpy &spy, const QString &name)
{
    for (qsizetype i = spy.size() - 1; i >= 0; --i) {
        const QCborMap m = spy.at(i).at(0).value<QCborMap>();
        if (m.value(QStringLiteral("ev")).toString() == name)
            return m;
    }
    return {};
}

int countEvents(const QSignalSpy &spy, const QString &name)
{
    int n = 0;
    for (const QList<QVariant> &args : spy) {
        if (args.at(0).value<QCborMap>().value(QStringLiteral("ev")).toString() == name)
            ++n;
    }
    return n;
}

QList<RemoteView::ViewState> states(const QSignalSpy &spy)
{
    QList<RemoteView::ViewState> out;
    for (const QList<QVariant> &args : spy)
        out.append(args.at(1).value<RemoteView::ViewState>());
    return out;
}

QString lastReason(const QSignalSpy &spy) { return spy.isEmpty() ? QString() : spy.last().at(0).toString(); }

// B's one screen, and how A asks for it: shown on a 1920 x 1080 local monitor.
const QRect kScreenB(0, 0, 1280, 720);

QList<ViewScreen> one(const QRect &remote = kScreenB, int index = 0)
{
    return {ViewScreen{index, remote, QRect(0, 0, 1920, 1080)}};
}

QList<CaptureSpec> specs(const QSignalSpy &spy, qsizetype n) { return spy.at(n).at(0).value<QList<CaptureSpec>>(); }

// The generation the source gave screen `index` in its latest capture request (0: not captured).
quint32 generationOf(const QSignalSpy &requested, int index = 0)
{
    for (const CaptureSpec &s : specs(requested, requested.size() - 1)) {
        if (s.index == index)
            return s.generation;
    }
    return 0;
}

// A frame of the helper for the source's current capture of screen `index`.
QCborMap helperFrame(const QSignalSpy &requested, quint32 seq, QSize size, bool full, const QList<Tile> &tiles, int index = 0)
{
    return toFrame(seq, size, full, tiles, index, generationOf(requested, index));
}

// Makes `to` believe `from` runs `version` (a hello is a control message any node may send).
void claimVersion(Node &from, Node &to, const QString &version)
{
    QCborMap hello;
    hello.insert(QStringLiteral("t"), QStringLiteral("hello"));
    hello.insert(QStringLiteral("n"), from.name());
    hello.insert(QStringLiteral("tp"), from.tcpPort());
    hello.insert(QStringLiteral("v"), version);
    hello.insert(QStringLiteral("tl"), QStringLiteral("device"));
    from.sendMessage(to.id(), hello);
}

// Two paired devices: A views, B shows its screen. B has a local user to ask; A is the master
// unless `master` is false.
struct ViewPair {
    QTemporaryDir dirA, dirB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Workspace> wa, wb;
    std::unique_ptr<RemoteView> viewer, source;

    bool setup(bool master = true)
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        wa = std::make_unique<Workspace>(a.get(), dirA.path());
        wb = std::make_unique<Workspace>(b.get(), dirB.path());
        wb->setLocalScreens({ScreenInfo{"b1", "", kScreenB, 1, true}});
        if (!test::pairNodes(*a, *b))
            return false;
        viewer = std::make_unique<RemoteView>(a.get(), wa.get());
        source = std::make_unique<RemoteView>(b.get(), wb.get());
        source->setLocalUserPresent(true);
        if (!master)
            return true;
        wa->setSharing(true);
        return QTest::qWaitFor([this] { return wb->masterId() == a->id(); }, 5000);
    }
};

QCborMap cmd(const QString &name, std::initializer_list<std::pair<QString, QCborValue>> args = {})
{
    QCborMap m;
    m.insert(QStringLiteral("cmd"), name);
    for (const auto &[k, v] : args)
        m.insert(k, v);
    return m;
}

// A control message of the wire protocol.
QCborMap ctrl(const QString &t, std::initializer_list<std::pair<QString, QCborValue>> args = {})
{
    QCborMap m;
    m.insert(QStringLiteral("t"), t);
    for (const auto &[k, v] : args)
        m.insert(k, v);
    return m;
}

QCborMap rectMap(const QRect &r)
{
    QCborMap m;
    m.insert(QStringLiteral("x"), r.x());
    m.insert(QStringLiteral("y"), r.y());
    m.insert(QStringLiteral("w"), r.width());
    m.insert(QStringLiteral("h"), r.height());
    return m;
}

// One entry of a `view {v: 2, screens}` request.
QCborMap viewEntry(int index, const QRect &screen, QSize maxSize = QSize(1920, 1080))
{
    QCborMap m = rectMap(screen);
    m.insert(QStringLiteral("i"), index);
    m.insert(QStringLiteral("mw"), maxSize.width());
    m.insert(QStringLiteral("mh"), maxSize.height());
    return m;
}

QCborMap frameOf(const QSignalSpy &received, qsizetype n) { return received.at(n).at(0).value<QCborMap>(); }

// A viewer speaking the wire protocol by hand: an older version, or one sending bad requests.
struct RawViewer {
    std::unique_ptr<SecureStream> stream;
    QList<QCborMap> control; // control messages from the source, in order
    int tiles = 0;

    bool open(Node &from, const QByteArray &to)
    {
        stream.reset(from.openStream(to));
        if (!stream)
            return false;
        QObject::connect(stream.get(), &SecureStream::received, stream.get(), [this](const QByteArray &f) {
            if (f.isEmpty())
                return;
            if (quint8(f[0]) == 1)
                control.append(QCborValue::fromCbor(f.mid(1)).toMap());
            else if (quint8(f[0]) == 3)
                ++tiles;
        });
        return QTest::qWaitFor([this] { return stream->isEstablished(); }, 5000);
    }
    void send(const QCborMap &msg) { stream->send(char(1) + QCborValue(msg).toCbor()); }
    QCborMap last(const QString &t) const
    {
        for (qsizetype i = control.size() - 1; i >= 0; --i) {
            if (control.at(i).value(QStringLiteral("t")).toString() == t)
                return control.at(i);
        }
        return {};
    }
    int count(const QString &t) const
    {
        int n = 0;
        for (const QCborMap &m : control)
            n += m.value(QStringLiteral("t")).toString() == t;
        return n;
    }
};

// A source speaking the wire protocol by hand on the stream a viewer opens to `node` (which must
// have no RemoteView of its own): an older version, or one sending frames it should not.
struct RawSource {
    QPointer<SecureStream> stream; // owned by the node
    QList<QCborMap> control;       // control messages from the viewer, in order

    void listen(Node &node)
    {
        QObject::connect(&node, &Node::streamAccepted, &node, [this](SecureStream *s) {
            stream = s;
            QObject::connect(s, &SecureStream::received, s, [this](const QByteArray &f) {
                if (!f.isEmpty() && quint8(f[0]) == 1)
                    control.append(QCborValue::fromCbor(f.mid(1)).toMap());
            });
        });
    }
    void send(const QCborMap &msg)
    {
        if (stream)
            stream->send(char(1) + QCborValue(msg).toCbor());
    }
    // A tile frame carrying `bytes` of (not decodable) image data.
    void sendTile(int bytes)
    {
        QByteArray t(9, '\0');
        t[0] = char(3);
        t += QByteArray(bytes, 'x');
        if (stream)
            stream->send(t);
    }
    QCborMap last(const QString &t) const
    {
        for (qsizetype i = control.size() - 1; i >= 0; --i) {
            if (control.at(i).value(QStringLiteral("t")).toString() == t)
                return control.at(i);
        }
        return {};
    }
    int count(const QString &t) const
    {
        int n = 0;
        for (const QCborMap &m : control)
            n += m.value(QStringLiteral("t")).toString() == t;
        return n;
    }
};

// Monitors side by side from the left, 1920 x 1080 each, the first one primary.
QList<ScreenInfo> row(const QString &prefix, int count)
{
    QList<ScreenInfo> out;
    for (int i = 0; i < count; ++i)
        out.append(ScreenInfo{prefix + QString::number(i + 1), "", QRect(1920 * i, 0, 1920, 1080), 1, i == 0});
    return out;
}

const QRect kA1(0, 0, 1920, 1080), kA2(1920, 0, 1920, 1080);
const QRect kB1(0, 0, 1920, 1080), kB2(1920, 0, 1920, 1080);

// A views B through the input controller: A's monitors in a row, B's pack right of them, A
// sharing. The plan, if any, is set once B is paired (a plan is kept for trusted devices only).
struct Viewing {
    QTemporaryDir dirA, dirB;
    std::unique_ptr<Node> a, b;
    std::unique_ptr<Workspace> wa, wb;
    std::unique_ptr<InputController> ia;
    std::unique_ptr<QSignalSpy> toA, requested, changed;
    int width = 0; // of A's row

    bool setup(const QList<ScreenInfo> &screensA, const QList<ScreenInfo> &screensB, const Workspace::ViewPlan &plan = {})
    {
        a = test::makeNode(dirA.path());
        b = test::makeNode(dirB.path());
        wa = std::make_unique<Workspace>(a.get(), dirA.path());
        wb = std::make_unique<Workspace>(b.get(), dirB.path());
        wa->setLocalScreens(screensA);
        wb->setLocalScreens(screensB);
        if (!test::pairNodes(*a, *b)
            || !QTest::qWaitFor([&] { return wa->screens(b->id()).size() == screensB.size(); }, 5000))
            return false;
        for (const ScreenInfo &s : screensA)
            width = std::max(width, s.geometry.right() + 1);
        wa->setLayout({{a->id(), {0, 0}}, {b->id(), {width, 0}}});
        wa->setRemoteView(b->id(), true);
        wa->setViewPlan(b->id(), plan);
        wa->setSharing(true);
        if (!QTest::qWaitFor([&] { return wb->masterId() == a->id(); }, 5000))
            return false;
        ia = std::make_unique<InputController>(a.get(), wa.get());
        toA = std::make_unique<QSignalSpy>(ia.get(), &InputController::toSession);
        requested = std::make_unique<QSignalSpy>(ia.get(), &InputController::viewRequested);
        changed = std::make_unique<QSignalSpy>(ia.get(), &InputController::viewScreensChanged);
        ia->sessionConnected();
        return true;
    }
    // The cursor pushes past the right edge of A's last monitor, `y` down.
    void cross(int y = 540) { ia->handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", width - 1}, {"y", y}})); }
    void move(int dx, int dy) { ia->handleSessionMessage(cmd("mm", {{"x", dx}, {"y", dy}})); }
    QString versionOfB() const
    {
        for (const PeerView &p : a->peers()) {
            if (p.id == b->id())
                return p.version;
        }
        return {};
    }
    void claimVersionOfB(const QString &version) { claimVersion(*b, *a, version); }
};
} // namespace

class TestView : public QObject
{
    Q_OBJECT
private slots:
    void codecSendsOnlyWhatChanged()
    {
        FrameEncoder encoder;
        FrameCanvas canvas;
        const QSize size(1000, 700); // not a multiple of the tile size
        const QImage first = desktop(size, Qt::darkBlue);
        const QList<Tile> all = encoder.encode(first, false);
        QCOMPARE(all.size(), 8 * 6); // ceil(1000/128) x ceil(700/128)
        QVERIFY(canvas.apply(size, all));
        QVERIFY(difference(canvas.image(), first) < 3.0);

        // Nothing changed: nothing sent.
        QCOMPARE(encoder.encode(first, false).size(), 0);

        // A small change touches one tile.
        QImage second = first;
        QPainter(&second).fillRect(QRect(900, 650, 20, 20), Qt::red);
        const QList<Tile> delta = encoder.encode(second, false);
        QCOMPARE(delta.size(), 1);
        QCOMPARE(delta.first().rect, QRect(896, 640, 104, 60));
        QVERIFY(canvas.apply(size, delta));
        QVERIFY(difference(canvas.image(), second) < 3.0);

        // A forced key frame or a new size sends everything.
        QCOMPARE(encoder.encode(second, true).size(), 48);
        QCOMPARE(encoder.encode(desktop(QSize(256, 128), Qt::gray), false).size(), 2);
    }

    void streamsOnlyToTheMasterAndIsPaced()
    {
        ViewPair p;
        QVERIFY(p.setup(false));
        RemoteView &viewer = *p.viewer, &source = *p.source;
        QSignalSpy requested(&source, &RemoteView::captureRequested);
        QSignalSpy failed(&viewer, &RemoteView::viewingFailed);

        // A is not the master yet: B refuses to show its screen, and says why.
        viewer.startViewing(p.b->id(), one());
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("not"), qPrintable(lastReason(failed)));
        QCOMPARE(requested.size(), 0);

        p.wa->setSharing(true);
        QTRY_COMPARE(p.wb->masterId(), p.a->id());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy state(&viewer, &RemoteView::viewStateChanged);
        viewer.startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(specs(requested, 0).first().maxSize, QSize(1920, 1080));
        QCOMPARE(specs(requested, 0).first().screen, kScreenB);
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::Starting));

        QSignalSpy received(&viewer, &RemoteView::frameReceived);
        QSignalSpy next(&source, &RemoteView::captureNext);
        FrameEncoder encoder;
        const QImage screen = desktop(QSize(1280, 720), Qt::darkGreen);
        source.frameCaptured(helperFrame(requested, 1, screen.size(), true, encoder.encode(screen, true)));
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(states(state).last(), RemoteView::ViewState::Streaming);
        QVERIFY(states(state).indexOf(RemoteView::ViewState::Starting)
                < states(state).indexOf(RemoteView::ViewState::Streaming));
        const QCborMap frame = received.first().at(0).value<QCborMap>();
        QCOMPARE(frame.value("s").toInteger(), 1);
        FrameCanvas canvas;
        QVERIFY(canvas.apply(QSize(1280, 720), fromFrame(frame)));
        QVERIFY(difference(canvas.image(), screen) < 3.0);

        // The next capture waits for the viewer's acknowledgement, and one frame is one capture.
        QTest::qWait(200);
        QCOMPARE(next.size(), 0);
        viewer.frameShown(1, 0);
        QTRY_COMPARE(next.size(), 1);
        QCOMPARE(next.first().at(0).toInt(), 0);
        viewer.frameShown(1, 0);
        QTest::qWait(200);
        QCOMPARE(next.size(), 1);

        QSignalSpy stopped(&source, &RemoteView::captureStopped);
        viewer.stopViewing();
        QTRY_COMPARE(stopped.size(), 1);
    }

    void switchesScreensOnTheSameStream()
    {
        ViewPair p;
        QVERIFY(p.setup());
        const QRect primary(0, 0, 2560, 1440), second(2560, 0, 1920, 1080);
        p.wb->setLocalScreens({ScreenInfo{"b1", "", primary, 1, true}, ScreenInfo{"b2", "", second, 1, false}});
        p.b->setViewAllowed(p.a->id(), true);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id())); // B's hello (version) has arrived
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy state(p.viewer.get(), &RemoteView::viewStateChanged);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);

        // The view starts on the screen the viewer asked for.
        p.viewer->startViewing(p.b->id(), one(second));
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(specs(requested, 0).first().screen, second);
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::Starting));

        // Another screen of the source: same stream, no new request or prompt, the set replaced.
        p.viewer->setScreens(one(primary));
        QTRY_COMPARE(requested.size(), 2);
        QCOMPARE(specs(requested, 1).size(), 1);
        QCOMPARE(specs(requested, 1).first().screen, primary);
        QCOMPARE(states(state).count(RemoteView::ViewState::Starting), 1);

        // Something that is not one of its screens is ignored.
        p.viewer->setScreens(one(QRect(-5000, 0, 800, 600)));
        QTest::qWait(200);
        QCOMPARE(requested.size(), 2);
        QCOMPARE(failed.size(), 0);

        // In a new request it is refused as a bad request.
        p.viewer->startViewing(p.b->id(), one(QRect(1, 2, 3, 4)));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("Bad request"), qPrintable(lastReason(failed)));
        QCOMPARE(requested.size(), 2);
    }

    void twoScreensStreamAtOncePacedPerScreen()
    {
        ViewPair p;
        QVERIFY(p.setup());
        const QRect first(0, 0, 1280, 720), second(1280, 0, 1920, 1080);
        p.wb->setLocalScreens({ScreenInfo{"b1", "", first, 1, true}, ScreenInfo{"b2", "", second, 1, false}});
        p.b->setViewAllowed(p.a->id(), true);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id()));
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy next(p.source.get(), &RemoteView::captureNext);
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);

        // Each screen on a local monitor of its own: one stream, both captured, each sized for
        // the monitor showing it.
        p.viewer->startViewing(p.b->id(), {ViewScreen{0, first, QRect(0, 0, 1920, 1080)},
                                           ViewScreen{1, second, QRect(1920, 0, 2560, 1440)}});
        QTRY_COMPARE(requested.size(), 1);
        const QList<CaptureSpec> captured = specs(requested, 0);
        QCOMPARE(captured.size(), 2);
        QCOMPARE(captured.at(0).index, 0);
        QCOMPARE(captured.at(0).screen, first);
        QCOMPARE(captured.at(0).maxSize, QSize(1920, 1080));
        QCOMPARE(captured.at(1).index, 1);
        QCOMPARE(captured.at(1).screen, second);
        QCOMPARE(captured.at(1).maxSize, QSize(2560, 1440));

        // Frames reach the viewer tagged with their screen...
        FrameEncoder e0, e1;
        const QImage img0 = desktop(QSize(1280, 720), Qt::darkGreen), img1 = desktop(QSize(1920, 1080), Qt::darkRed);
        p.source->frameCaptured(helperFrame(requested, 1, img0.size(), true, e0.encode(img0, true), 0));
        p.source->frameCaptured(helperFrame(requested, 1, img1.size(), true, e1.encode(img1, true), 1));
        QTRY_COMPARE(received.size(), 2);
        QCOMPARE(frameOf(received, 0).value("i").toInteger(), 0);
        QCOMPARE(frameOf(received, 0).value("w").toInteger(), 1280);
        QCOMPARE(frameOf(received, 1).value("i").toInteger(), 1);
        QCOMPARE(frameOf(received, 1).value("w").toInteger(), 1920);
        FrameCanvas canvas;
        QVERIFY(canvas.apply(QSize(1920, 1080), fromFrame(frameOf(received, 1))));
        QVERIFY(difference(canvas.image(), img1) < 3.0);

        // ...and are paced per screen: showing one releases the next frame of that one only, and
        // a screen whose frame was not shown yet gets no second one.
        QTest::qWait(200);
        QCOMPARE(next.size(), 0);
        p.viewer->frameShown(1, 1);
        QTRY_COMPARE(next.size(), 1);
        QCOMPARE(next.first().at(0).toInt(), 1);
        p.source->frameCaptured(helperFrame(requested, 2, img0.size(), false, {}, 0)); // still in flight: dropped
        p.source->frameCaptured(helperFrame(requested, 2, img1.size(), false, {}, 1));
        QTRY_COMPARE(received.size(), 3);
        QCOMPARE(frameOf(received, 2).value("i").toInteger(), 1);
        p.viewer->frameShown(1, 0);
        QTRY_COMPARE(next.size(), 2);
        QCOMPARE(next.last().at(0).toInt(), 0);
        QTest::qWait(200);
        QCOMPARE(received.size(), 3);
    }

    void aNewSetStopsOldScreensAndStartsChangedOnesAfresh()
    {
        ViewPair p;
        QVERIFY(p.setup());
        const QRect first(0, 0, 1280, 720), second(1280, 0, 1920, 1080);
        p.wb->setLocalScreens({ScreenInfo{"b1", "", first, 1, true}, ScreenInfo{"b2", "", second, 1, false}});
        p.b->setViewAllowed(p.a->id(), true);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id()));
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy next(p.source.get(), &RemoteView::captureNext);
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);
        p.viewer->startViewing(p.b->id(), one(first));
        QTRY_COMPARE(requested.size(), 1);
        // A frame of screen 0 is in flight when the set changes.
        const quint32 before = generationOf(requested, 0);
        p.source->frameCaptured(helperFrame(requested, 1, QSize(1280, 720), true, {}, 0));
        QTRY_COMPARE(received.size(), 1);

        // Index 0 now shows the other screen and index 1 joins: both are captured anew.
        p.viewer->setScreens({ViewScreen{0, second, QRect(0, 0, 1920, 1080)}, ViewScreen{1, first, QRect(1920, 0, 1920, 1080)}});
        QTRY_COMPARE(requested.size(), 2);
        QCOMPARE(specs(requested, 1).size(), 2);
        QCOMPARE(specs(requested, 1).at(0).index, 0);
        QCOMPARE(specs(requested, 1).at(0).screen, second);
        QCOMPARE(specs(requested, 1).at(1).index, 1);
        QCOMPARE(specs(requested, 1).at(1).screen, first);
        QVERIFY(generationOf(requested, 0) != before); // starts afresh
        QVERIFY(generationOf(requested, 1) != 0);

        // The acknowledgement of the frame index 0 used to show paces nothing: the screen starts
        // afresh, its first frame goes out at once and its acknowledgement paces it again.
        p.viewer->frameShown(1, 0);
        QTest::qWait(200);
        QCOMPARE(next.size(), 0);
        p.source->frameCaptured(helperFrame(requested, 2, QSize(1920, 1080), true, {}, 0));
        QTRY_COMPARE(received.size(), 2);
        QCOMPARE(frameOf(received, 1).value("i").toInteger(), 0);
        QCOMPARE(frameOf(received, 1).value("w").toInteger(), 1920);
        p.viewer->frameShown(2, 0);
        QTRY_COMPARE(next.size(), 1);
        QCOMPARE(next.first().at(0).toInt(), 0);

        // Down to one screen again: index 1 stops, and a late frame of it goes nowhere.
        p.viewer->setScreens({ViewScreen{0, second, QRect(0, 0, 1920, 1080)}});
        QTRY_COMPARE(requested.size(), 3);
        QCOMPARE(specs(requested, 2).size(), 1);
        QCOMPARE(specs(requested, 2).first().index, 0);
        p.source->frameCaptured(toFrame(1, QSize(1280, 720), true, {}, 1, specs(requested, 1).at(1).generation));
        p.source->frameCaptured(helperFrame(requested, 3, QSize(1920, 1080), false, {}, 0));
        QTRY_COMPARE(received.size(), 3);
        QCOMPARE(frameOf(received, 2).value("i").toInteger(), 0);
        QCOMPARE(frameOf(received, 2).value("s").toInteger(), 3);
    }

    void anOlderViewerIsServedOneScreenAsIndexZero()
    {
        ViewPair p;
        QVERIFY(p.setup());
        const QRect second(1280, 0, 1920, 1080);
        p.wb->setLocalScreens({ScreenInfo{"b1", "", kScreenB, 1, true}, ScreenInfo{"b2", "", second, 1, false}});
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy next(p.source.get(), &RemoteView::captureNext);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);

        // view {w, h} without a screen: the primary one, as index 0, frames no larger than w x h.
        RawViewer v;
        QVERIFY(v.open(*p.a, p.b->id()));
        v.send(ctrl("view", {{"w", 1600}, {"h", 900}}));
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(specs(requested, 0).size(), 1);
        QCOMPARE(specs(requested, 0).first().index, 0);
        QCOMPARE(specs(requested, 0).first().screen, kScreenB);
        QCOMPARE(specs(requested, 0).first().maxSize, QSize(1600, 900));
        QTRY_COMPARE(v.last("status").value("s").toString(), QString("accepted"));

        // Its frames are tagged 0, and an ack without a screen paces that one.
        FrameEncoder encoder;
        const QImage screen = desktop(QSize(1280, 720), Qt::darkGreen);
        p.source->frameCaptured(helperFrame(requested, 1, screen.size(), true, encoder.encode(screen, true)));
        QTRY_COMPARE(v.count("fend"), 1);
        QCOMPARE(v.last("frame").value("i").toInteger(), 0);
        QCOMPARE(v.last("frame").value("s").toInteger(), 1);
        QCOMPARE(v.last("frame").value("n").toInteger(), 60); // 10 x 6 tiles
        QCOMPARE(v.tiles, 60);
        QTest::qWait(200);
        QCOMPARE(next.size(), 0);
        v.send(ctrl("ack", {{"s", 1}}));
        QTRY_COMPARE(next.size(), 1);
        QCOMPARE(next.first().at(0).toInt(), 0);

        // screen {screen}: another of its screens, still index 0 with the frame size asked for.
        v.send(ctrl("screen", {{"screen", rectMap(second)}}));
        QTRY_COMPARE(requested.size(), 2);
        QCOMPARE(specs(requested, 1).size(), 1);
        QCOMPARE(specs(requested, 1).first().index, 0);
        QCOMPARE(specs(requested, 1).first().screen, second);
        QCOMPARE(specs(requested, 1).first().maxSize, QSize(1600, 900));

        v.send(ctrl("stop"));
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!p.source->isStreaming());
    }

    void aLateFrameOfTheCaptureBeforeIsDropped()
    {
        ViewPair p;
        QVERIFY(p.setup());
        const QRect first(0, 0, 1280, 720), second(1280, 0, 1920, 1080);
        p.wb->setLocalScreens({ScreenInfo{"b1", "", first, 1, true}, ScreenInfo{"b2", "", second, 1, false}});
        p.b->setViewAllowed(p.a->id(), true);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id()));
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy next(p.source.get(), &RemoteView::captureNext);
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);
        p.viewer->startViewing(p.b->id(), one(first));
        QTRY_COMPARE(requested.size(), 1);
        const quint32 g0 = generationOf(requested, 0);
        QVERIFY(g0 != 0);
        p.source->frameCaptured(helperFrame(requested, 1, QSize(1280, 720), true, {}, 0));
        QTRY_COMPARE(received.size(), 1);
        p.viewer->frameShown(1, 0);
        QTRY_COMPARE(next.size(), 1);

        // The screen changes while the helper is still sending a frame of the capture before:
        // that frame is dropped, and does not hold up the full frame of the new capture.
        p.viewer->setScreens(one(second));
        QTRY_COMPARE(requested.size(), 2);
        const quint32 g1 = generationOf(requested, 0);
        QVERIFY(g1 != g0);
        p.source->frameCaptured(toFrame(2, QSize(1280, 720), false, {}, 0, g0));
        p.source->frameCaptured(toFrame(3, QSize(1920, 1080), true, {}, 0, g1));
        QTRY_COMPARE(received.size(), 2);
        QCOMPARE(frameOf(received, 1).value("s").toInteger(), 3);
        QCOMPARE(frameOf(received, 1).value("w").toInteger(), 1920);
        QTest::qWait(100);
        QCOMPARE(received.size(), 2);

        // Another stream of the same viewer takes over (its old one was lost): every screen starts
        // afresh, so the helper sends a full frame tagged anew, and a frame of the old capture,
        // which the new viewer never saw begin, goes nowhere.
        RemoteView viewer2(p.a.get(), p.wa.get());
        QSignalSpy received2(&viewer2, &RemoteView::frameReceived);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        viewer2.startViewing(p.b->id(), one(second));
        QTRY_COMPARE(requested.size(), 3);
        QTRY_COMPARE(failed.size(), 1);
        const quint32 g2 = generationOf(requested, 0);
        QVERIFY(g2 != g1);
        QCOMPARE(specs(requested, 2).first().screen, second);
        p.source->frameCaptured(toFrame(4, QSize(1920, 1080), false, {}, 0, g1));
        p.source->frameCaptured(toFrame(5, QSize(1920, 1080), true, {}, 0, g2));
        QTRY_COMPARE(received2.size(), 1);
        QCOMPARE(frameOf(received2, 0).value("s").toInteger(), 5);
        QTest::qWait(100);
        QCOMPARE(received2.size(), 1);
        viewer2.stopViewing();
    }

    void aScreenNeverAcknowledgedEndsTheStreamWhileAnotherFlows()
    {
        ViewPair p;
        QVERIFY(p.setup());
        const QRect first(0, 0, 1280, 720), second(1280, 0, 1920, 1080);
        p.wb->setLocalScreens({ScreenInfo{"b1", "", first, 1, true}, ScreenInfo{"b2", "", second, 1, false}});
        p.b->setViewAllowed(p.a->id(), true);
        p.source->setAckTimeout(300);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id()));
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy next(p.source.get(), &RemoteView::captureNext);
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        p.viewer->startViewing(p.b->id(), {ViewScreen{0, first, QRect(0, 0, 1920, 1080)},
                                           ViewScreen{1, second, QRect(1920, 0, 1920, 1080)}});
        QTRY_COMPARE(requested.size(), 1);
        QElapsedTimer elapsed; // from the frame of screen 0, which is never shown
        elapsed.start();
        p.source->frameCaptured(helperFrame(requested, 1, QSize(1280, 720), true, {}, 0));
        p.source->frameCaptured(helperFrame(requested, 1, QSize(1920, 1080), true, {}, 1));
        QTRY_COMPARE(received.size(), 2);
        p.viewer->frameShown(1, 1);
        QTRY_COMPARE(next.size(), 1);

        // Screen 1 keeps flowing (frame, shown, next frame...); screen 0 is never shown: the
        // watchdog runs from its frame, not from the latest traffic, and ends the stream.
        quint32 seq = 1;
        while (stopped.isEmpty() && elapsed.elapsed() < 3000) {
            ++seq;
            p.source->frameCaptured(helperFrame(requested, seq, QSize(1920, 1080), false, {}, 1));
            QVERIFY(QTest::qWaitFor([&] { return received.size() == qsizetype(seq) + 1 || !stopped.isEmpty(); }, 1000));
            if (!stopped.isEmpty())
                break;
            p.viewer->frameShown(seq, 1);
            QVERIFY(QTest::qWaitFor([&] { return next.size() == qsizetype(seq) || !stopped.isEmpty(); }, 1000));
            QTest::qWait(20);
        }
        QCOMPARE(stopped.size(), 1);
        QVERIFY2(elapsed.elapsed() >= 250, qPrintable(QString::number(elapsed.elapsed()))); // not before the timeout
        QVERIFY(!p.source->isStreaming());
    }

    void aScreensMessageWithoutTheSetIsIgnored()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        RawViewer v;
        QVERIFY(v.open(*p.a, p.b->id()));
        QCborMap view = ctrl("view", {{"v", 2}});
        view.insert(QStringLiteral("screens"), QCborArray{viewEntry(0, kScreenB)});
        v.send(view);
        QTRY_COMPARE(requested.size(), 1);
        QTRY_COMPARE(v.last("status").value("s").toString(), QString("accepted"));

        // Not the legacy single-screen request: nothing is captured anew.
        v.send(ctrl("screens"));
        v.send(ctrl("screens", {{"w", 1}, {"h", 1}}));
        QTest::qWait(200);
        QCOMPARE(requested.size(), 1);
        QVERIFY(p.source->isStreaming());

        // A proper set still replaces it.
        QCborMap screens = ctrl("screens");
        screens.insert(QStringLiteral("screens"), QCborArray{viewEntry(0, kScreenB, QSize(800, 600))});
        v.send(screens);
        QTRY_COMPARE(requested.size(), 2);
        QCOMPARE(specs(requested, 1).first().maxSize, QSize(800, 600));
    }

    void anOlderSourceIsSpokenToInTheSingleScreenForms()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.source.reset(); // B answers by hand
        RawSource s;
        s.listen(*p.b);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id()));
        claimVersion(*p.b, *p.a, QStringLiteral("0.1.19"));
        QTRY_VERIFY(!p.viewer->peerSupportsMultiView(p.b->id()));
        QSignalSpy state(p.viewer.get(), &RemoteView::viewStateChanged);
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);

        // view {w, h, screen} for the one screen, whatever its index here.
        const QRect other(1280, 0, 1920, 1080);
        p.viewer->startViewing(p.b->id(), one(kScreenB, 1));
        QTRY_COMPARE(s.count("view"), 1);
        const QCborMap view = s.last("view");
        QCOMPARE(view.value("w").toInteger(), 1920);
        QCOMPARE(view.value("h").toInteger(), 1080);
        QCOMPARE(view.value("screen").toMap().value("w").toInteger(), 1280);
        QVERIFY(!view.contains(QStringLiteral("screens")));
        QVERIFY(!view.contains(QStringLiteral("v")));

        // Another screen while its user is still asked: screen {screen}, which such a source takes
        // for its pending request.
        s.send(ctrl("status", {{"s", "pending"}, {"to", 5000}}));
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::AwaitingApproval));
        p.viewer->setScreens(one(other, 1));
        QTRY_COMPARE(s.count("screen"), 1);
        QCOMPARE(s.last("screen").value("screen").toMap().value("x").toInteger(), 1280);
        QVERIFY(!s.last("screen").contains(QStringLiteral("screens")));

        // Its frames carry no index: they are of the screen asked for. Shown: ack {s} without one.
        s.send(ctrl("status", {{"s", "accepted"}}));
        s.send(ctrl("frame", {{"s", 7}, {"w", 640}, {"h", 360}, {"full", true}, {"n", 1}}));
        s.sendTile(100);
        s.send(ctrl("fend", {{"s", 7}}));
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(frameOf(received, 0).value("i").toInteger(), 1);
        QCOMPARE(frameOf(received, 0).value("s").toInteger(), 7);
        QCOMPARE(frameOf(received, 0).value("tiles").toArray().size(), 1);
        QCOMPARE(states(state).last(), RemoteView::ViewState::Streaming);
        p.viewer->frameShown(7, 1);
        QTRY_COMPARE(s.count("ack"), 1);
        QCOMPARE(s.last("ack").value("s").toInteger(), 7);
        QVERIFY(!s.last("ack").contains(QStringLiteral("i")));
        QCOMPARE(failed.size(), 0);
        p.viewer->stopViewing();
        QTRY_COMPARE(s.count("stop"), 1);
    }

    void framesBeyondWhatWasAskedAreDroppedAndHugeOnesEndTheView()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.source.reset();
        RawSource s;
        s.listen(*p.b);
        QTRY_VERIFY(p.viewer->peerSupportsMultiView(p.b->id()));
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        const QRect second(1280, 0, 1920, 1080);
        p.viewer->startViewing(p.b->id(), {ViewScreen{0, kScreenB, QRect(0, 0, 1920, 1080)},
                                           ViewScreen{1, second, QRect(1920, 0, 3840, 2160)}});
        QTRY_COMPARE(s.count("view"), 1);
        s.send(ctrl("status", {{"s", "accepted"}}));

        // Wider than the limit given for screen 0, or of a screen never asked for: read through
        // and dropped, the view goes on.
        s.send(ctrl("frame", {{"i", 0}, {"s", 1}, {"w", 2000}, {"h", 1080}, {"full", true}, {"n", 1}}));
        s.sendTile(100);
        s.send(ctrl("fend", {{"s", 1}}));
        s.send(ctrl("frame", {{"i", 5}, {"s", 2}, {"w", 640}, {"h", 360}, {"full", true}, {"n", 0}}));
        s.send(ctrl("fend", {{"s", 2}}));
        s.send(ctrl("frame", {{"i", 0}, {"s", 3}, {"w", 1280}, {"h", 720}, {"full", true}, {"n", 1}}));
        s.sendTile(100);
        s.send(ctrl("fend", {{"s", 3}}));
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(frameOf(received, 0).value("s").toInteger(), 3);
        QCOMPARE(frameOf(received, 0).value("tiles").toArray().size(), 1);
        QCOMPARE(failed.size(), 0);
        QVERIFY(p.viewer->isViewing());

        // Within the size limit but more tile bytes than one frame may carry: the view ends
        // before the helper would be handed something its IPC refuses.
        const int tileBytes = 65000;
        const int tiles = RemoteView::kMaxFrameBytes / tileBytes + 2;
        s.send(ctrl("frame", {{"i", 1}, {"s", 4}, {"w", 3840}, {"h", 2160}, {"full", true}, {"n", tiles}}));
        for (int n = 0; n < tiles; ++n)
            s.sendTile(tileBytes);
        s.send(ctrl("fend", {{"s", 4}}));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("Invalid frame"), qPrintable(lastReason(failed)));
        QCOMPARE(received.size(), 1);
    }

    void tooManyOrForeignScreensAreABadRequest()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QList<ScreenInfo> nine;
        for (int i = 0; i <= RemoteView::kMaxViewScreens; ++i)
            nine.append(ScreenInfo{QStringLiteral("b%1").arg(i), "", QRect(1920 * i, 0, 1920, 1080), 1, i == 0});
        p.wb->setLocalScreens(nine);
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        // The source's answer to a `view` request listing `screens`.
        const auto answer = [&](const QCborArray &screens) {
            RawViewer v;
            if (!v.open(*p.a, p.b->id()))
                return QStringLiteral("not opened");
            QCborMap view = ctrl("view", {{"v", 2}});
            view.insert(QStringLiteral("screens"), screens);
            v.send(view);
            if (!QTest::qWaitFor([&] { return !v.last("refused").isEmpty() || !v.last("status").isEmpty(); }, 5000))
                return QStringLiteral("no answer");
            return v.last("refused").isEmpty() ? v.last("status").value("s").toString() : v.last("refused").value("c").toString();
        };
        QCborArray all;
        for (int i = 0; i < nine.size(); ++i)
            all.append(viewEntry(i, nine.at(i).geometry));

        // One screen too many, a repeated index, a screen that is not the source's, a frame
        // larger than kMaxFrameSide: refused, nothing captured.
        QCOMPARE(answer(all), QString("bad"));
        QCOMPARE(answer({viewEntry(0, nine.at(0).geometry), viewEntry(0, nine.at(1).geometry)}), QString("bad"));
        QCOMPARE(answer({viewEntry(0, nine.at(0).geometry), viewEntry(1, QRect(-5000, 0, 800, 600))}), QString("bad"));
        QCOMPARE(answer({viewEntry(0, nine.at(0).geometry, QSize(RemoteView::kMaxFrameSide + 1, 1080))}), QString("bad"));
        QCOMPARE(answer({}), QString("bad"));
        QCOMPARE(requested.size(), 0);

        // Exactly the cap is fine.
        all.removeLast();
        QCOMPARE(answer(all), QString("accepted"));
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(specs(requested, 0).size(), RemoteView::kMaxViewScreens);
    }

    void asksTheSourceUserFirst()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy state(p.viewer.get(), &RemoteView::viewStateChanged);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(asked.size(), 1);
        QCOMPARE(asked.first().at(0).toByteArray(), p.a->id());
        QCOMPARE(asked.first().at(2).toInt(), RemoteView::kPromptTimeoutMs);
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::AwaitingApproval));
        QTest::qWait(200);
        QCOMPARE(requested.size(), 0);
        QCOMPARE(asked.size(), 1);
        QVERIFY(p.viewer->isViewing());
    }

    void aGuestNeverViewsTheScreen()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setTrust(p.a->id(), TrustLevel::Guest);
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("guests"), qPrintable(lastReason(failed)));
        QCOMPARE(asked.size(), 0); // its user is not even asked
    }

    void deniedRequestIsRefusedWithAReason()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy closed(p.source.get(), &RemoteView::authorizationClosed);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(asked.size(), 1);
        p.source->answer(p.a->id(), RemoteView::Decision::Deny);
        QCOMPARE(closed.size(), 1);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("denied"), qPrintable(lastReason(failed)));

        // Asking again right away is refused without bothering the user (cool-down).
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(failed.size(), 2);
        QVERIFY(lastReason(failed).contains("denied"));
        QCOMPARE(asked.size(), 1);
    }

    void allowOnceDoesNotPersist()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy streaming(p.source.get(), &RemoteView::streamingChanged);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(asked.size(), 1);
        p.source->answer(p.a->id(), RemoteView::Decision::Once);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(streaming.size(), 1);
        QCOMPARE(p.source->streamingTo(), p.a->id());
        QVERIFY(!p.b->isViewAllowed(p.a->id()));

        // The next request asks again.
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(asked.size(), 2);
    }

    void alwaysAllowIsPersistedAndRevocable()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(asked.size(), 1);
        p.source->answer(p.a->id(), RemoteView::Decision::Always);
        QCOMPARE(requested.size(), 1);
        QVERIFY(p.b->isViewAllowed(p.a->id()));
        PeerStore store;
        QVERIFY(store.load(p.dirB.path()));
        QVERIFY(store.find(p.a->id()));
        QVERIFY(store.find(p.a->id())->allowView);

        p.viewer->stopViewing();
        QTRY_COMPARE(stopped.size(), 1);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 2);
        QCOMPARE(asked.size(), 1); // no prompt this time

        p.b->setViewAllowed(p.a->id(), false);
        p.source->revoke(p.a->id());
        QTRY_COMPARE(stopped.size(), 2);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("revoked"), qPrintable(lastReason(failed)));
        QVERIFY(store.load(p.dirB.path()));
        QVERIFY(!store.find(p.a->id())->allowView);
    }

    void promptTimesOutAsDenied()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.source->setPromptTimeout(200);
        QSignalSpy closed(p.source.get(), &RemoteView::authorizationClosed);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("in time"), qPrintable(lastReason(failed)));
        QCOMPARE(closed.size(), 1);
    }

    void refusesWithoutAUserToAsk()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.source->setLocalUserPresent(false);
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("Nobody"), qPrintable(lastReason(failed)));
        QCOMPARE(asked.size(), 0);
    }

    void stopsWhenTheViewerIsNoLongerMaster()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        p.wa->setSharing(false);
        QTRY_VERIFY(p.wb->masterId().isEmpty());
        QTRY_COMPARE(stopped.size(), 1);
        QTRY_COMPARE(failed.size(), 1);
    }

    void remoteViewOffRefusesEveryoneAndKeepsTheGrants()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true); // given in advance
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(asked.size(), 0);

        // Turned off: the live view ends, and so does a new request, even with the grant.
        p.b->setRemoteViewEnabled(false);
        QTRY_COMPARE(stopped.size(), 1);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("remote view is off"), qPrintable(lastReason(failed)));
        QVERIFY(!p.b->isViewAllowed(p.a->id()));
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(failed.size(), 2);
        QVERIFY2(lastReason(failed).contains("remote view is off"), qPrintable(lastReason(failed)));
        QCOMPARE(requested.size(), 1);
        QCOMPARE(asked.size(), 0);
        bool kept = false;
        for (const PeerView &peer : p.b->peers())
            kept |= peer.id == p.a->id() && peer.allowView;
        QVERIFY(kept);

        // Back on: the grant still holds.
        p.b->setRemoteViewEnabled(true);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 2);
        QCOMPARE(asked.size(), 0);
    }

    void viewStartsOnTheScreenBeyondTheEdge()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        Workspace wa(a.get(), dirA.path()), wb(b.get(), dirB.path());
        wa.setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        // B: a secondary screen on the left, next to A, and its primary one beyond it.
        wb.setLocalScreens({ScreenInfo{"b2", "", QRect(0, 0, 1920, 1080), 1, false},
                            ScreenInfo{"b1", "", QRect(1920, 0, 1920, 1080), 1, true}});
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(wa.screens(b->id()).size(), 2);
        wa.setLayout({{a->id(), {0, 0}}, {b->id(), {1920, 0}}});
        wa.setRemoteView(b->id(), true);
        wa.setSharing(true);
        QTRY_COMPARE(wb.masterId(), a->id());

        InputController ia(a.get(), &wa);
        QSignalSpy viewRequested(&ia, &InputController::viewRequested);
        ia.sessionConnected();
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 540}}));
        QVERIFY(ia.isViewing());
        QCOMPARE(viewRequested.size(), 1);
        QCOMPARE(shownIn(viewRequested.first()).size(), 1);
        QCOMPARE(shownIn(viewRequested.first()).first().remote, QRect(0, 0, 1920, 1080)); // not the primary one
        QCOMPARE(ia.cursor().x(), 1); // one pixel inside, as on any crossing

        // Further right: the primary one; back left twice: home.
        QSignalSpy screenChanged(&ia, &InputController::viewScreensChanged);
        ia.handleSessionMessage(cmd("mm", {{"x", 2000}, {"y", 0}}));
        QCOMPARE(screenChanged.size(), 1);
        QCOMPARE(shownIn(screenChanged.first()).size(), 1); // same monitor: one screen at a time
        QCOMPARE(shownIn(screenChanged.first()).first().remote, QRect(1920, 0, 1920, 1080));
        ia.handleSessionMessage(cmd("mm", {{"x", -2000}, {"y", 0}}));
        QCOMPARE(screenChanged.size(), 2);
        ia.handleSessionMessage(cmd("mm", {{"x", -2000}, {"y", 0}}));
        QVERIFY(!ia.isViewing());
        QCOMPARE(ia.activeDevice(), a->id());
    }

    void remoteViewOffHereEntersFlaggedDevicesAsUsual()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        Workspace wa(a.get(), dirA.path()), wb(b.get(), dirB.path());
        wa.setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        wb.setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 1920, 1080), 1, true}});
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(wa.screens(b->id()).size(), 1);
        wa.setLayout({{a->id(), {0, 0}}, {b->id(), {1920, 0}}});
        wa.setRemoteView(b->id(), true);
        wa.setSharing(true);
        QTRY_COMPARE(wb.masterId(), a->id());

        a->setRemoteViewEnabled(false);
        InputController ia(a.get(), &wa), ib(b.get(), &wb);
        QSignalSpy viewRequested(&ia, &InputController::viewRequested);
        QSignalSpy toB(&ib, &InputController::toSession);
        ia.sessionConnected();
        ib.sessionConnected();
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 540}}));
        QVERIFY(!ia.isViewing());
        QCOMPARE(viewRequested.size(), 0);
        QCOMPARE(ia.activeDevice(), b->id());
        QTRY_VERIFY(!lastEvent(toB, "mabs").isEmpty()); // the cursor really moved there
    }

    void unpairStopsTheStream()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        p.b->unpair(p.a->id());
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!p.source->isStreaming());
    }

    void userLeavingEndsTheStream()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        p.source->setLocalUserPresent(false);
        QCOMPARE(stopped.size(), 1); // at once, not when the socket is gone
        QVERIFY(!p.source->isStreaming());
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("Nobody"), qPrintable(lastReason(failed)));
    }

    void sourceGivesUpOnASilentViewer()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        p.source->setAckTimeout(300);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        FrameEncoder encoder;
        const QImage screen = desktop(QSize(640, 360), Qt::darkGreen);
        p.source->frameCaptured(helperFrame(requested, 1, screen.size(), true, encoder.encode(screen, true)));
        // The viewer never acknowledges it.
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!p.source->isStreaming());
    }

    void viewerTimesOutWithoutFrames()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        p.viewer->setViewTimeouts(500, 500);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        QTRY_COMPARE(failed.size(), 1); // frameCaptured is never called
        QVERIFY2(lastReason(failed).contains("capture failed"), qPrintable(lastReason(failed)));
        QVERIFY(!p.viewer->isViewing());
        QTRY_COMPARE(stopped.size(), 1);
    }

    void captureFailureReachesTheViewer()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        p.source->captureFailed("x");
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("capture"), qPrintable(lastReason(failed)));
    }

    void rejectsOversizedFrames()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        QSignalSpy received(p.viewer.get(), &RemoteView::frameReceived);
        p.viewer->startViewing(p.b->id(), one());
        QTRY_COMPARE(requested.size(), 1);
        p.source->frameCaptured(helperFrame(requested, 1, QSize(100000, 10), true, {}));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("Invalid frame"), qPrintable(lastReason(failed)));
        QCOMPARE(received.size(), 0);
    }

    void remoteInputIsBlockedDuringThePrompt()
    {
        ViewPair p;
        QVERIFY(p.setup());
        InputController ib(p.b.get(), p.wb.get());
        QSignalSpy toB(&ib, &InputController::toSession);
        ib.sessionConnected();
        ib.setRemoteInputBlocked(p.a->id(), true);
        QVERIFY(!lastEvent(toB, "releaseAll").isEmpty());

        QCborMap key;
        key.insert(QStringLiteral("t"), QStringLiteral("in.key"));
        key.insert(QStringLiteral("s"), 0x1E);
        key.insert(QStringLiteral("d"), true);
        QVERIFY(p.a->sendMessage(p.b->id(), key));
        QTest::qWait(300);
        QCOMPARE(countEvents(toB, "key"), 0);

        ib.setRemoteInputBlocked(p.a->id(), false);
        QVERIFY(p.a->sendMessage(p.b->id(), key));
        QTRY_COMPARE(countEvents(toB, "key"), 1);
    }

    void remoteViewModeInTheInputController()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        Workspace wa(a.get(), dirA.path()), wb(b.get(), dirB.path());
        wa.setLocalScreens({ScreenInfo{"a1", "", QRect(0, 0, 1920, 1080), 1, true}});
        // B: a 4K primary screen at (0,0) and a second one to its right.
        wb.setLocalScreens({ScreenInfo{"b1", "", QRect(0, 0, 3840, 2160), 2, true},
                            ScreenInfo{"b2", "", QRect(3840, 0, 1920, 1080), 1, false}});
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(wa.devices().size(), 2);
        QTRY_COMPARE(wa.screens(b->id()).size(), 2);
        wa.setLayout({{a->id(), {0, 0}}, {b->id(), {1920, 0}}});
        wa.setRemoteView(b->id(), true);
        wa.setSharing(true);
        QTRY_COMPARE(wb.masterId(), a->id());
        QTRY_VERIFY(wb.isRemoteView(b->id())); // replicated with the layout

        InputController ia(a.get(), &wa), ib(b.get(), &wb);
        QSignalSpy toA(&ia, &InputController::toSession), toB(&ib, &InputController::toSession);
        QSignalSpy viewRequested(&ia, &InputController::viewRequested), viewEnded(&ia, &InputController::viewEnded);
        ia.sessionConnected();
        ib.sessionConnected();

        // Halfway down A's right edge: B's primary screen, halfway down its left edge.
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 540}}));
        QVERIFY(ia.isViewing());
        QCOMPARE(viewRequested.size(), 1);
        QCOMPARE(shownIn(viewRequested.first()).size(), 1);
        QCOMPARE(shownIn(viewRequested.first()).first().monitor, QRect(0, 0, 1920, 1080));
        const QCborMap open = lastEvent(toA, "viewOpen");
        QCOMPARE(open.value("screens").toArray().size(), 1);
        QCOMPARE(open.value("screens").toArray().first().toMap().value("w").toInteger(), 1920);
        QCOMPARE(open.value("screens").toArray().first().toMap().value("rw").toInteger(), 3840);
        QCOMPARE(open.value("active").toInteger(), 0);
        QCOMPARE(ia.cursor(), QPoint(1, 1081)); // edges map to edges (one pixel inside): 540 / 1079 * 2159

        // Nothing reaches B until it accepts the view.
        QTest::qWait(200);
        QVERIFY(lastEvent(toB, "mabs").isEmpty());
        ia.setViewAccepted(b->id());
        QTRY_COMPARE(lastEvent(toB, "mabs").value("y").toInteger(), 1081);

        // Past the right edge of B's primary screen: its second screen (shorter, so the cursor
        // lands on its bottom row) is shown instead.
        QSignalSpy screenChanged(&ia, &InputController::viewScreensChanged);
        ia.handleSessionMessage(cmd("mm", {{"x", 5000}, {"y", 0}}));
        QVERIFY(ia.isViewing());
        QCOMPARE(ia.cursor(), QPoint(5001, 1079));
        QCOMPARE(screenChanged.size(), 1);
        QCOMPARE(shownIn(screenChanged.first()).first().remote, QRect(3840, 0, 1920, 1080));
        QCOMPARE(shownIn(screenChanged.first()).first().index, 1);
        const QCborMap show = lastEvent(toA, "viewShow");
        QCOMPARE(show.value("screens").toArray().size(), 1);
        QCOMPARE(show.value("screens").toArray().first().toMap().value("rw").toInteger(), 1920);
        QCOMPARE(show.value("active").toInteger(), 1);
        QCOMPARE(lastEvent(toA, "viewCursor").value("i").toInteger(), 1);
        QCOMPARE(lastEvent(toA, "viewCursor").value("x").toInteger(), 1161); // relative to that screen
        QTRY_COMPARE(lastEvent(toB, "mabs").value("x").toInteger(), 5001);

        // Its right edge leads nowhere: the cursor stops there.
        ia.handleSessionMessage(cmd("mm", {{"x", 5000}, {"y", 0}}));
        QCOMPARE(ia.cursor(), QPoint(5759, 1079));
        ia.handleSessionMessage(cmd("mm", {{"x", 100}, {"y", 0}}));
        QCOMPARE(ia.cursor(), QPoint(5759, 1079));
        QCOMPARE(screenChanged.size(), 1);

        // Back left onto the primary screen.
        ia.handleSessionMessage(cmd("mm", {{"x", -2000}, {"y", 0}}));
        QCOMPARE(ia.cursor(), QPoint(3759, 1079));
        QCOMPARE(screenChanged.size(), 2);
        QCOMPARE(shownIn(screenChanged.last()).first().remote, QRect(0, 0, 3840, 2160));
        QCOMPARE(lastEvent(toA, "viewShow").value("screens").toArray().first().toMap().value("rw").toInteger(), 3840);

        // Back out through the left edge, a quarter of the way down: home at the same proportion.
        ia.handleSessionMessage(cmd("mm", {{"x", 0}, {"y", -1079 + 541}}));
        ia.handleSessionMessage(cmd("mm", {{"x", -4000}, {"y", 0}}));
        QVERIFY(!ia.isViewing());
        QCOMPARE(ia.activeDevice(), a->id());
        QCOMPARE(viewEnded.size(), 1);
        QVERIFY(!lastEvent(toA, "viewClose").isEmpty());
        const QCborMap capture = lastEvent(toA, "capture");
        QCOMPARE(capture.value("on").toBool(), false);
        QCOMPARE(capture.value("x").toInteger(), 1918);
        QCOMPARE(capture.value("y").toInteger(), 270);

        // A failed view returns home next to the crossed edge, and the edge does not ask again
        // right away.
        QTest::qWait(300); // past the return guard
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 540}}));
        QVERIFY(ia.isViewing());
        QCOMPARE(viewRequested.size(), 2);
        const int closes = countEvents(toA, "viewClose");
        ia.cancelView();
        QVERIFY(!ia.isViewing());
        QCOMPARE(ia.activeDevice(), a->id());
        QCOMPARE(countEvents(toA, "viewClose"), closes + 1);
        QCOMPARE(viewEnded.size(), 2);
        const QCborMap home = lastEvent(toA, "capture");
        QCOMPARE(home.value("on").toBool(), false);
        QCOMPARE(home.value("x").toInteger(), 1918);
        QCOMPARE(home.value("y").toInteger(), 540);
        QTest::qWait(300);
        ia.handleSessionMessage(cmd("edge", {{"s", int(Side::Right)}, {"x", 1919}, {"y", 540}}));
        QVERIFY(!ia.isViewing());
        QCOMPARE(viewRequested.size(), 2);
    }

    void aHiddenScreenIsCrossedOver()
    {
        // B's middle screen is hidden: the view starts on the first one, on the crossed monitor,
        // and pushing past its right edge lands on the third one (never on the hidden one), which
        // shares the monitor; back left the same way. Past the third one's right edge: nothing.
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 3), {{"b2", Workspace::kHiddenScreen}}));
        v.cross();
        QVERIFY(v.ia->isViewing());
        QCOMPARE(v.requested->size(), 1);
        QCOMPARE(shownIn(v.requested->first()).size(), 1);
        QCOMPARE(shownIn(v.requested->first()).first().remote, kB1);
        QCOMPARE(shownIn(v.requested->first()).first().monitor, kA2);
        QCOMPARE(v.ia->cursor(), QPoint(1, 540));
        v.move(3000, 0);
        QVERIFY(v.ia->isViewing());
        QCOMPARE(v.ia->cursor(), QPoint(3840, 540));
        QCOMPARE(v.changed->size(), 1);
        QCOMPARE(shownIn(v.changed->first()).size(), 1);
        QCOMPARE(shownIn(v.changed->first()).first().index, 2);
        QCOMPARE(shownIn(v.changed->first()).first().monitor, kA2);
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("active").toInteger(), 2);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("i").toInteger(), 2);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("x").toInteger(), 0);
        v.move(3000, 0);
        QCOMPARE(v.ia->cursor(), QPoint(5759, 540));
        QCOMPARE(v.changed->size(), 1);
        v.move(-3000, 0);
        QCOMPARE(v.ia->cursor(), QPoint(1919, 540));
        QCOMPARE(v.changed->size(), 2);
        QCOMPARE(shownIn(v.changed->last()).first().index, 0);
        QVERIFY(v.ia->isViewing());
    }

    void aScreenBehindAHiddenOneIsReachedOnItsMonitor()
    {
        // The third screen is shown on a1 and the middle one hidden: both visible screens open at
        // once, and pushing right from the first one crosses the hidden one onto the third.
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 3), {{"b2", Workspace::kHiddenScreen}, {"b3", "a1"}}));
        v.cross();
        QVERIFY(v.ia->isViewing());
        const QList<ViewScreen> shown = shownIn(v.requested->first());
        QCOMPARE(shown.size(), 2);
        QCOMPARE(shown.at(0).index, 0);
        QCOMPARE(shown.at(0).monitor, kA2);
        QCOMPARE(shown.at(1).index, 2);
        QCOMPARE(shown.at(1).monitor, kA1);
        v.move(3000, 0);
        QCOMPARE(v.ia->cursor(), QPoint(3840, 540));
        QCOMPARE(v.changed->size(), 0); // both were displayed already
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("screens").toArray().size(), 2);
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("active").toInteger(), 2);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("i").toInteger(), 2);
        v.move(-10, 0);
        QCOMPARE(v.ia->cursor(), QPoint(1919, 540));
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("i").toInteger(), 0);
        QVERIFY(v.ia->isViewing());
    }

    void aHiddenScreenIsNeverTheEntry()
    {
        // The screen beyond the edge is hidden: the view starts on the nearest visible one, and
        // leaving it through the side the cursor came in still leads home.
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 3), {{"b1", Workspace::kHiddenScreen}}));
        v.cross();
        QVERIFY(v.ia->isViewing());
        QCOMPARE(shownIn(v.requested->first()).size(), 1);
        QCOMPARE(shownIn(v.requested->first()).first().index, 1);
        QCOMPARE(shownIn(v.requested->first()).first().remote, kB2);
        QCOMPARE(shownIn(v.requested->first()).first().monitor, kA2);
        QCOMPARE(v.ia->cursor(), QPoint(1921, 540));
        QCOMPARE(lastEvent(*v.toA, "viewOpen").value("active").toInteger(), 1);
        v.move(-3000, 0);
        QVERIFY(!v.ia->isViewing());
        QCOMPARE(v.ia->activeDevice(), v.a->id());
        QCOMPARE(lastEvent(*v.toA, "capture").value("on").toBool(), false);
        QCOMPARE(lastEvent(*v.toA, "capture").value("x").toInteger(), 3838); // A's right edge, inset
        QCOMPARE(lastEvent(*v.toA, "capture").value("y").toInteger(), 540);
    }

    void aMappedScreenOpensOnItsMonitorAndHomeIsThere()
    {
        // b1 is shown on a1 while the edge is crossed on a2: both of B's screens are shown at
        // once, each on its monitor, and leaving b1 lands on the edge of a1.
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 2), {{"b1", "a1"}}));
        v.cross();
        QVERIFY(v.ia->isViewing());
        QCOMPARE(v.requested->size(), 1);
        const QList<ViewScreen> shown = shownIn(v.requested->first());
        QCOMPARE(shown.size(), 2);
        QCOMPARE(shown.at(0).index, 0);
        QCOMPARE(shown.at(0).remote, kB1);
        QCOMPARE(shown.at(0).monitor, kA1);
        QCOMPARE(shown.at(1).index, 1);
        QCOMPARE(shown.at(1).remote, kB2);
        QCOMPARE(shown.at(1).monitor, kA2);
        const QCborMap open = lastEvent(*v.toA, "viewOpen");
        QCOMPARE(open.value("screens").toArray().size(), 2);
        QCOMPARE(open.value("screens").toArray().at(0).toMap().value("x").toInteger(), 0);
        QCOMPARE(open.value("screens").toArray().at(1).toMap().value("x").toInteger(), 1920);
        QCOMPARE(open.value("active").toInteger(), 0);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("i").toInteger(), 0);

        // Onto b2: only the active screen changes, both windows keep their screens.
        v.move(3000, 0);
        QCOMPARE(v.ia->cursor(), QPoint(3001, 540));
        QCOMPARE(v.changed->size(), 0);
        const QCborMap show = lastEvent(*v.toA, "viewShow");
        QCOMPARE(show.value("screens").toArray().size(), 2);
        QCOMPARE(show.value("active").toInteger(), 1);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("i").toInteger(), 1);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("x").toInteger(), 1081);

        // Back onto b1 and out through its left edge: home on a1, not on the crossed monitor.
        v.move(-3000, 0);
        QCOMPARE(v.ia->cursor(), QPoint(1, 540));
        QCOMPARE(v.changed->size(), 0);
        v.move(-10, 0);
        QVERIFY(!v.ia->isViewing());
        QCOMPARE(v.ia->activeDevice(), v.a->id());
        QVERIFY(!lastEvent(*v.toA, "viewClose").isEmpty());
        QCOMPARE(lastEvent(*v.toA, "capture").value("on").toBool(), false);
        QCOMPARE(lastEvent(*v.toA, "capture").value("x").toInteger(), 1918);
        QCOMPARE(lastEvent(*v.toA, "capture").value("y").toInteger(), 540);
    }

    void aPlanChangedDuringTheViewAppliesAtOnce()
    {
        // The plan changes while B is viewed: b2 goes to a1 (both shown at once, the cursor
        // stays), then b1, which has the cursor, is hidden (the cursor moves to b2, now alone on
        // the crossed monitor).
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 2)));
        v.cross();
        QVERIFY(v.ia->isViewing());
        QCOMPARE(shownIn(v.requested->first()).size(), 1);
        const QPoint cursor = v.ia->cursor();

        v.wa->setViewPlan(v.b->id(), {{"b2", "a1"}});
        QVERIFY(v.ia->isViewing());
        QCOMPARE(v.changed->size(), 1);
        QList<ViewScreen> shown = shownIn(v.changed->last());
        QCOMPARE(shown.size(), 2);
        QCOMPARE(shown.at(0).remote, kB1);
        QCOMPARE(shown.at(0).monitor, kA2);
        QCOMPARE(shown.at(1).remote, kB2);
        QCOMPARE(shown.at(1).monitor, kA1);
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("screens").toArray().size(), 2);
        QCOMPARE(v.ia->cursor(), cursor);

        v.wa->setViewPlan(v.b->id(), {{"b1", Workspace::kHiddenScreen}});
        QVERIFY(v.ia->isViewing());
        QCOMPARE(v.changed->size(), 2);
        shown = shownIn(v.changed->last());
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown.first().remote, kB2);
        QCOMPARE(shown.first().monitor, kA2);
        QCOMPARE(v.ia->cursor(), kB2.center());
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("active").toInteger(), 1);
        QCOMPARE(lastEvent(*v.toA, "viewCursor").value("i").toInteger(), 1);

        // The same plan again changes nothing.
        v.wa->setViewPlan(v.b->id(), {{"b1", Workspace::kHiddenScreen}});
        QCOMPARE(v.changed->size(), 2);

        // A plan hiding every screen is ignored, during the view as before it: both screens are
        // back on the crossed monitor, the cursor staying on b2.
        v.wa->setViewPlan(v.b->id(), {{"b1", Workspace::kHiddenScreen}, {"b2", Workspace::kHiddenScreen}});
        QVERIFY(v.ia->isViewing());
        QCOMPARE(v.changed->size(), 3);
        shown = shownIn(v.changed->last());
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown.first().remote, kB2);
        QCOMPARE(shown.first().monitor, kA2);
        QCOMPARE(v.ia->cursor(), kB2.center());
        v.move(-1920, 0); // b1 can be entered again
        QCOMPARE(shownIn(v.changed->last()).first().remote, kB1);
    }

    void aPlanHidingEveryScreenIsIgnored()
    {
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 2), {{"b1", Workspace::kHiddenScreen}, {"b2", Workspace::kHiddenScreen}}));
        QCOMPARE(v.wa->viewPlan(v.b->id()).size(), 2); // kept as the user left it
        v.cross();
        QVERIFY(v.ia->isViewing());
        QCOMPARE(shownIn(v.requested->first()).size(), 1);
        QCOMPARE(shownIn(v.requested->first()).first().remote, kB1);
        QCOMPARE(shownIn(v.requested->first()).first().monitor, kA2);
        // As without a plan: the other screen is shown on the same monitor when entered.
        v.move(3000, 0);
        QCOMPARE(v.changed->size(), 1);
        QCOMPARE(shownIn(v.changed->first()).size(), 1);
        QCOMPARE(shownIn(v.changed->first()).first().remote, kB2);
        QCOMPARE(shownIn(v.changed->first()).first().monitor, kA2);
    }

    void anOlderSourceShowsOneScreenAtATime()
    {
        Viewing v;
        QVERIFY(v.setup(row("a", 2), row("b", 2), {{"b1", "a1"}}));
        QTRY_VERIFY(peerSupportsMultiView(v.versionOfB()));
        v.claimVersionOfB(QStringLiteral("0.1.19"));
        QTRY_COMPARE(v.versionOfB(), QString("0.1.19"));

        // The plan still says where: the active screen alone, on its monitor.
        v.cross();
        QVERIFY(v.ia->isViewing());
        QCOMPARE(shownIn(v.requested->first()).size(), 1);
        QCOMPARE(shownIn(v.requested->first()).first().remote, kB1);
        QCOMPARE(shownIn(v.requested->first()).first().monitor, kA1);
        QCOMPARE(lastEvent(*v.toA, "viewOpen").value("screens").toArray().size(), 1);
        v.move(3000, 0);
        QCOMPARE(v.changed->size(), 1);
        QCOMPARE(shownIn(v.changed->first()).size(), 1);
        QCOMPARE(shownIn(v.changed->first()).first().index, 1);
        QCOMPARE(shownIn(v.changed->first()).first().remote, kB2);
        QCOMPARE(shownIn(v.changed->first()).first().monitor, kA2);
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("screens").toArray().size(), 1);
        QCOMPARE(lastEvent(*v.toA, "viewShow").value("active").toInteger(), 1);
    }
};

QTEST_GUILESS_MAIN(TestView)
#include "tst_view.moc"
