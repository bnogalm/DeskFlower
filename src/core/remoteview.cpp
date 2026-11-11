#include "remoteview.h"

#include "clock.h"
#include "node.h"
#include "securestream.h"
#include "workspace.h"

#include <QCborArray>
#include <QCborValue>
#include <QLoggingCategory>
#include <QtEndian>

#include <utility>

Q_LOGGING_CATEGORY(lcView, "deskflower.view")

namespace df {

namespace {
constexpr quint8 kControl = 1;
constexpr quint8 kTile = 3;
constexpr int kTileSide = 128; // view::FrameEncoder::kTileSize (the core does not link the codec)

const QString kT = QStringLiteral("t");
const QString kC = QStringLiteral("c");
const QString kR = QStringLiteral("r");
const QString kS = QStringLiteral("s");
const QString kI = QStringLiteral("i");
const QString kG = QStringLiteral("g");

QByteArray control(const QCborMap &msg) { return char(kControl) + QCborValue(msg).toCbor(); }

void insertRect(QCborMap *m, const QRect &r)
{
    m->insert(QStringLiteral("x"), r.x());
    m->insert(QStringLiteral("y"), r.y());
    m->insert(QStringLiteral("w"), r.width());
    m->insert(QStringLiteral("h"), r.height());
}

QCborMap rectMap(const QRect &r)
{
    QCborMap m;
    insertRect(&m, r);
    return m;
}

QCborMap typed(const QString &t)
{
    QCborMap m;
    m.insert(kT, t);
    return m;
}

// The largest frame worth sending for a screen shown on `monitor`.
QSize frameLimit(const QRect &monitor)
{
    return QSize(qBound(1, monitor.width(), RemoteView::kMaxFrameSide), qBound(1, monitor.height(), RemoteView::kMaxFrameSide));
}

QCborArray screensArray(const QList<ViewScreen> &screens)
{
    QCborArray out;
    for (const ViewScreen &s : screens) {
        QCborMap m;
        m.insert(kI, s.index);
        insertRect(&m, s.remote);
        const QSize limit = frameLimit(s.monitor);
        m.insert(QStringLiteral("mw"), limit.width());
        m.insert(QStringLiteral("mh"), limit.height());
        out.append(m);
    }
    return out;
}

bool same(const QList<ViewScreen> &a, const QList<ViewScreen> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype n = 0; n < a.size(); ++n) {
        if (a[n].index != b[n].index || a[n].remote != b[n].remote || frameLimit(a[n].monitor) != frameLimit(b[n].monitor))
            return false;
    }
    return true;
}

bool same(const QList<CaptureSpec> &a, const QList<CaptureSpec> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype n = 0; n < a.size(); ++n) {
        if (a[n].index != b[n].index || a[n].screen != b[n].screen || a[n].maxSize != b[n].maxSize)
            return false;
    }
    return true;
}

// refused {c, r} (a request that never started) or error {c, r} (a stream that ends), then close.
void sendEnd(SecureStream *stream, const QString &type, const QString &code, const QString &text)
{
    QCborMap m = typed(type);
    m.insert(kC, code);
    m.insert(kR, text);
    stream->send(control(m));
    stream->close(); // flushes the message first
}

void appendU16(QByteArray &b, int v)
{
    char tmp[2];
    qToLittleEndian<quint16>(quint16(v), tmp);
    b.append(tmp, 2);
}

int readU16(const QByteArray &b, int offset) { return qFromLittleEndian<quint16>(b.constData() + offset); }

int tilesFor(int side) { return (side + kTileSide - 1) / kTileSide; }
} // namespace

RemoteView::RemoteView(Node *node, Workspace *workspace, QObject *parent)
    : QObject(parent), m_node(node), m_workspace(workspace)
{
    connect(m_node, &Node::streamAccepted, this, &RemoteView::onStreamAccepted);
    connect(m_node, &Node::peersChanged, this, &RemoteView::recheck);
    connect(m_workspace, &Workspace::changed, this, &RemoteView::recheck);
    // A viewer that vanished never closes its TCP stream while the source waits for an ack.
    connect(m_node, &Node::peerDisconnected, this, [this](const QByteArray &peer) {
        if (m_pending && m_pending->peer == peer)
            dropPending(QStringLiteral("stopped"), QStringLiteral("Disconnected"));
        if (streamingTo() == peer)
            endStream(QStringLiteral("stopped"), QStringLiteral("Disconnected"));
    });
    m_ackWatchdog.setSingleShot(true);
    connect(&m_ackWatchdog, &QTimer::timeout, this, [this] {
        qCWarning(lcView) << "The viewer stopped acknowledging frames";
        endStream(QStringLiteral("stalled"), QStringLiteral("No acknowledgement from the viewer"));
    });
    m_viewWatchdog.setSingleShot(true);
    connect(&m_viewWatchdog, &QTimer::timeout, this, [this] { failViewing(m_watchdogFailure); });
    m_promptTimer.setSingleShot(true);
    connect(&m_promptTimer, &QTimer::timeout, this, [this] {
        qCInfo(lcView) << "Screen view request not answered in time";
        dropPending(QStringLiteral("timeout"), QStringLiteral("Not answered"));
    });
}

