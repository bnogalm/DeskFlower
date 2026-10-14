#include "framecodec.h"
#include "inputcontroller.h"
#include "remoteview.h"
#include "store.h"
#include "testutil.h"
#include "workspace.h"

#include <QCborArray>
#include <QPainter>
#include <QPen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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

QCborMap toFrame(quint32 seq, QSize size, bool full, const QList<Tile> &tiles)
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
        viewer.startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("not"), qPrintable(lastReason(failed)));
        QCOMPARE(requested.size(), 0);

        p.wa->setSharing(true);
        QTRY_COMPARE(p.wb->masterId(), p.a->id());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy state(&viewer, &RemoteView::viewStateChanged);
        viewer.startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(requested.first().at(0).toSize(), QSize(1920, 1080));
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::Starting));

        QSignalSpy received(&viewer, &RemoteView::frameReceived);
        QSignalSpy next(&source, &RemoteView::captureNext);
        FrameEncoder encoder;
        const QImage screen = desktop(QSize(1280, 720), Qt::darkGreen);
        source.frameCaptured(toFrame(1, screen.size(), true, encoder.encode(screen, true)));
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
        viewer.frameShown(1);
        QTRY_COMPARE(next.size(), 1);
        viewer.frameShown(1);
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
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy switched(p.source.get(), &RemoteView::captureScreenChanged);
        QSignalSpy state(p.viewer.get(), &RemoteView::viewStateChanged);

        // The view starts on the screen the viewer asked for.
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080), second);
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(requested.first().at(1).toRect(), second);
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::Starting));

        // Another screen of the source: same stream, no new request or prompt.
        p.viewer->switchScreen(primary);
        QTRY_COMPARE(switched.size(), 1);
        QCOMPARE(switched.first().at(0).toRect(), primary);
        QCOMPARE(requested.size(), 1);

        // Something that is not one of its screens is ignored.
        p.viewer->switchScreen(QRect(-5000, 0, 800, 600));
        QTest::qWait(200);
        QCOMPARE(switched.size(), 1);

        // Neither is it honoured in a new request: the primary screen is shown.
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080), QRect(1, 2, 3, 4));
        QTRY_COMPARE(requested.size(), 2);
        QVERIFY(requested.last().at(1).toRect().isEmpty());
        p.viewer->stopViewing();
    }

    void asksTheSourceUserFirst()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy state(p.viewer.get(), &RemoteView::viewStateChanged);
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(asked.size(), 1);
        QCOMPARE(asked.first().at(0).toByteArray(), p.a->id());
        QCOMPARE(asked.first().at(2).toInt(), RemoteView::kPromptTimeoutMs);
        QTRY_VERIFY(states(state).contains(RemoteView::ViewState::AwaitingApproval));
        QTest::qWait(200);
        QCOMPARE(requested.size(), 0);
        QCOMPARE(asked.size(), 1);
        QVERIFY(p.viewer->isViewing());
    }

    void deniedRequestIsRefusedWithAReason()
    {
        ViewPair p;
        QVERIFY(p.setup());
        QSignalSpy asked(p.source.get(), &RemoteView::authorizationRequested);
        QSignalSpy closed(p.source.get(), &RemoteView::authorizationClosed);
        QSignalSpy failed(p.viewer.get(), &RemoteView::viewingFailed);
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(asked.size(), 1);
        p.source->answer(p.a->id(), RemoteView::Decision::Deny);
        QCOMPARE(closed.size(), 1);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY2(lastReason(failed).contains("denied"), qPrintable(lastReason(failed)));

        // Asking again right away is refused without bothering the user (cool-down).
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(asked.size(), 1);
        p.source->answer(p.a->id(), RemoteView::Decision::Once);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(streaming.size(), 1);
        QCOMPARE(p.source->streamingTo(), p.a->id());
        QVERIFY(!p.b->isViewAllowed(p.a->id()));

        // The next request asks again.
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(requested.size(), 1);
        p.wa->setSharing(false);
        QTRY_VERIFY(p.wb->masterId().isEmpty());
        QTRY_COMPARE(stopped.size(), 1);
        QTRY_COMPARE(failed.size(), 1);
    }

    void unpairStopsTheStream()
    {
        ViewPair p;
        QVERIFY(p.setup());
        p.b->setViewAllowed(p.a->id(), true);
        QSignalSpy requested(p.source.get(), &RemoteView::captureRequested);
        QSignalSpy stopped(p.source.get(), &RemoteView::captureStopped);
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(requested.size(), 1);
        FrameEncoder encoder;
        const QImage screen = desktop(QSize(640, 360), Qt::darkGreen);
        p.source->frameCaptured(toFrame(1, screen.size(), true, encoder.encode(screen, true)));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
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
        p.viewer->startViewing(p.b->id(), QSize(1920, 1080));
        QTRY_COMPARE(requested.size(), 1);
        p.source->frameCaptured(toFrame(1, QSize(100000, 10), true, {}));
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
        QCOMPARE(viewRequested.first().at(1).toSize(), QSize(1920, 1080));
        const QCborMap open = lastEvent(toA, "viewOpen");
        QCOMPARE(open.value("w").toInteger(), 1920);
        QCOMPARE(open.value("rw").toInteger(), 3840);
        QCOMPARE(ia.cursor(), QPoint(1, 1081)); // edges map to edges (one pixel inside): 540 / 1079 * 2159

        // Nothing reaches B until it accepts the view.
        QTest::qWait(200);
        QVERIFY(lastEvent(toB, "mabs").isEmpty());
        ia.setViewAccepted(b->id());
        QTRY_COMPARE(lastEvent(toB, "mabs").value("y").toInteger(), 1081);

        // Past the right edge of B's primary screen: its second screen (shorter, so the cursor
        // lands on its bottom row) is shown instead.
        QSignalSpy screenChanged(&ia, &InputController::viewScreenChanged);
        ia.handleSessionMessage(cmd("mm", {{"x", 5000}, {"y", 0}}));
        QVERIFY(ia.isViewing());
        QCOMPARE(ia.cursor(), QPoint(5001, 1079));
        QCOMPARE(screenChanged.size(), 1);
        QCOMPARE(screenChanged.first().at(1).toRect(), QRect(3840, 0, 1920, 1080));
        QCOMPARE(lastEvent(toA, "viewScreen").value("rw").toInteger(), 1920);
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
        QCOMPARE(screenChanged.last().at(1).toRect(), QRect(0, 0, 3840, 2160));
        QCOMPARE(lastEvent(toA, "viewScreen").value("rw").toInteger(), 3840);

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
};

QTEST_GUILESS_MAIN(TestView)
#include "tst_view.moc"
