#include "remoteview.h"

#include "clock.h"
#include "node.h"
#include "securestream.h"
#include "workspace.h"

#include <QCborArray>
#include <QCborValue>
#include <QLoggingCategory>
#include <QtEndian>

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

QByteArray control(const QCborMap &msg) { return char(kControl) + QCborValue(msg).toCbor(); }

QCborMap rectMap(const QRect &r)
{
    QCborMap m;
    m.insert(QStringLiteral("x"), r.x());
    m.insert(QStringLiteral("y"), r.y());
    m.insert(QStringLiteral("w"), r.width());
    m.insert(QStringLiteral("h"), r.height());
    return m;
}

QCborMap typed(const QString &t)
{
    QCborMap m;
    m.insert(kT, t);
    return m;
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

// --- Viewer side ---------------------------------------------------------------------------------

void RemoteView::setViewTimeouts(int answerMs, int firstFrameMs)
{
    m_answerTimeoutMs = answerMs;
    m_firstFrameTimeoutMs = firstFrameMs;
}

void RemoteView::startViewing(const QByteArray &peerId, QSize maxSize, const QRect &screen)
{
    stopViewing();
    m_viewPeer = peerId;
    m_requestedSize = maxSize;
    m_viewScreen = screen;
    m_expectedTiles = -1;
    const QString name = peerName(peerId);
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
    connect(stream, &SecureStream::established, this, [this, stream, maxSize, name] {
        if (m_viewStream != stream)
            return;
        qCInfo(lcView) << "Screen view stream to" << m_viewPeer.toHex() << "established";
        QCborMap view = typed(QStringLiteral("view"));
        view.insert(QStringLiteral("w"), maxSize.width());
        view.insert(QStringLiteral("h"), maxSize.height());
        if (!m_viewScreen.isEmpty()) // possibly switched while connecting
            view.insert(QStringLiteral("screen"), rectMap(m_viewScreen));
        stream->send(control(view));
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

void RemoteView::switchScreen(const QRect &screen)
{
    if (!m_viewStream || screen == m_viewScreen)
        return;
    m_viewScreen = screen;
    if (!m_viewStream->isEstablished())
        return; // the `view` request carries it
    QCborMap msg = typed(QStringLiteral("screen"));
    msg.insert(QStringLiteral("screen"), rectMap(screen));
    m_viewStream->send(control(msg));
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

void RemoteView::frameShown(quint32 seq)
{
    if (!m_viewStream)
        return;
    QCborMap ack = typed(QStringLiteral("ack"));
    ack.insert(kS, qint64(seq));
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
    setViewState(ViewState::Failed, reason);
    emit viewingFailed(reason);
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
        }
    } else if (t == QLatin1String("refused") || t == QLatin1String("error")) {
        failViewing(textFor(msg.value(kC).toString(), msg.value(kR).toString()));
    } else if (t == QLatin1String("frame")) {
        // A frame while Connecting comes from an older source that never sends status: accepted.
        const qint64 w = msg.value(QStringLiteral("w")).toInteger();
        const qint64 h = msg.value(QStringLiteral("h")).toInteger();
        const qint64 n = msg.value(QStringLiteral("n")).toInteger();
        if (w < 1 || w > kMaxFrameSide || h < 1 || h > kMaxFrameSide || n < 0
            || n > qint64(tilesFor(int(w))) * tilesFor(int(h))) {
            failViewing(QStringLiteral("Invalid frame from %1").arg(name));
            return;
        }
        m_incoming = msg;
        m_incoming.remove(kT);
        m_incomingTiles = {};
        m_expectedTiles = int(n);
    } else if (t == QLatin1String("fend")) {
        if (m_expectedTiles < 0)
            return;
        if (m_expectedTiles != m_incomingTiles.size())
            qCWarning(lcView) << "Incomplete frame:" << m_incomingTiles.size() << "of" << m_expectedTiles << "tiles";
        // Shown (and acknowledged) even if incomplete: the next frames repair it, and a missing
        // acknowledgement would stall the stream.
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
        const int w = int(qBound(qint64(1), msg.value(QStringLiteral("w")).toInteger(), qint64(kMaxFrameSide)));
        const int h = int(qBound(qint64(1), msg.value(QStringLiteral("h")).toInteger(), qint64(kMaxFrameSide)));
        const QRect screen = localScreen(msg.value(QStringLiteral("screen")));
        connect(stream, &SecureStream::received, this,
                [this, stream](const QByteArray &f) { onSourceFrame(stream, f); });
        connect(stream, &SecureStream::closed, this, [this, stream] { onSourceClosed(stream); });
        onViewRequest(stream, QSize(w, h), screen);
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

void RemoteView::onViewRequest(SecureStream *stream, QSize size, const QRect &screen)
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
        accept(stream, size, screen);
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
    m_pending->size = size;
    m_pending->screen = screen;
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
    const QSize size = pending->size;
    const QRect screen = pending->screen;
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
        accept(stream, size, screen);
        break;
    case Decision::Deny:
        qCInfo(lcView) << "Screen view denied by the local user";
        m_deniedUntil.insert(peerId, nowMs() + kDenyCooldownMs);
        sendEnd(stream, QStringLiteral("refused"), QStringLiteral("denied"), QStringLiteral("Denied"));
        break;
    }
}

void RemoteView::accept(SecureStream *stream, QSize size, const QRect &screen)
{
    if (SecureStream *old = m_sourceStream; old && old != stream) {
        // One viewer at a time; capture goes on for the new stream, so no captureStopped.
        m_sourceStream = nullptr;
        old->close();
    }
    m_sourceStream = stream;
    m_frameInFlight = false;
    m_ackWatchdog.start(m_ackTimeoutMs); // covers the first frame too
    QCborMap status = typed(QStringLiteral("status"));
    status.insert(kS, QStringLiteral("accepted"));
    stream->send(control(status));
    qCInfo(lcView) << "Streaming this screen to" << stream->peerId().toHex();
    emit captureRequested(size, screen);
    emit streamingChanged(stream->peerId(), true);
}

void RemoteView::onSourceFrame(SecureStream *stream, const QByteArray &frame)
{
    // After the first `view`, only ack, screen and stop are understood; anything else is ignored.
    if (frame.isEmpty() || quint8(frame[0]) != kControl)
        return;
    const QCborMap msg = QCborValue::fromCbor(frame.mid(1)).toMap();
    const QString t = msg.value(kT).toString();
    if (t == QLatin1String("stop")) {
        stream->close();
    } else if (t == QLatin1String("screen")) {
        // Another of the screens the user agreed to show (the whole desktop is shared).
        const QRect screen = localScreen(msg.value(QStringLiteral("screen")));
        if (screen.isEmpty())
            return;
        if (m_pending && m_pending->stream == stream)
            m_pending->screen = screen;
        else if (stream == m_sourceStream)
            emit captureScreenChanged(screen);
    } else if (t == QLatin1String("ack") && stream == m_sourceStream && m_frameInFlight) {
        m_frameInFlight = false;
        m_ackWatchdog.start(m_ackTimeoutMs);
        emit captureNext();
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
    m_frameInFlight = false;
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
    // The master changed or a peer was unpaired: whatever is no longer allowed ends now.
    const QByteArray master = m_workspace->masterId();
    if (m_pending) {
        const QByteArray peer = m_pending->peer;
        if (peer != master)
            dropPending(QStringLiteral("notMaster"), QStringLiteral("No longer the master"));
        else if (!m_node->isTrusted(peer))
            dropPending(QStringLiteral("revoked"), QStringLiteral("Unpaired"));
    }
    if (m_sourceStream) {
        const QByteArray peer = m_sourceStream->peerId();
        if (peer != master)
            endStream(QStringLiteral("notMaster"), QStringLiteral("No longer the master"));
        else if (!m_node->isTrusted(peer))
            endStream(QStringLiteral("revoked"), QStringLiteral("Unpaired"));
    }
}

void RemoteView::frameCaptured(const QCborMap &frame)
{
    if (!m_sourceStream)
        return;
    const QCborArray tiles = frame.value(QStringLiteral("tiles")).toArray();
    QCborMap header = typed(QStringLiteral("frame"));
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
    m_frameInFlight = true;
    m_ackWatchdog.start(m_ackTimeoutMs);
}

} // namespace df