RemoteView::~RemoteView()
{
    stopViewing();
    // Accepted streams belong to the Node: end them without calling back into this object.
    for (SecureStream *stream : {m_pending ? m_pending->stream.data() : nullptr, m_sourceStream.data()}) {
        if (stream) {
            disconnect(stream, nullptr, this, nullptr);
            stream->close();
        }
    }
}

QString RemoteView::peerName(const QByteArray &id) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == id && !p.name.isEmpty())
            return p.name;
    }
    return QStringLiteral("The remote device");
}

bool RemoteView::peerSupportsMultiView(const QByteArray &peerId) const
{
    // Only a version it declared counts (empty until its hello): a source whose version is not
    // known yet gets the single-screen protocol, which every version understands.
    for (const PeerView &p : m_node->peers()) {
        if (p.id == peerId)
            return df::peerSupportsMultiView(p.version);
    }
    return false;
}

// --- Viewer side ---------------------------------------------------------------------------------

void RemoteView::setViewTimeouts(int answerMs, int firstFrameMs)
{
    m_answerTimeoutMs = answerMs;
    m_firstFrameTimeoutMs = firstFrameMs;
}

void RemoteView::startViewing(const QByteArray &peerId, const QList<ViewScreen> &screens)
{
    stopViewing();
    m_viewPeer = peerId;
    m_viewScreens = screens.mid(0, kMaxViewScreens);
    m_sentScreens.clear();
    m_legacySource = !peerSupportsMultiView(peerId);
    m_legacyBound = {};
    m_expectedTiles = -1;
    m_incomingTiles = {};
    m_incomingDiscarded = false;
    const QString name = peerName(peerId);
    if (m_viewScreens.isEmpty()) {
        failViewing(QStringLiteral("No screen of %1 to show").arg(name));
        return;
    }
    setViewState(ViewState::Connecting, QStringLiteral("Connecting to %1…").arg(name));
    SecureStream *stream = m_node->openStream(peerId);
    if (!stream) {
        failViewing(QStringLiteral("%1 is not reachable").arg(name));
        return;
    }
    stream->setParent(this);
    m_viewStream = stream;
    // Covers the connection and handshake too (SecureStream gives up on its own after
    // kHandshakeTimeoutMs, which is not longer).
    armWatchdog(m_answerTimeoutMs, QStringLiteral("%1 did not answer").arg(name));
    connect(stream, &SecureStream::established, this, [this, stream, name] {
        if (m_viewStream != stream)
            return;
        qCInfo(lcView) << "Screen view stream to" << m_viewPeer.toHex() << "established"
                       << (m_legacySource ? "(single-screen protocol)" : "");
        sendScreens(); // the `view` request, with the set as it is now (possibly changed meanwhile)
        armWatchdog(m_answerTimeoutMs, QStringLiteral("%1 did not answer").arg(name));
    });
    connect(stream, &SecureStream::received, this, [this, stream](const QByteArray &frame) {
        if (m_viewStream == stream)
            onViewerFrame(frame);
    });
    connect(stream, &SecureStream::closed, this, [this, stream, name](const QString &reason) {
        if (m_viewStream == stream)
            failViewing(QStringLiteral("Connection to %1 lost (%2)").arg(name, reason));
        stream->deleteLater();
    });
}

void RemoteView::sendScreens()
{
    const bool first = m_sentScreens.isEmpty();
    QCborMap msg;
    if (m_legacySource) {
        // One screen at a time: the first of the set (a single one for such a source).
        const ViewScreen &s = m_viewScreens.first();
        if (first) {
            msg = typed(QStringLiteral("view"));
            const QSize limit = frameLimit(s.monitor);
            m_legacyBound = limit;
            msg.insert(QStringLiteral("w"), limit.width());
            msg.insert(QStringLiteral("h"), limit.height());
            if (!s.remote.isEmpty())
                msg.insert(QStringLiteral("screen"), rectMap(s.remote));
        } else {
            msg = typed(QStringLiteral("screen"));
            msg.insert(QStringLiteral("screen"), rectMap(s.remote));
        }
    } else {
        msg = typed(first ? QStringLiteral("view") : QStringLiteral("screens"));
        if (first)
            msg.insert(QStringLiteral("v"), 2);
        msg.insert(QStringLiteral("screens"), screensArray(m_viewScreens));
    }
    m_sentScreens = m_viewScreens;
    m_viewStream->send(control(msg));
}

void RemoteView::setScreens(const QList<ViewScreen> &screens)
{
    if (!m_viewStream)
        return;
    const QList<ViewScreen> next = screens.mid(0, kMaxViewScreens);
    if (next.isEmpty() || same(next, m_viewScreens))
        return;
    m_viewScreens = next;
    if (!m_viewStream->isEstablished() || m_sentScreens.isEmpty())
        return; // the `view` request carries it
    if (m_legacySource) {
        if (m_sentScreens.first().remote == next.first().remote)
            m_sentScreens = next; // only the frame size changed, which that protocol cannot say
        else
            sendScreens();
        return;
    }
    // A source ignores `screens` until it accepted the view: sent then (status accepted).
    if (m_viewState == ViewState::Starting || m_viewState == ViewState::Streaming)
        sendScreens();
}

void RemoteView::stopViewing()
{
    m_viewWatchdog.stop();
    if (!m_viewStream)
        return;
    SecureStream *stream = m_viewStream;
    m_viewStream = nullptr;
    stream->send(control(typed(QStringLiteral("stop"))));
    stream->close();
}

void RemoteView::frameShown(quint32 seq, int index)
{
    if (!m_viewStream)
        return;
    QCborMap ack = typed(QStringLiteral("ack"));
    ack.insert(kS, qint64(seq));
    if (!m_legacySource)
        ack.insert(kI, index);
    m_viewStream->send(control(ack));
    // From here the source owes the next frame: the stall timeout measures the source only.
    armWatchdog(m_firstFrameTimeoutMs, QStringLiteral("No image from %1 (screen capture failed)").arg(peerName(m_viewPeer)));
}

void RemoteView::setViewState(ViewState state, const QString &text)
{
    m_viewState = state;
    emit viewStateChanged(m_viewPeer, state, text);
}

void RemoteView::armWatchdog(int ms, const QString &failure)
{
    m_watchdogFailure = failure;
    m_viewWatchdog.start(ms);
}

void RemoteView::failViewing(const QString &reason)
{
    qCWarning(lcView) << "Screen view failed:" << reason;
    m_viewWatchdog.stop();
    if (SecureStream *stream = m_viewStream) {
        m_viewStream = nullptr;
        stream->send(control(typed(QStringLiteral("stop"))));
        stream->close();
    }
    m_expectedTiles = -1;
    m_incomingTiles = {};
    m_incomingDiscarded = false;
    setViewState(ViewState::Failed, reason);
    emit viewingFailed(reason);
}

QSize RemoteView::frameBound(int index) const
{
    // A legacy source only knows the limit of the `view` request; a changed set has not reached a
    // v2 source until it is sent (m_sentScreens), so that is what its frames are measured against.
    if (m_legacySource)
        return m_legacyBound;
    for (const ViewScreen &s : m_sentScreens) {
        if (s.index == index)
            return frameLimit(s.monitor);
    }
    return {};
}

QString RemoteView::textFor(const QString &code, const QString &hint) const
{
    const QString name = peerName(m_viewPeer);
    if (code == QLatin1String("notMaster"))
        return QStringLiteral("%1 refused: this device is not sharing its keyboard and mouse").arg(name);
    if (code == QLatin1String("noUser"))
        return QStringLiteral("Nobody on %1 can approve the request").arg(name);
    if (code == QLatin1String("denied"))
        return QStringLiteral("The request was denied on %1").arg(name);
    if (code == QLatin1String("timeout"))
        return QStringLiteral("Nobody answered the request on %1 in time").arg(name);
    if (code == QLatin1String("busy"))
        return QStringLiteral("%1 is busy with another screen view request").arg(name);
    if (code == QLatin1String("revoked"))
        return QStringLiteral("Permission to view %1 was revoked").arg(name);
    if (code == QLatin1String("disabled"))
        return QStringLiteral("%1 does not share its screen (remote view is off in its advanced settings)").arg(name);
    if (code == QLatin1String("guest"))
        return QStringLiteral("%1 does not show its screen to guests").arg(name);
    if (code == QLatin1String("bad"))
        return QStringLiteral("Bad request: %1 does not have the screens asked for").arg(name);
    if (code == QLatin1String("stopped"))
        return QStringLiteral("%1 stopped showing its screen").arg(name);
    if (code == QLatin1String("capture"))
        return QStringLiteral("No image from %1 (screen capture failed)").arg(name);
    const QString text = hint.left(200);
    return text.isEmpty() ? QStringLiteral("%1 refused the screen view").arg(name)
                          : QStringLiteral("%1: %2").arg(name, text);
}

void RemoteView::onViewerFrame(const QByteArray &frame)
{
    if (frame.isEmpty())
        return;
    if (quint8(frame[0]) == kTile) {
        if (m_expectedTiles < 0 || frame.size() < 9 || m_incomingTiles.size() >= m_expectedTiles)
            return;
        m_incomingBytes += frame.size();
        if (m_incomingBytes > kMaxFrameBytes) {
            failViewing(QStringLiteral("Invalid frame from %1").arg(peerName(m_viewPeer)));
            return;
        }
        QCborMap tile;
        tile.insert(QStringLiteral("x"), readU16(frame, 1));
        tile.insert(QStringLiteral("y"), readU16(frame, 3));
        tile.insert(QStringLiteral("w"), readU16(frame, 5));
        tile.insert(QStringLiteral("h"), readU16(frame, 7));
        tile.insert(QStringLiteral("d"), frame.mid(9));
        m_incomingTiles.append(tile);
        return;
    }
    if (quint8(frame[0]) != kControl)
        return;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(kT).toString();
    const QString name = peerName(m_viewPeer);
    if (t == QLatin1String("status")) {
        const QString s = msg.value(kS).toString();
        if (s == QLatin1String("pending")) {
            const qint64 to = qBound(qint64(0), msg.value(QStringLiteral("to")).toInteger(), qint64(kMaxApprovalWaitMs));
            setViewState(ViewState::AwaitingApproval, QStringLiteral("Waiting for approval on %1…").arg(name));
            armWatchdog(int(qMin<qint64>(to + kApprovalGraceMs, kMaxApprovalWaitMs)),
                        QStringLiteral("Not approved on %1").arg(name));
        } else if (s == QLatin1String("accepted")) {
            setViewState(ViewState::Starting, QStringLiteral("Starting the screen of %1…").arg(name));
            armWatchdog(m_firstFrameTimeoutMs, QStringLiteral("No image from %1 (screen capture failed)").arg(name));
            if (!m_legacySource && !same(m_sentScreens, m_viewScreens))
                sendScreens(); // changed while the source's user was deciding
        }
    } else if (t == QLatin1String("refused") || t == QLatin1String("error")) {
        failViewing(textFor(msg.value(kC).toString(), msg.value(kR).toString()));
    } else if (t == QLatin1String("frame")) {
        // A frame while Connecting comes from an older source that never sends status: accepted.
        const qint64 w = msg.value(QStringLiteral("w")).toInteger();
        const qint64 h = msg.value(QStringLiteral("h")).toInteger();
        const qint64 n = msg.value(QStringLiteral("n")).toInteger();
        // A legacy source tags nothing: its frames are of the one screen asked for.
        const qint64 i = m_legacySource ? m_viewScreens.first().index : msg.value(kI).toInteger(-1);
        if (w < 1 || w > kMaxFrameSide || h < 1 || h > kMaxFrameSide || n < 0
            || n > qint64(tilesFor(int(w))) * tilesFor(int(h)) || i < 0 || i > kMaxScreenIndex) {
            failViewing(QStringLiteral("Invalid frame from %1").arg(name));
            return;
        }
        m_incomingTiles = {};
        m_incomingBytes = 0;
        // Of a screen no longer asked for, or larger than the limit given for it: a late frame of
        // the set before (the source starts that screen afresh and expects no acknowledgement of
        // it), or a source not keeping to the limit. Either way it is read through and dropped.
        const QSize bound = frameBound(int(i));
        m_incomingDiscarded = !bound.isValid() || w > bound.width() || h > bound.height();
        if (m_incomingDiscarded) {
            qCInfo(lcView) << "Dropped a frame of screen" << i << "of" << w << "x" << h << "(limit" << bound << ")";
            m_expectedTiles = -1;
            return;
        }
        m_incoming = msg;
        m_incoming.remove(kT);
        m_incoming.insert(kI, i);
        m_expectedTiles = int(n);
    } else if (t == QLatin1String("fend")) {
        if (std::exchange(m_incomingDiscarded, false) || m_expectedTiles < 0)
            return;
        if (m_expectedTiles != m_incomingTiles.size())
            qCWarning(lcView) << "Incomplete frame:" << m_incomingTiles.size() << "of" << m_expectedTiles << "tiles";
        // Shown (and acknowledged) even if incomplete: the next frames repair it, and a missing
        // acknowledgement would stall the stream. A frame of a screen the source was told of but
        // that is no longer displayed goes to the helper too, which acknowledges it without a window.
        m_incoming.insert(QStringLiteral("tiles"), m_incomingTiles);
        m_expectedTiles = -1;
        m_incomingTiles = {};
        if (m_viewState != ViewState::Streaming)
            setViewState(ViewState::Streaming, QStringLiteral("Showing the screen of %1").arg(name));
        armWatchdog(m_firstFrameTimeoutMs, QStringLiteral("No image from %1 (screen capture failed)").arg(name));
        emit frameReceived(m_incoming);
    }
}

// --- Source side ---------------------------------------------------------------------------------

void RemoteView::onStreamAccepted(SecureStream *stream)
{
    // Only the first frame says whether this stream is a remote view request.
    auto first = std::make_shared<QMetaObject::Connection>();
    *first = connect(stream, &SecureStream::received, this, [this, stream, first](const QByteArray &frame) {
        disconnect(*first);
        if (frame.isEmpty() || quint8(frame[0]) != kControl)
            return;
        const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
        if (msg.value(kT).toString() != QLatin1String("view"))
            return;
        connect(stream, &SecureStream::received, this,
                [this, stream](const QByteArray &f) { onSourceFrame(stream, f); });
        connect(stream, &SecureStream::closed, this, [this, stream] { onSourceClosed(stream); });
        onViewRequest(stream, requestedScreens(msg));
    });
}

QRect RemoteView::localScreen(const QCborValue &value) const
{
    const QCborMap m = value.toMap();
    const QRect r(int(m.value(QStringLiteral("x")).toInteger()), int(m.value(QStringLiteral("y")).toInteger()),
                  int(m.value(QStringLiteral("w")).toInteger()), int(m.value(QStringLiteral("h")).toInteger()));
    for (const ScreenInfo &s : m_workspace->screens(m_node->id())) {
        if (!r.isEmpty() && s.geometry == r)
            return r;
    }
    return {};
}

QList<CaptureSpec> RemoteView::requestedScreens(const QCborMap &msg) const
{
    QList<CaptureSpec> out;
    if (!msg.contains(QStringLiteral("screens"))) {
        // Legacy: one screen (absent or unknown: the primary one; an empty rect, when no screen is
        // known here yet, makes the helper capture its primary one), frames no larger than w x h.
        const int w = int(qBound(qint64(1), msg.value(QStringLiteral("w")).toInteger(), qint64(kMaxFrameSide)));
        const int h = int(qBound(qint64(1), msg.value(QStringLiteral("h")).toInteger(), qint64(kMaxFrameSide)));
        QRect screen = localScreen(msg.value(QStringLiteral("screen")));
        if (screen.isEmpty()) {
            for (const ScreenInfo &s : m_workspace->screens(m_node->id())) {
                if (!s.geometry.isEmpty() && (screen.isEmpty() || s.primary))
                    screen = s.geometry;
            }
        }
        out.append(CaptureSpec{0, screen, QSize(w, h)});
        return out;
    }
    const QCborArray list = msg.value(QStringLiteral("screens")).toArray();
    if (list.isEmpty() || list.size() > kMaxViewScreens)
        return {};
    for (const QCborValue &v : list) {
        const QCborMap m = v.toMap();
        const qint64 i = m.value(kI).toInteger(-1);
        const QRect screen = localScreen(v);
        const qint64 mw = m.value(QStringLiteral("mw")).toInteger();
        const qint64 mh = m.value(QStringLiteral("mh")).toInteger();
        bool repeated = false;
        for (const CaptureSpec &s : out)
            repeated |= s.index == i || s.screen == screen;
        if (i < 0 || i > kMaxScreenIndex || screen.isEmpty() || repeated || mw < 1 || mw > kMaxFrameSide || mh < 1
            || mh > kMaxFrameSide)
            return {};
        out.append(CaptureSpec{int(i), screen, QSize(int(mw), int(mh))});
    }
    return out;
}

void RemoteView::onViewRequest(SecureStream *stream, const QList<CaptureSpec> &screens)
{
    const QByteArray peer = stream->peerId();
    const QString name = peerName(peer);
    if (peer != m_workspace->masterId()) {
        qCWarning(lcView) << "Screen view refused: the requester is not the master";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("notMaster"),
                QStringLiteral("%1 is not the master").arg(name));
        return;
    }
    if (!m_node->isTrusted(peer)) {
        // The stream may predate an unpair (accepted streams outlive it): never prompt for it.
        qCWarning(lcView) << "Screen view refused: the requester is no longer paired";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("revoked"), QStringLiteral("Not paired"));
        return;
    }
    if (m_node->isGuest(peer)) {
        qCWarning(lcView) << "Screen view refused: the requester is a guest";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("guest"), QStringLiteral("Guests cannot view this screen"));
        return;
    }
    if (screens.isEmpty()) {
        qCWarning(lcView) << "Screen view refused: the request names no screen of this device";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("bad"), QStringLiteral("Bad request"));
        return;
    }
    if (!m_node->isRemoteViewEnabled()) {
        qCInfo(lcView) << "Screen view refused: remote view is off on this device";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("disabled"), QStringLiteral("Remote view is off"));
        return;
    }
    if (!m_userPresent) {
        qCWarning(lcView) << "Screen view refused: nobody is signed in to approve it";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("noUser"), QStringLiteral("Nobody can approve it"));
        return;
    }
    if (nowMs() < m_deniedUntil.value(peer, -1)) {
        qCInfo(lcView) << "Screen view refused: denied moments ago";
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("denied"), QStringLiteral("Denied"));
        return;
    }
    if (m_node->isViewAllowed(peer)) {
        accept(stream, screens);
        return;
    }
    if (m_pending) {
        if (m_pending->peer != peer) {
            sendEnd(stream, QStringLiteral("refused"), QStringLiteral("busy"), QStringLiteral("Busy"));
            return;
        }
        // The same viewer asked again: the new request replaces the old one (same prompt).
        const auto old = takePending();
        if (old->stream)
            sendEnd(old->stream, QStringLiteral("refused"), QStringLiteral("busy"), QStringLiteral("Replaced"));
    }

    m_pending = std::make_unique<Pending>();
    m_pending->peer = peer;
    m_pending->stream = stream;
    m_pending->screens = screens;
    m_promptTimer.start(m_promptTimeoutMs);
    QCborMap status = typed(QStringLiteral("status"));
    status.insert(kS, QStringLiteral("pending"));
    status.insert(QStringLiteral("to"), m_promptTimeoutMs);
    stream->send(control(status));
    qCInfo(lcView) << "Asking the local user to show this screen to" << peer.toHex();
    emit authorizationRequested(peer, name, m_promptTimeoutMs);
}

void RemoteView::answer(const QByteArray &peerId, Decision decision)
{
    if (!m_pending || m_pending->peer != peerId)
        return;
    const auto pending = takePending();
    const QPointer<SecureStream> stream = pending->stream;
    const QList<CaptureSpec> screens = pending->screens;
    emit authorizationClosed(peerId);
    if (!stream)
        return;
    if (peerId != m_workspace->masterId()) {
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("notMaster"), QStringLiteral("Not the master"));
        return;
    }
    if (!m_node->isTrusted(peerId)) {
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("revoked"), QStringLiteral("Not paired"));
        return;
    }
    switch (decision) {
    case Decision::Always:
        m_node->setViewAllowed(peerId, true);
        [[fallthrough]];
    case Decision::Once:
        qCInfo(lcView) << "Screen view approved by the local user";
        accept(stream, screens);
        break;
    case Decision::Deny:
        qCInfo(lcView) << "Screen view denied by the local user";
        m_deniedUntil.insert(peerId, nowMs() + kDenyCooldownMs);
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("denied"), QStringLiteral("Denied"));
        break;
    }
}

void RemoteView::accept(SecureStream *stream, const QList<CaptureSpec> &screens)
{
    if (SecureStream *old = m_sourceStream; old && old != stream) {
        // One viewer at a time; capture goes on for the new stream, so no captureStopped: every
        // screen starts afresh below, as a stream the old viewer's frames never reached needs.
        m_sourceStream = nullptr;
        old->close();
    }
    m_sourceStream = stream;
    m_captured = screens;
    for (CaptureSpec &s : m_captured)
        restart(&s);
    m_inFlight.clear();
    armAckWatchdog(); // covers the first frame too
    QCborMap status = typed(QStringLiteral("status"));
    status.insert(kS, QStringLiteral("accepted"));
    stream->send(control(status));
    qCInfo(lcView) << "Streaming" << screens.size() << "screen(s) to" << stream->peerId().toHex();
    emit captureRequested(m_captured);
    emit streamingChanged(stream->peerId(), true);
}

void RemoteView::restart(CaptureSpec *spec)
{
    spec->generation = ++m_generation;
    m_inFlight.remove(spec->index);
}

void RemoteView::armAckWatchdog()
{
    qint64 oldest = nowMs();
    for (const InFlight &f : std::as_const(m_inFlight))
        oldest = qMin(oldest, f.sentMs);
    m_ackWatchdog.start(int(qMax<qint64>(1, m_ackTimeoutMs - (nowMs() - oldest))));
}

void RemoteView::onSourceFrame(SecureStream *stream, const QByteArray &frame)
{
    // After the first `view`, only ack, screen(s) and stop are understood; anything else is ignored.
    if (frame.isEmpty() || quint8(frame[0]) != kControl)
        return;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(kT).toString();
    if (t == QLatin1String("stop")) {
        stream->close();
    } else if (t == QLatin1String("screen")) {
        // Legacy viewer: another of the screens the user agreed to show (the whole desktop is
        // shared), still as index 0 and with the frame size of the request.
        const QRect screen = localScreen(msg.value(QStringLiteral("screen")));
        if (screen.isEmpty())
            return;
        if (m_pending && m_pending->stream == stream) {
            m_pending->screens = {CaptureSpec{0, screen, m_pending->screens.first().maxSize}};
        } else if (stream == m_sourceStream && !m_captured.isEmpty() && m_captured.first().screen != screen) {
            m_captured = {CaptureSpec{0, screen, m_captured.first().maxSize}};
            restart(&m_captured.first()); // a full frame of the new screen comes at once
            emit captureRequested(m_captured);
        }
    } else if (t == QLatin1String("screens")) {
        if (stream != m_sourceStream)
            return; // not before the view is accepted
        if (!msg.contains(QStringLiteral("screens")))
            return; // only the shape documented above; the legacy forms are `view` and `screen`
        QList<CaptureSpec> screens = requestedScreens(msg);
        if (screens.isEmpty()) {
            qCWarning(lcView) << "Ignored a screen set that is not this device's";
            return;
        }
        if (same(screens, m_captured))
            return;
        // A screen kept as it is keeps its capture and pacing; a changed one starts afresh (full
        // frame) and the acknowledgement it still owes paces nothing; one no longer shown owes none.
        QHash<int, InFlight> inFlight;
        for (CaptureSpec &s : screens) {
            bool kept = false;
            for (const CaptureSpec &c : std::as_const(m_captured)) {
                if (c.index == s.index && c.screen == s.screen && c.maxSize == s.maxSize) {
                    s.generation = c.generation;
                    kept = true;
                }
            }
            if (!kept)
                restart(&s);
            else if (m_inFlight.contains(s.index))
                inFlight.insert(s.index, m_inFlight.value(s.index));
        }
        m_inFlight = inFlight;
        m_captured = screens;
        armAckWatchdog();
        emit captureRequested(screens);
    } else if (t == QLatin1String("ack") && stream == m_sourceStream) {
        const int i = int(msg.value(kI).toInteger(0)); // absent (legacy viewer): the one screen
        if (!m_inFlight.contains(i) || m_inFlight.value(i).seq != msg.value(kS).toInteger(-1))
            return;
        m_inFlight.remove(i);
        armAckWatchdog();
        emit captureNext(i);
    }
}

void RemoteView::onSourceClosed(SecureStream *stream)
{
    if (m_pending && m_pending->stream == stream)
        emit authorizationClosed(takePending()->peer);
    if (m_sourceStream == stream)
        detachSource();
}

void RemoteView::detachSource()
{
    SecureStream *stream = m_sourceStream;
    m_sourceStream = nullptr;
    m_captured.clear();
    m_inFlight.clear();
    m_ackWatchdog.stop();
    qCInfo(lcView) << "Stopped streaming this screen";
    emit captureStopped();
    emit streamingChanged(stream->peerId(), false);
}

std::unique_ptr<RemoteView::Pending> RemoteView::takePending()
{
    m_promptTimer.stop();
    return std::move(m_pending);
}

void RemoteView::dropPending(const QString &code, const QString &text)
{
    if (!m_pending)
        return;
    const auto pending = takePending();
    emit authorizationClosed(pending->peer);
    if (pending->stream)
        sendEnd(pending->stream, QStringLiteral("refused"), code, text);
}

void RemoteView::endStream(const QString &code, const QString &text)
{
    SecureStream *stream = m_sourceStream;
    if (!stream)
        return;
    // Stops now: closing waits for unacknowledged data, which a dead viewer may hold up for minutes.
    detachSource();
    sendEnd(stream, QStringLiteral("error"), code, text);
}

void RemoteView::setLocalUserPresent(bool present)
{
    m_userPresent = present;
    if (!present) {
        // Consent belongs to the signed-in user: another session must not inherit a live view.
        dropPending(QStringLiteral("noUser"), QStringLiteral("The user left"));
        endStream(QStringLiteral("noUser"), QStringLiteral("The user left"));
    }
}

void RemoteView::stopStreaming() { endStream(QStringLiteral("stopped"), QStringLiteral("Stopped by the user")); }

void RemoteView::revoke(const QByteArray &peerId)
{
    if (streamingTo() == peerId)
        endStream(QStringLiteral("revoked"), QStringLiteral("Permission revoked"));
}

void RemoteView::captureFailed(const QString &reason)
{
    qCWarning(lcView) << "Screen capture failed:" << reason;
    endStream(QStringLiteral("capture"), reason);
}

QByteArray RemoteView::streamingTo() const { return m_sourceStream ? m_sourceStream->peerId() : QByteArray(); }

void RemoteView::recheck()
{
    // The master changed, a peer was unpaired or remote view was turned off: whatever is no longer
    // allowed ends now.
    if (!m_node->isRemoteViewEnabled()) {
        dropPending(QStringLiteral("disabled"), QStringLiteral("Remote view is off"));
        endStream(QStringLiteral("disabled"), QStringLiteral("Remote view is off"));
        return;
    }
    const QByteArray master = m_workspace->masterId();
    if (m_pending) {
        const QByteArray peer = m_pending->peer;
        if (peer != master)
            dropPending(QStringLiteral("notMaster"), QStringLiteral("No longer the master"));
        else if (!m_node->isTrusted(peer))
            dropPending(QStringLiteral("revoked"), QStringLiteral("Unpaired"));
        else if (m_node->isGuest(peer))
            dropPending(QStringLiteral("guest"), QStringLiteral("Now a guest"));
    }
    if (m_sourceStream) {
        const QByteArray peer = m_sourceStream->peerId();
        if (peer != master)
            endStream(QStringLiteral("notMaster"), QStringLiteral("No longer the master"));
        else if (!m_node->isTrusted(peer))
            endStream(QStringLiteral("revoked"), QStringLiteral("Unpaired"));
        else if (m_node->isGuest(peer))
            endStream(QStringLiteral("guest"), QStringLiteral("Now a guest"));
    }
}

void RemoteView::frameCaptured(const QCborMap &frame)
{
    if (!m_sourceStream)
        return;
    const int i = int(frame.value(kI).toInteger(0));
    const qint64 generation = frame.value(kG).toInteger(-1);
    bool captured = false;
    for (const CaptureSpec &s : m_captured)
        captured |= s.index == i && s.generation == generation;
    if (!captured)
        return; // of a screen that just stopped, or of its capture before it started afresh
    if (m_inFlight.contains(i)) {
        qCWarning(lcView) << "Dropped a frame of screen" << i << "sent before the previous one was shown";
        return;
    }
    const QCborArray tiles = frame.value(QStringLiteral("tiles")).toArray();
    QCborMap header = typed(QStringLiteral("frame"));
    header.insert(kI, i);
    header.insert(kS, frame.value(kS));
    header.insert(QStringLiteral("w"), frame.value(QStringLiteral("w")));
    header.insert(QStringLiteral("h"), frame.value(QStringLiteral("h")));
    header.insert(QStringLiteral("full"), frame.value(QStringLiteral("full")));
    header.insert(QStringLiteral("n"), tiles.size());
    m_sourceStream->send(control(header));
    for (const QCborValue &v : tiles) {
        const QCborMap t = v.toMap();
        QByteArray out;
        out.append(char(kTile));
        appendU16(out, int(t.value(QStringLiteral("x")).toInteger()));
        appendU16(out, int(t.value(QStringLiteral("y")).toInteger()));
        appendU16(out, int(t.value(QStringLiteral("w")).toInteger()));
        appendU16(out, int(t.value(QStringLiteral("h")).toInteger()));
        out.append(t.value(QStringLiteral("d")).toByteArray());
        if (!m_sourceStream->send(out))
            qCWarning(lcView) << "Tile too large for a stream frame";
    }
    QCborMap end = typed(QStringLiteral("fend"));
    end.insert(kS, frame.value(kS));
    m_sourceStream->send(control(end));
    m_inFlight.insert(i, InFlight{frame.value(kS).toInteger(), nowMs()});
    armAckWatchdog();
}

} // namespace df
