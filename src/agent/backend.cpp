#include "backend.h"

#include "buildinfo.h"
#include "thumbnailprovider.h"

#include <QBuffer>
#include <QCborArray>
#include <QCborValue>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QVariantMap>
#include <QtEndian>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include "pastesession_win.h"
#include "pasterequests_win.h"

#include <windows.h>
#include <winsvc.h>
#elif defined(Q_OS_MACOS)
#include <unistd.h>
#endif

namespace {
// Thumbnails are captured at most 480x300; anything far larger is refused undecoded.
constexpr int kMaxThumbnailSide = 1024;

// A blocking request to the service (no event loop, any thread): clip* commands to its render
// server (see ipc::renderServerName), anything else, or a service without one, to the main server.
bool connectToServiceFor(QLocalSocket &socket, const QString &cmd, const QString &renderServer,
                         const QString &mainServer, int timeoutMs)
{
    // A paste goes to the service's render server, which sends nothing but the answer (a clipboard
    // reader may ask many times in a row); a service without one answers on the main server.
    if (cmd.startsWith(QLatin1String("clip"))) {
        socket.connectToServer(renderServer);
        if (socket.waitForConnected(timeoutMs))
            return true;
        socket.abort();
    }
    socket.connectToServer(mainServer);
    return socket.waitForConnected(timeoutMs);
}

#ifdef Q_OS_WIN
// The names and sizes of the current clipboard's files (clipList: no contents move, no paste
// needed). Blocking, on any thread.
QList<ClipboardBridge::ListedFile> listClipboardFiles(const QString &renderServer, const QString &mainServer,
                                                      const QByteArray &kid, QString *error)
{
    QLocalSocket socket;
    if (!connectToServiceFor(socket, QStringLiteral("clipList"), renderServer, mainServer, 3000)) {
        *error = Backend::tr("The DeskFlower service is not running");
        return {};
    }
    return df::pasterequests::listFiles(socket, kid, error);
}

// A streaming paste's request (on its PasteSession's thread), on a connection of its own (see
// df::pasterequests::runStreamingFetch).
void runStreamingFetch(const QString &renderServer, const QString &mainServer, const QByteArray &kid,
                       qint64 listedAgoMs, PasteSession::Link &link, int idleTimeoutMs, bool fresh = false)
{
    QLocalSocket socket;
    if (!connectToServiceFor(socket, QStringLiteral("clipFetch"), renderServer, mainServer, 3000)) {
        link.finished(ClipboardBridge::PasteSessionBase::State::Failed,
                      Backend::tr("The DeskFlower service is not running"));
        return;
    }
    df::pasterequests::runStreamingFetch(socket, kid, listedAgoMs, link, idleTimeoutMs, fresh);
}
#endif
} // namespace

Backend::Backend(const QString &instance, ThumbnailProvider *thumbnails, QObject *parent)
    : QObject(parent),
      m_instance(instance),
      m_serverName(df::ipc::serverName(instance)),
      m_conn(&m_socket),
      m_thumbnails(thumbnails)
{
    connect(&m_conn, &df::ipc::Connection::messageReceived, this, &Backend::onMessage);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_restartAttempts = 0;
        setServiceStartError({});
        // What this agent understands (an agent left running across an update may not): the
        // service sends a sealed clipboard only to agents that declared a version that renders it.
        QCborMap hello;
        hello.insert(QStringLiteral("v"), df::build::version());
        // Whether files dragged from other devices can be shown in a native drag here.
        hello.insert(QStringLiteral("dnd"), FileDragAgent::supportsNativeDrag());
#if defined(Q_OS_LINUX)
        // The graphical session this agent runs in, for the service's session helper (a systemd
        // user service does not always get it; the service keeps only these variables).
        QCborMap env;
        for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
                                 "XDG_SESSION_DESKTOP", "DESKTOP_SESSION", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"}) {
            const QString value = qEnvironmentVariable(name);
            if (!value.isEmpty())
                env.insert(QLatin1String(name), value);
        }
        hello.insert(QStringLiteral("env"), env);
#endif
        command(QStringLiteral("agent"), hello);
        emit serviceConnectedChanged();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        emit serviceConnectedChanged();
        if (!m_transfers.isEmpty() || !m_ended.isEmpty()) { // the service's transfers ended with it
            m_transfers.clear();
            m_ended.clear();
            m_rates.clear();
            emit transfersChanged();
        }
        clearWaiting(); // so did its clipboard
        m_reconnect.start();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        // restartService: the old process may still be stopping, so keep trying for a while.
        if (m_restartAttempts > 0 && m_socket.error() == QLocalSocket::ServerNotFoundError) {
            --m_restartAttempts;
            startService();
        }
        // Launching the agent starts a service that the user stopped (by quitting) last time.
        if (!m_triedStart && m_socket.error() == QLocalSocket::ServerNotFoundError) {
            m_triedStart = true;
            startService();
        }
        m_reconnect.start();
    });

    m_reconnect.setInterval(1000);
    m_transferBusyTimer.setInterval(1000);
    connect(&m_transferBusyTimer, &QTimer::timeout, this, &Backend::updateTransferBusy);
    connect(this, &Backend::transfersChanged, this, &Backend::updateTransferBusy);
    m_keyDiagTextTimer.setSingleShot(true);
    m_keyDiagTextTimer.setInterval(kKeyDiagTextMs);
    connect(&m_keyDiagTextTimer, &QTimer::timeout, this, &Backend::updateKeyDiagText);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Backend::connectToService);
    connect(&m_fileDrag, &FileDragAgent::notice, this, [this](const QString &text) { emit notice(text, true); });

    connect(&m_clipboard, &ClipboardBridge::changed, this, [this](const QCborMap &formats, const QStringList &files) {
        QCborMap a;
        a.insert(QStringLiteral("f"), formats);
        a.insert(QStringLiteral("files"), QCborArray::fromStringList(files));
        command(QStringLiteral("clip"), a);
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_clipboard.setEnabled(false);
        m_fileDrag.serviceLost(); // a drag shown here could never end otherwise
    });

    // Quitting: requests for pastes give up, and the threads of streaming pastes are stopped and
    // joined while Qt is still whole (a stream Explorer holds may keep a session past the agent).
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [] {
            ClipboardBridge::abandonFetches();
#ifdef Q_OS_WIN
            PasteSession::setStartedHook({});
            PasteSession::stopAll();
#endif
        });
    }
#ifdef Q_OS_WIN
    // A streaming paste's first transfer report comes before its session knows its folder: once
    // the folder is known, the transfers shown here are matched again (Explorer shows that one).
    PasteSession::setStartedHook([guard = QPointer<Backend>(this)] {
        QObject *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [guard] {
                if (guard)
                    guard->recheckShellDriven();
            },
            Qt::QueuedConnection);
    });
#endif

    m_clock.start();
    connectToService();
}

void Backend::replyToService(qint64 requestId, QCborMap reply)
{
    reply.insert(QStringLiteral("rid"), requestId);
    command(QStringLiteral("agentReply"), reply);
}

QStringList Backend::fetchClipboardFiles(const QByteArray &kid, QString *error) const
{
    // The service refuses a kid that is not its current clipboard (a stale object pastes nothing).
    QCborMap args;
    if (!kid.isEmpty())
        args.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
    return fetchFromService(QStringLiteral("clipFetch"), args, QStringLiteral("clipFetched"), error);
}

QByteArray Backend::unsealClipboardFormat(const QByteArray &kid, const QString &mime, QString *error) const
{
    QCborMap args;
    args.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
    args.insert(QStringLiteral("mime"), mime);
    return requestFromService(QStringLiteral("clipUnseal"), args, QStringLiteral("clipUnsealed"),
                              df::ipc::kClipboardRenderTimeoutMs, error);
}

bool Backend::connectForRequest(QLocalSocket &socket, const QString &cmd, int timeoutMs) const
{
    return connectToServiceFor(socket, cmd, df::ipc::renderServerName(m_instance), m_serverName, timeoutMs);
}

ClipboardBridge::FileStreaming Backend::fileStreaming(const QByteArray &kid, bool stream, bool move) const
{
#ifdef Q_OS_WIN
    // Copies of the server names, not this object: a session may outlive it (a stream Explorer
    // still holds while the agent quits).
    const QString renderServer = df::ipc::renderServerName(m_instance);
    const QString mainServer = m_serverName;
    ClipboardBridge::FileStreaming streaming;
    if (move) {
        // A fresh transfer for each paste, shown in DeskFlower's window (Explorer shows nothing
        // while it waits for CF_HDROP); its folder kept while the session lives.
        streaming.move = [renderServer, mainServer, kid](QString *error)
            -> std::shared_ptr<ClipboardBridge::PasteSessionBase> {
            return PasteSession::open(
                [renderServer, mainServer, kid](PasteSession::Link &link) {
                    runStreamingFetch(renderServer, mainServer, kid, -1, link, kPasteIdleTimeoutMs, true);
                },
                error, PasteSession::kAbandonMs, false);
        };
    }
    if (!stream)
        return streaming;
    streaming.list = [renderServer, mainServer, kid](QString *error) {
        return listClipboardFiles(renderServer, mainServer, kid, error);
    };
    streaming.open = [renderServer, mainServer, kid](
                         QString *error, qint64 listedAgoMs) -> std::shared_ptr<ClipboardBridge::PasteSessionBase> {
        return PasteSession::open(
            [renderServer, mainServer, kid, listedAgoMs](PasteSession::Link &link) {
                runStreamingFetch(renderServer, mainServer, kid, listedAgoMs, link, kPasteIdleTimeoutMs);
            },
            error);
    };
    return streaming;
#else
    Q_UNUSED(kid)
    Q_UNUSED(stream)
    Q_UNUSED(move)
    return {}; // the helper's paste transfers the files whole (no file promises here)
#endif
}

QByteArray Backend::requestFromService(const QString &cmd, QCborMap args, const QString &replyEvent, int timeoutMs,
                                       QString *error) const
{
    // Called while an application waits for the pasted content (inside its clipboard request): no
    // event loop, so a separate connection with blocking reads.
    QLocalSocket socket;
    if (!connectForRequest(socket, cmd, 2000)) {
        *error = tr("The DeskFlower service is not running");
        return {};
    }
    QCborMap request = args;
    request.insert(QStringLiteral("cmd"), cmd);
    const QByteArray body = request.toCborValue().toCbor();
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    socket.write(frame + body);
    socket.waitForBytesWritten(2000);

    QByteArray buffer;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (!socket.waitForReadyRead(int(timeoutMs - elapsed.elapsed()) + 1)) {
            if (socket.state() != QLocalSocket::ConnectedState)
                break;
            continue;
        }
        buffer += socket.readAll();
        while (buffer.size() >= 4) {
            const quint32 size = qFromBigEndian<quint32>(buffer.constData());
            if (quint32(buffer.size() - 4) < size)
                break;
            const QCborMap msg = QCborValue::fromCbor(buffer.mid(4, int(size))).toMap();
            buffer.remove(0, int(size) + 4);
            if (msg.value(QStringLiteral("ev")).toString() != replyEvent)
                continue; // state, thumbnails...: what every new connection gets
            if (!msg.value(QStringLiteral("ok")).toBool()) {
                *error = msg.value(QStringLiteral("error")).toString();
                return {};
            }
            return msg.value(QStringLiteral("data")).toByteArray();
        }
    }
    *error = tr("No answer from the DeskFlower service");
    return {};
}

QStringList Backend::fetchFromService(const QString &cmd, QCborMap args, const QString &replyEvent, QString *error) const
{
    // Called while an application waits for the pasted files (inside its clipboard request): no
    // event loop, so a separate connection with blocking reads.
    QLocalSocket socket;
    if (!connectForRequest(socket, cmd, 3000)) {
        *error = tr("The DeskFlower service is not running");
        return {};
    }
    QCborMap request = args;
    request.insert(QStringLiteral("cmd"), cmd);
    const QByteArray body = request.toCborValue().toCbor();
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    socket.write(frame + body);
    socket.waitForBytesWritten(3000);

    QByteArray buffer;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < kPasteIdleTimeoutMs) {
        if (ClipboardBridge::fetchesAbandoned()) { // the agent quits: it does not wait for the transfer
            *error = tr("DeskFlower is closing");
            return {};
        }
        if (!socket.waitForReadyRead(1000)) {
            if (socket.state() != QLocalSocket::ConnectedState)
                break;
            continue;
        }
        buffer += socket.readAll();
        while (buffer.size() >= 4) {
            const quint32 size = qFromBigEndian<quint32>(buffer.constData());
            if (quint32(buffer.size() - 4) < size)
                break;
            const QCborMap msg = QCborValue::fromCbor(buffer.mid(4, int(size))).toMap();
            buffer.remove(0, int(size) + 4);
            const QString ev = msg.value(QStringLiteral("ev")).toString();
            if (ev == QLatin1String("clipFetching")) {
                elapsed.restart(); // still transferring
                continue;
            }
            if (ev != replyEvent)
                continue; // state, thumbnails...: what every new connection gets
            if (!msg.value(QStringLiteral("ok")).toBool()) {
                // Not a paste (a reader looking at the clipboard): refused without telling anyone.
                *error = msg.value(QStringLiteral("code")).toString() == QLatin1String("noPaste")
                             ? QString::fromLatin1(ClipboardBridge::kNotAPaste)
                             : msg.value(QStringLiteral("error")).toString();
                return {};
            }
            QStringList files;
            for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
                files.append(v.toString());
            return files;
        }
    }
    *error = tr("No answer from the DeskFlower service");
    return {};
}

QString Backend::thumbKey(const QString &deviceId, const QString &screenId)
{
    return deviceId + QLatin1Char('-') + QString::fromLatin1(screenId.toUtf8().toHex());
}

QString Backend::masterName() const
{
    for (const DeviceInfo &d : m_devices) {
        if (d.id == m_masterId)
            return d.name;
    }
    return {};
}

DeviceInfo Backend::selfDevice() const
{
    for (const DeviceInfo &d : m_devices) {
        if (d.self)
            return d;
    }
    return {};
}

DeviceInfo Backend::device(const QString &deviceId) const
{
    for (const DeviceInfo &d : m_devices) {
        if (d.id == deviceId)
            return d;
    }
    return {};
}

QList<double> Backend::packEdges() const
{
    QList<double> edges;
    for (const DeviceInfo &d : m_devices) {
        const QRectF b = d.bounds();
        const double left = d.x + b.x();
        const double top = d.y + b.y();
        edges << left << top << left + b.width() << top + b.height();
    }
    return edges;
}

QList<PeerInfo> Backend::protocolAlerts() const
{
    QList<PeerInfo> out;
    for (const PeerInfo &p : m_trusted) {
        if (!p.protocolMismatch.isEmpty())
            out.append(p);
    }
    return out;
}

QString Backend::protocolAlertText(const PeerInfo &peer)
{
    const QString name = peer.name.isEmpty() ? peer.shortId() : peer.name;
    return peer.protocolMismatch == QLatin1String("newer")
               ? tr("%1 uses a newer protocol (version %2). The protocol has changed: update this device.")
                     .arg(name)
                     .arg(peer.protocol)
               : tr("%1 uses an older protocol (version %2). The protocol has changed: update it.")
                     .arg(name)
                     .arg(peer.protocol);
}

QStringList Backend::connectedIds() const
{
    QStringList ids;
    if (!m_selfId.isEmpty())
        ids.append(m_selfId);
    for (const PeerInfo &p : m_trusted) {
        if (p.connected && !p.id.isEmpty())
            ids.append(p.id);
    }
    return ids;
}

void Backend::setRemoteView(const QString &deviceId, bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), deviceId);
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setRemoteView"), a);
}

void Backend::setViewPlan(const QString &deviceId, const QStringList &screenIds, const QStringList &placements)
{
    // Only the screens with a choice, in a map sorted by key (as the plan always went out).
    QVariantMap plan;
    for (qsizetype i = 0; i < screenIds.size() && i < placements.size(); ++i) {
        if (!placements.at(i).isEmpty())
            plan.insert(screenIds.at(i), placements.at(i));
    }
    QCborMap a;
    a.insert(QStringLiteral("id"), deviceId);
    a.insert(QStringLiteral("plan"), QCborMap::fromVariantMap(plan));
    command(QStringLiteral("setViewPlan"), a);
}

void Backend::setSharing(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setSharing"), a);
}

void Backend::setSharedFolder(const QUrl &folder)
{
    QCborMap a;
    a.insert(QStringLiteral("path"), folder.isLocalFile() ? folder.toLocalFile() : folder.toString());
    command(QStringLiteral("setSharedFolder"), a);
}

void Backend::openSharedFolder()
{
    if (!sharedFolder().isEmpty())
        QDesktopServices::openUrl(sharedFolderUrl());
}

void Backend::setPhoneAccess(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setPhoneAccess"), a);
}

QStringList Backend::phoneUrlLabels() const
{
    // Cut at the first "/?t=", where the token starts.
    QStringList labels;
    for (const QString &url : phoneUrls()) {
        const qsizetype token = url.indexOf(QLatin1String("/?t="));
        labels.append(token >= 0 ? url.left(token) : url);
    }
    return labels;
}

void Backend::newPhoneLink() { command(QStringLiteral("newPhoneLink")); }

void Backend::revokePhones() { command(QStringLiteral("revokePhones")); }

void Backend::answerView(const QString &peerId, const QString &decision)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("decision"), decision);
    command(QStringLiteral("viewAnswer"), a);
}

void Backend::stopScreenView() { command(QStringLiteral("stopScreenView")); }

void Backend::setViewAllowed(const QString &peerId, bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setViewAllowed"), a);
}

void Backend::setSwapCtrlCmd(const QString &peerId, bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setSwapCtrlCmd"), a);
}

void Backend::updateFrom(const QString &peerId)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    command(QStringLiteral("update"), a);
}

void Backend::cancelUpdate() { command(QStringLiteral("cancelUpdate")); }

double Backend::updateProgress() const
{
    const double total = m_updates.value(QStringLiteral("total")).toDouble();
    return total > 0 ? m_updates.value(QStringLiteral("received")).toDouble() / total : 0.0;
}

void Backend::cancelTransfer(qint64 id)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), id);
    command(QStringLiteral("cancelTransfer"), a);
}

bool Backend::isShellDriven(const QString &target)
{
#ifdef Q_OS_WIN
    if (target.isEmpty())
        return false;
    const QString wanted = QDir::cleanPath(QDir::fromNativeSeparators(target));
    for (const QString &root : PasteSession::liveRoots()) {
        if (QDir::cleanPath(QDir::fromNativeSeparators(root)).compare(wanted, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
#else
    Q_UNUSED(target)
    return false; // no file promises here: the progress window shows every pasted transfer
#endif
}

bool Backend::isMovePaste(const QString &target)
{
#ifdef Q_OS_WIN
    if (target.isEmpty())
        return false;
    const QString wanted = QDir::cleanPath(QDir::fromNativeSeparators(target));
    for (const QString &root : PasteSession::liveMoveRoots()) {
        if (QDir::cleanPath(QDir::fromNativeSeparators(root)).compare(wanted, Qt::CaseInsensitive) == 0)
            return true;
    }
#else
    Q_UNUSED(target)
#endif
    return false;
}

void Backend::recheckShellDriven()
{
    bool changed = false;
    for (TransferInfo &t : m_transfers) {
        if (!t.shellDriven && t.kind == QLatin1String("clipboard") && isShellDriven(t.target)) {
            t.shellDriven = true;
            changed = true;
        }
    }
    if (changed)
        emit transfersChanged();
}

void Backend::onTransfer(const QCborMap &msg)
{
    TransferInfo t = TransferInfo::fromCbor(msg);
    qsizetype index = -1;
    for (qsizetype i = 0; i < m_transfers.size(); ++i) {
        if (m_transfers.at(i).id == t.id)
            index = i;
    }
    // Once recognised as a paste Explorer shows, it stays so (its session may go first).
    t.shellDriven = (index >= 0 && m_transfers.at(index).shellDriven) || isShellDriven(t.target);
    if (t.state != QLatin1String("running")) {
        // Done, failed or cancelled: the service also sends a notice saying how it went. The
        // progress window shows the outcome a few seconds (with the counts last reported).
        if (index >= 0) {
            const TransferInfo last = m_transfers.takeAt(index);
            t.done = last.done;
            t.total = last.total;
            t.files = last.files;
            t.fileCount = last.fileCount;
        }
        m_rates.remove(t.id);
        t.cancellable = false;
        if (t.kind == QLatin1String("clipboard") && isMovePaste(t.target))
            t.target.clear(); // moved away by the paste: no folder to open

        if (t.kind == QLatin1String("clipboard") && !t.shellDriven) {
            m_ended.append(t);
            QTimer::singleShot(kEndedShownMs, this, [this, id = t.id] {
                for (qsizetype i = 0; i < m_ended.size(); ++i) {
                    if (m_ended.at(i).id == id) {
                        m_ended.removeAt(i);
                        emit transfersChanged();
                        return;
                    }
                }
            });
        }
        if (index >= 0 || !m_ended.isEmpty())
            emit transfersChanged();
        return;
    }
    // The rate: a moving average of what came between two reports (at most every 250 ms), and the
    // time left at that rate.
    RateSample &r = m_rates[t.id];
    const qint64 now = m_clock.elapsed();
    if (r.at < 0) {
        r.at = now;
        r.done = t.done;
    } else if (now - r.at >= 200) {
        const double instant = std::max(0.0, t.done - r.done) * 1000.0 / double(now - r.at);
        r.rate = r.rate > 0 ? 0.7 * r.rate + 0.3 * instant : instant;
        r.at = now;
        r.done = t.done;
    }
    t.rateBytesPerSec = r.rate;
    t.secondsLeft = r.rate > 0 && t.total > 0 ? std::max(0.0, t.total - t.done) / r.rate : -1;
    if (index >= 0)
        m_transfers[index] = t;
    else
        m_transfers.append(t);
    emit transfersChanged();
}

void Backend::updateTransferBusy()
{
    bool busy = !m_transfers.isEmpty();
#ifdef Q_OS_WIN
    busy = busy || PasteSession::activeWithin(kPasteQuietMs);
#endif
    if (busy && !m_transferBusyTimer.isActive())
        m_transferBusyTimer.start();
    else if (!busy)
        m_transferBusyTimer.stop();
    if (busy != m_transferBusy) {
        m_transferBusy = busy;
        emit transferBusyChanged();
    }
}

QList<TransferInfo> Backend::windowTransfers() const
{
    QList<TransferInfo> shown;
    for (const TransferInfo &t : m_transfers) {
        if (t.kind == QLatin1String("clipboard") && !t.shellDriven)
            shown.append(t);
    }
    return shown + m_ended;
}

void Backend::openTransferFolder(qint64 id)
{
    // Only a folder the service named for a pasted transfer of the progress window that is done
    // (never a path from elsewhere, nor files still arriving that a cancel may remove).
    for (const TransferInfo &t : m_ended) {
        if (t.id == id && t.state == QLatin1String("done") && t.kind == QLatin1String("clipboard") && !t.shellDriven
            && !t.target.isEmpty() && QFileInfo(t.target).isDir()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(t.target));
            return;
        }
    }
}

void Backend::onClipWaiting(const QCborMap &msg)
{
    const QByteArray kid = QByteArray::fromHex(msg.value(QStringLiteral("kid")).toString().toLatin1());
    const int count = int(msg.value(QStringLiteral("count")).toInteger());
    if (count <= 0) { // that offer is over (the clipboard changed, or the files are here)
        if (kid == m_waitingKid)
            clearWaiting();
        return;
    }
    m_waitingKid = kid;
    m_waitingCount = count;
    m_waitingFrom = msg.value(QStringLiteral("from")).toString();
    m_waitingError.clear();
    m_receiving = false;
    emit waitingChanged();
}

void Backend::onClipReceived(const QCborMap &msg)
{
    const QByteArray kid = QByteArray::fromHex(msg.value(QStringLiteral("kid")).toString().toLatin1());
    if (kid != m_waitingKid || !m_receiving)
        return;
    m_receiving = false;
    const QString code = msg.value(QStringLiteral("code")).toString();
    if (msg.value(QStringLiteral("ok")).toBool() || code == QLatin1String("changed")) {
        clearWaiting(); // here, or not on the clipboard any more
        return;
    }
    // Cancelled from the progress window: offered again as it was. Otherwise the offer says why.
    if (code != QLatin1String("cancelled"))
        m_waitingError = msg.value(QStringLiteral("error")).toString();
    emit waitingChanged();
}

void Backend::receiveClipboard()
{
    if (m_waitingKid.isEmpty() || m_receiving)
        return;
    QCborMap a;
    a.insert(QStringLiteral("kid"), QString::fromLatin1(m_waitingKid.toHex()));
    command(QStringLiteral("clipReceive"), a);
    m_receiving = true;
    m_waitingError.clear();
    emit waitingChanged();
}

void Backend::dismissWaiting()
{
    if (!m_waitingKid.isEmpty() && !m_receiving) {
        // The service forgets the offer too (an older one ignores this): it is not made again when
        // this agent connects anew.
        QCborMap a;
        a.insert(QStringLiteral("kid"), QString::fromLatin1(m_waitingKid.toHex()));
        command(QStringLiteral("clipWaitingDismiss"), a);
    }
    clearWaiting();
}

void Backend::clearWaiting()
{
    if (m_waitingKid.isEmpty() && m_waitingCount == 0)
        return;
    m_waitingKid.clear();
    m_waitingCount = 0;
    m_waitingFrom.clear();
    m_waitingError.clear();
    m_receiving = false;
    emit waitingChanged();
}

QString Backend::waitingSummary() const
{
    if (m_waitingCount <= 0)
        return {};
    return tr("%n item(s) copied on %1 wait for a paste", nullptr, m_waitingCount).arg(m_waitingFrom);
}

QString Backend::transfersSummary() const
{
    if (m_transfers.isEmpty())
        return {};
    qint64 done = 0;
    qint64 total = 0;
    for (const TransferInfo &t : m_transfers) {
        done += qint64(t.done);
        total += qint64(t.total);
    }
    // "1.2 GB of 15 GB, 13.8 GB left (8%)" once the total is known. Traditional units (1024),
    // as the transfer rows show them.
    QString progress;
    if (total > 0) {
        const QLocale locale;
        const auto size = [&locale](qint64 bytes) {
            return locale.formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
        };
        progress = tr(": %1 of %2, %3 left (%4%)")
                       .arg(size(done), size(total), size(qMax<qint64>(0, total - done)))
                       .arg(done * 100 / total);
    }
    if (m_transfers.size() == 1)
        return tr("Receiving files from %1%2").arg(m_transfers.first().peerName, progress);
    return tr("Receiving files in %n transfer(s)%1", nullptr, int(m_transfers.size())).arg(progress);
}

QString Backend::guestsSummary() const
{
    QStringList guests, guestOf;
    for (const PeerInfo &p : m_trusted) {
        if (!p.connected)
            continue;
        if (p.trust == QLatin1String("guest"))
            guests.append(p.name);
        else if (p.declaredTrust == QLatin1String("guest"))
            guestOf.append(p.name);
    }
    QStringList lines;
    if (!guests.isEmpty())
        lines.append(tr("Guests here: %1").arg(guests.join(QStringLiteral(", "))));
    if (!guestOf.isEmpty())
        lines.append(tr("A guest on: %1").arg(guestOf.join(QStringLiteral(", "))));
    return lines.join(QLatin1Char('\n'));
}

void Backend::setAutostart(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setAgentAutostart"), a);
}

void Backend::setRemoteViewEnabled(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setRemoteViewEnabled"), a);
}

void Backend::setKeyDiagnostics(bool on)
{
    if (!keyDiagnosticsAvailable())
        return;
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("keyDiag"), a);
}

void Backend::updateKeyDiagText()
{
    QStringList all = m_keyDiagHeader;
    if (m_keyDiagDropped)
        all << tr("... %1 older lines not shown (in the log) ...").arg(m_keyDiagDropped);
    all << m_keyDiagLines;
    m_keyDiagText = all.join(QLatin1Char('\n'));
    emit keyDiagTextChanged();
}

void Backend::setOtherSubnets(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setOtherSubnets"), a);
}

bool Backend::pasteMoveSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void Backend::setPasteMove(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setPasteMove"), a);
}

void Backend::setClipboardSharing(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setClipboardSharing"), a);
}

void Backend::moveDevice(const QString &deviceId, double x, double y)
{
    // Math.round: a half goes up (towards +infinity), unlike qRound.
    const auto jsRound = [](double v) {
        const double down = std::floor(v);
        return qint64(v - down >= 0.5 ? down + 1 : down);
    };
    QCborArray list;
    for (const DeviceInfo &d : m_devices) {
        const bool moved = d.id == deviceId;
        QCborMap m;
        m.insert(QStringLiteral("id"), d.id);
        m.insert(QStringLiteral("x"), moved ? jsRound(x) : qint64(d.x));
        m.insert(QStringLiteral("y"), moved ? jsRound(y) : qint64(d.y));
        list.append(m);
    }
    QCborMap a;
    a.insert(QStringLiteral("packs"), list);
    command(QStringLiteral("setLayout"), a);
}

void Backend::refreshThumbnails() { command(QStringLiteral("requestThumbs")); }

void Backend::stopServiceAndQuit()
{
    if (serviceConnected()) {
        command(QStringLiteral("stopService"));
        m_socket.flush();
        m_socket.waitForBytesWritten(1000);
    }
    QCoreApplication::quit();
}

void Backend::restartService()
{
    if (!serviceConnected())
        return startService();
    m_restartAttempts = 15;
    command(QStringLiteral("stopService"));
}

void Backend::setPorts(int data, int discovery, int phone)
{
    QCborMap a;
    a.insert(QStringLiteral("data"), data);
    a.insert(QStringLiteral("discovery"), discovery);
    a.insert(QStringLiteral("phone"), phone);
    command(QStringLiteral("setPorts"), a);
}

void Backend::setServiceStartError(const QString &error)
{
    if (m_serviceStartError == error)
        return;
    m_serviceStartError = error;
    emit serviceStartErrorChanged();
}

void Backend::startService()
{
    setServiceStartError({});
    if (!m_instance.isEmpty()) {
        setServiceStartError(tr("Start the service of instance \"%1\" by hand").arg(m_instance));
        return;
    }
#ifdef Q_OS_WIN
    // Signed-in users may start (not stop) the service: see platform::install.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = scm ? OpenServiceW(scm, L"Deskflower", SERVICE_START | SERVICE_QUERY_STATUS) : nullptr;
    const bool ok = service && (StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);
    const DWORD error = GetLastError();
    if (service)
        CloseServiceHandle(service);
    if (scm)
        CloseServiceHandle(scm);
    if (!ok)
        setServiceStartError(error == ERROR_ACCESS_DENIED
                                 ? tr("Windows did not let this user start the service (reinstalling DeskFlower "
                                      "fixes it, or start \"DeskFlower\" in Services)")
                                 : tr("The service could not be started (error %1)").arg(error));
#elif defined(Q_OS_MACOS)
    // The service is in this bundle (Contents/MacOS, next to the agent). Its LaunchAgent is written
    // here the first time, and again when the bundle has moved: a DMG has no installer.
    const QString service = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("deskflower-service"));
    const QString plist = QDir::home().filePath(QStringLiteral("Library/LaunchAgents/com.deskflower.service.plist"));
    QFile current(plist);
    const bool installed = current.open(QIODevice::ReadOnly)
                           && QString::fromUtf8(current.readAll()).contains(service.toHtmlEscaped());
    const auto kickstart = [] {
        QProcess::startDetached(QStringLiteral("launchctl"),
                                {QStringLiteral("kickstart"),
                                 QStringLiteral("gui/%1/com.deskflower.service").arg(::getuid())});
    };
    if (!installed && QFileInfo(service).isExecutable()) {
        // Asynchronously (launchctl may take a while): the window stays responsive meanwhile.
        auto *install = new QProcess(this);
        connect(install, &QProcess::finished, this, [this, install, kickstart](int exitCode, QProcess::ExitStatus status) {
            install->deleteLater();
            if (status != QProcess::NormalExit || exitCode != 0)
                setServiceStartError(tr("The service could not be registered (%1)")
                                         .arg(QString::fromLocal8Bit(install->readAllStandardError()).trimmed()));
            kickstart();
        });
        connect(install, &QProcess::errorOccurred, this, [this, install](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return; // the others end in finished
            install->deleteLater();
            setServiceStartError(tr("The service could not be registered (%1)").arg(install->errorString()));
        });
        install->start(service, {QStringLiteral("--install")}); // writes the LaunchAgent and loads it
    } else {
        kickstart();
    }
#else
    // The user service may start before (or outside) a graphical session that tells systemd about
    // its display: hand it this session's variables first (both the systemd and the D-Bus
    // activation environment), then start it. Harmless where the desktop already did it.
    importSessionEnvironment();
    QProcess::startDetached(QStringLiteral("systemctl"),
                            {QStringLiteral("--user"), QStringLiteral("start"), QStringLiteral("deskflower.service")});
#endif
    m_reconnect.start(); // connects as soon as it listens
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
void Backend::importSessionEnvironment()
{
    QStringList names;
    for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
                             "XDG_SESSION_DESKTOP", "DESKTOP_SESSION", "DBUS_SESSION_BUS_ADDRESS"}) {
        if (qEnvironmentVariableIsSet(name))
            names.append(QLatin1String(name));
    }
    if (names.isEmpty())
        return;
    // Waited for (briefly, this is the UI thread): the start that follows must see them.
    QProcess import;
    import.start(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user"), QStringLiteral("import-environment")} + names);
    if (import.waitForStarted(1000) && !import.waitForFinished(2000)) {
        import.kill();
        import.waitForFinished(500);
    }
    QProcess::startDetached(QStringLiteral("dbus-update-activation-environment"), QStringList{QStringLiteral("--systemd")} + names);
}
#endif

void Backend::connectToService()
{
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        m_socket.abort();
    m_socket.connectToServer(m_serverName);
}

void Backend::command(const QString &cmd, QCborMap args)
{
    if (!serviceConnected())
        return;
    args.insert(QStringLiteral("cmd"), cmd);
    m_conn.send(args);
}

void Backend::setExploring(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setExploring"), a);
}

void Backend::pair(const QString &peerId, const QString &trust)
{
    m_pairTrust.insert(peerId, trust);
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("trust"), trust);
    command(QStringLiteral("pair"), a);
}

void Backend::setTrust(const QString &peerId, const QString &trust)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("trust"), trust);
    command(QStringLiteral("setTrust"), a);
}

void Backend::setPairTrust(const QString &peerId, const QString &trust)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("trust"), trust);
    command(QStringLiteral("setPairTrust"), a);
}

void Backend::submitPin(const QString &peerId, const QString &pin)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    a.insert(QStringLiteral("pin"), pin);
    command(QStringLiteral("pin"), a);
}

void Backend::cancelPair(const QString &peerId)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    command(QStringLiteral("cancelPair"), a);
}

void Backend::unpair(const QString &peerId)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    command(QStringLiteral("unpair"), a);
}

void Backend::unforget(const QString &peerId)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    command(QStringLiteral("unforget"), a);
}

void Backend::rename(const QString &name)
{
    QCborMap a;
    a.insert(QStringLiteral("name"), name);
    command(QStringLiteral("rename"), a);
}

void Backend::addPeer(const QString &host, int port)
{
    QCborMap a;
    a.insert(QStringLiteral("host"), host.trimmed());
    a.insert(QStringLiteral("port"), port);
    command(QStringLiteral("addPeer"), a);
}

void Backend::onMessage(const QCborMap &msg)
{
    const QString ev = msg.value(QStringLiteral("ev")).toString();
    if (ev == QLatin1String("state")) {
        const QCborMap self = msg.value(QStringLiteral("self")).toMap();
        m_selfId = self.value(QStringLiteral("id")).toString();
        m_selfName = self.value(QStringLiteral("name")).toString();
        m_fingerprint = self.value(QStringLiteral("fingerprint")).toString();
        m_port = int(self.value(QStringLiteral("port")).toInteger());
        m_ports = PortsInfo::fromCbor(msg.value(QStringLiteral("ports")).toMap());
        m_selfVersion = self.value(QStringLiteral("version")).toString();
        m_updates = msg.value(QStringLiteral("updates")).toMap().toVariantMap();
        m_exploring = msg.value(QStringLiteral("exploring")).toBool();
        m_clipboardSharing = msg.value(QStringLiteral("clipboard")).toBool();
        m_autostartSupported = msg.value(QStringLiteral("autostartSupported")).toBool();
        m_autostart = msg.value(QStringLiteral("autostart")).toBool();
        m_remoteViewEnabled = msg.value(QStringLiteral("remoteViewEnabled")).toBool(true);
        m_otherSubnetsSupported = msg.value(QStringLiteral("otherSubnetsSupported")).toBool();
        m_otherSubnets = msg.value(QStringLiteral("otherSubnets")).toBool();
        m_pasteMove = msg.value(QStringLiteral("pasteMove")).toBool(false);
        if (keyDiagnosticsAvailable()) {
            const QCborMap diag = msg.value(QStringLiteral("keyDiag")).toMap();
            const bool on = diag.value(QStringLiteral("on")).toBool();
            const QString by = diag.value(QStringLiteral("by")).toString();
            const double endsAt = on ? double(QDateTime::currentMSecsSinceEpoch() + diag.value(QStringLiteral("ms")).toInteger()) : 0;
            // The end moves a little with each state (its delivery takes time): only a change of
            // more than a second is one.
            if (on != m_keyDiagActive || by != m_keyDiagBy || qAbs(endsAt - m_keyDiagEndsAt) > 1000) {
                m_keyDiagActive = on;
                m_keyDiagBy = by;
                m_keyDiagEndsAt = endsAt;
                emit keyDiagChanged();
            }
        }
        m_inputProblem = msg.value(QStringLiteral("inputProblem")).toString();
        m_inputProblemLink = QUrl(msg.value(QStringLiteral("inputProblemLink")).toString());
        m_files = msg.value(QStringLiteral("files")).toMap().toVariantMap();
        m_screenViewerName.clear();
        if (msg.contains(QStringLiteral("screenViewer"))) {
            const QCborMap viewer = msg.value(QStringLiteral("screenViewer")).toMap();
            m_screenViewerName = viewer.value(QStringLiteral("name")).toString();
            if (m_screenViewerName.isEmpty())
                m_screenViewerName = tr("another device");
        }
        m_trusted.clear();
        m_nearby.clear();
        for (const QCborValue &v : msg.value(QStringLiteral("peers")).toArray()) {
            const PeerInfo peer = PeerInfo::fromCbor(v.toMap());
            (peer.trusted ? m_trusted : m_nearby).append(peer);
        }
        for (const PeerInfo &p : m_protocolAlerts.fresh(m_trusted))
            emit protocolAlert(protocolAlertText(p));
        m_forgotten.clear();
        for (const QCborValue &v : msg.value(QStringLiteral("forgotten")).toArray())
            m_forgotten.append(PeerInfo::fromCbor(v.toMap()));
        const QCborMap ws = msg.value(QStringLiteral("workspace")).toMap();
        m_masterId = ws.value(QStringLiteral("master")).toString();
        m_activeId = ws.value(QStringLiteral("active")).toString();
        m_sessionRunning = ws.value(QStringLiteral("session")).toBool();
        m_devices.clear();
        for (const QCborValue &v : ws.value(QStringLiteral("devices")).toArray()) {
            DeviceInfo device = DeviceInfo::fromCbor(v.toMap());
            for (DeviceScreen &screen : device.screens)
                screen.thumbKey = thumbKey(device.id, screen.i);
            m_devices.append(device);
        }
        emit stateChanged();
    } else if (ev == QLatin1String("pairPin")) {
        emit pinShown(msg.value(QStringLiteral("id")).toString(), msg.value(QStringLiteral("name")).toString(),
                      msg.value(QStringLiteral("pin")).toString(),
                      msg.value(QStringLiteral("trust")).toString(QStringLiteral("device")),
                      msg.value(QStringLiteral("suggested")).toString());
    } else if (ev == QLatin1String("pairPrompt")) {
        // The service says what the request chose; the choice remembered here covers a service
        // that does not.
        const QString id = msg.value(QStringLiteral("id")).toString();
        emit pinRequested(id, msg.value(QStringLiteral("name")).toString(),
                          msg.value(QStringLiteral("trust")).toString(m_pairTrust.value(id, QStringLiteral("device"))));
    } else if (ev == QLatin1String("viewPrompt")) {
        emit viewPromptRequested(msg.value(QStringLiteral("id")).toString(), msg.value(QStringLiteral("name")).toString(),
                                 int(msg.value(QStringLiteral("timeout")).toInteger()));
    } else if (ev == QLatin1String("viewPromptDone")) {
        emit viewPromptClosed(msg.value(QStringLiteral("id")).toString());
    } else if (ev == QLatin1String("thumb")) {
        const QString key = thumbKey(msg.value(QStringLiteral("device")).toString(),
                                     msg.value(QStringLiteral("screen")).toString());
        const QByteArray jpeg = msg.value(QStringLiteral("jpeg")).toByteArray();
        if (jpeg.isEmpty()) {
            m_thumbnails->remove(key); // dropped (e.g. the device revoked our view grant)
        } else {
            // Peer data: check the declared size before decoding anything.
            QBuffer buffer;
            buffer.setData(jpeg);
            QImageReader reader(&buffer, "JPG");
            const QSize size = reader.size();
            if (!size.isValid() || size.width() > kMaxThumbnailSide || size.height() > kMaxThumbnailSide)
                return;
            const QImage image = reader.read();
            if (image.isNull())
                return;
            m_thumbnails->set(key, image);
        }
        ++m_thumbRevision;
        emit thumbnailsChanged();
    } else if (ev == QLatin1String("phoneUpload")) {
        emit phoneUploaded(QFileInfo(msg.value(QStringLiteral("path")).toString()).fileName());
    } else if (ev == QLatin1String("dragCatch")) {
        const qint64 rid = msg.value(QStringLiteral("rid")).toInteger();
        m_fileDrag.catchFiles(QPoint(int(msg.value(QStringLiteral("x")).toInteger()),
                                     int(msg.value(QStringLiteral("y")).toInteger())),
                              [this, rid](const QStringList &paths) {
                                  QCborMap r;
                                  r.insert(QStringLiteral("paths"), QCborArray::fromStringList(paths));
                                  replyToService(rid, r);
                              });
    } else if (ev == QLatin1String("dropFolder")) {
        QCborMap r;
        r.insert(QStringLiteral("folder"),
                 m_fileDrag.folderAt(QPoint(int(msg.value(QStringLiteral("x")).toInteger()),
                                            int(msg.value(QStringLiteral("y")).toInteger()))));
        replyToService(msg.value(QStringLiteral("rid")).toInteger(), r);
    } else if (ev == QLatin1String("recycle")) {
        QStringList paths;
        for (const QCborValue &v : msg.value(QStringLiteral("paths")).toArray())
            paths.append(v.toString());
        QString error;
        QCborMap r;
        r.insert(QStringLiteral("ok"), m_fileDrag.recycle(paths, &error));
        r.insert(QStringLiteral("error"), error);
        replyToService(msg.value(QStringLiteral("rid")).toInteger(), r);
    } else if (ev == QLatin1String("dragHover")) {
        const qint64 rid = msg.value(QStringLiteral("rid")).toInteger();
        const quint64 offer = quint64(msg.value(QStringLiteral("o")).toInteger());
        FileDragAgent::HoverSpec spec;
        spec.own = msg.value(QStringLiteral("own")).toBool();
        for (const QCborValue &v : msg.value(QStringLiteral("paths")).toArray())
            spec.paths.append(v.toString());
        for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
            spec.files.append(ClipboardBridge::listedFromCbor(v.toArray()));
        spec.count = int(msg.value(QStringLiteral("n")).toInteger());
        spec.local = msg.value(QStringLiteral("local")).toBool();
        spec.heartbeat = msg.value(QStringLiteral("alive")).toBool();
        QCborMap fetchArgs;
        fetchArgs.insert(QStringLiteral("o"), qint64(offer));
        m_fileDrag.hover(
            spec,
            [this, fetchArgs](QString *error) { // on the drag's thread, while the drop target waits
                return fetchFromService(QStringLiteral("dragFetch"), fetchArgs, QStringLiteral("dragFetched"), error);
            },
            [this, rid](const FileDragAgent::HoverResult &result) {
                QCborMap r;
                r.insert(QStringLiteral("dropped"), result.dropped);
                r.insert(QStringLiteral("confirmed"), result.confirmed);
                r.insert(QStringLiteral("unavailable"), result.unavailable);
                if (!result.folder.isEmpty())
                    r.insert(QStringLiteral("folder"), result.folder);
                replyToService(rid, r);
            });
    } else if (ev == QLatin1String("dragHoverEnd")) {
        m_fileDrag.endHover(msg.value(QStringLiteral("drop")).toBool());
    } else if (ev == QLatin1String("dragAlive")) {
        m_fileDrag.alive();
    } else if (ev == QLatin1String("dragBadge")) {
        m_fileDrag.setBadge(msg.value(QStringLiteral("text")).toString());
    } else if (ev == QLatin1String("clipConfig")) {
        m_clipboard.setEnabled(msg.value(QStringLiteral("on")).toBool());
    } else if (ev == QLatin1String("clipClear")) {
        // The service holds nothing for the clipboard object we may still own (it restarted):
        // drop it rather than let every paste of it fail.
        m_clipboard.clearOwned();
    } else if (ev == QLatin1String("clipSet")) {
        QStringList files;
        for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
            files.append(v.toString());
        // Files and folders copied on another device (a folder is listed too, as [path, -1]).
        const int pending = int(msg.value(QStringLiteral("pendingFiles")).toInteger());
        QList<ClipboardBridge::ListedFile> listed;
        for (const QCborValue &v : msg.value(QStringLiteral("pendingList")).toArray())
            listed.append(ClipboardBridge::listedFromCbor(v.toArray()));
        // Sealed content: the formats present, decrypted only when the user pastes. The id is sent
        // as bytes; the service's clipUnseal answer echoes it as hex. A paste of the files names it.
        const QCborValue kidValue = msg.value(QStringLiteral("kid"));
        const QByteArray kid = kidValue.isByteArray() ? kidValue.toByteArray()
                                                      : QByteArray::fromHex(kidValue.toString().toLatin1());
        auto fetch = [this, kid](QString *error) { return fetchClipboardFiles(kid, error); };
        // stream (a 0.1.30 service): the files can be pasted while they arrive, listed on demand
        // when the announcement does not list them (Explorer's own progress and Cancel).
        // move (a 0.1.31 service, the user's "move pastes" setting on): a large paste is a move
        // paste instead (transferred whole, then moved by Explorer out of its transfer folder).
        qint64 pendingBytes = msg.value(QStringLiteral("pendingBytes")).toInteger(-1);
        if (pendingBytes < 0) {
            pendingBytes = 0;
            for (const ClipboardBridge::ListedFile &f : std::as_const(listed))
                pendingBytes += f.size;
        }
        const bool move = pending > 0 && msg.value(QStringLiteral("move")).toBool()
            && ClipboardBridge::wantsMovePaste(pending, pendingBytes);
        const ClipboardBridge::FileStreaming streaming =
            fileStreaming(kid, msg.value(QStringLiteral("stream")).toBool(), move);
        if (msg.contains(QStringLiteral("sealed"))) {
            QList<ClipboardBridge::SealedFormat> formats;
            for (const QCborValue &v : msg.value(QStringLiteral("sealed")).toArray()) {
                const QCborArray item = v.toArray();
                formats.append(ClipboardBridge::SealedFormat{item.at(0).toString(), item.at(1).toInteger()});
            }
            auto unseal = [this, kid](const QString &mime, QString *error) {
                return unsealClipboardFormat(kid, mime, error);
            };
            // Files listed in full go on as virtual files; a longer copy (no list) as a deferred
            // CF_HDROP, both rendered at the paste.
            // gated: reading it needs a paste grant (this device is a guest of the master), so a
            // read that does not come from a paste is refused here without asking the service.
            m_clipboard.applySealed(formats, listed.size() == pending ? listed : QList<ClipboardBridge::ListedFile>(),
                                    pending, unseal, fetch, msg.value(QStringLiteral("gated")).toBool(), streaming);
        } else if (pending > 0 && ClipboardBridge::supportsDeferredFiles() && streaming.move) {
            m_clipboard.applySealed({}, listed.size() == pending ? listed : QList<ClipboardBridge::ListedFile>(),
                                    pending, {}, fetch, false, streaming);
        } else if (pending > 0 && ClipboardBridge::supportsDeferredFiles() && listed.size() == pending) {
            m_clipboard.applyVirtualFiles(listed, fetch, streaming); // names visible, nothing transferred yet
        } else if (pending > 0 && ClipboardBridge::supportsDeferredFiles()) {
            m_clipboard.applyDeferredFiles(pending, fetch);
        } else {
            m_clipboard.apply(msg.value(QStringLiteral("f")).toMap(), files); // phone text / older service
        }
    } else if (ev == QLatin1String("transfer")) {
        onTransfer(msg);
    } else if (ev == QLatin1String("clipWaiting")) {
        onClipWaiting(msg);
    } else if (ev == QLatin1String("clipReceived")) {
        onClipReceived(msg);
    } else if (ev == QLatin1String("keyDiagLine") && keyDiagnosticsAvailable()) {
        const QString line = msg.value(QStringLiteral("l")).toString();
        if (msg.value(QStringLiteral("h")).toBool()) {
            m_keyDiagHeader.append(line);
        } else {
            m_keyDiagLines.append(line);
            while (m_keyDiagLines.size() + m_keyDiagHeader.size() > kKeyDiagMaxLines) {
                m_keyDiagLines.removeFirst();
                ++m_keyDiagDropped;
            }
        }
        if (!m_keyDiagTextTimer.isActive())
            m_keyDiagTextTimer.start();
    } else if (ev == QLatin1String("keyDiagLines") && keyDiagnosticsAvailable()) {
        // The whole run so far; its first h lines are the header (the service gives the count).
        m_keyDiagHeader.clear();
        m_keyDiagLines.clear();
        m_keyDiagDropped = 0;
        const QCborArray lines = msg.value(QStringLiteral("lines")).toArray();
        const qsizetype header = msg.value(QStringLiteral("h")).toInteger();
        for (qsizetype i = 0; i < lines.size(); ++i)
            (i < header ? m_keyDiagHeader : m_keyDiagLines).append(lines.at(i).toString());
        m_keyDiagTextTimer.stop();
        updateKeyDiagText();
    } else if (ev == QLatin1String("notice")) {
        emit notice(msg.value(QStringLiteral("message")).toString(), msg.value(QStringLiteral("problem")).toBool());
    } else if (ev == QLatin1String("sharedFolderRejected")) {
        emit sharedFolderRejected(msg.value(QStringLiteral("path")).toString(),
                                  msg.value(QStringLiteral("reason")).toString());
    } else if (ev == QLatin1String("error")) {
        emit serviceError(msg.value(QStringLiteral("message")).toString());
    } else if (ev == QLatin1String("quit")) {
        QCoreApplication::quit(); // an update is being installed; the service starts the new agent
    } else if (ev == QLatin1String("updateResult")) {
        emit updateFinished(msg.value(QStringLiteral("ok")).toBool(), msg.value(QStringLiteral("message")).toString());
    } else if (ev == QLatin1String("pairResult")) {
        m_pairTrust.remove(msg.value(QStringLiteral("id")).toString());
        emit pairingFinished(msg.value(QStringLiteral("id")).toString(), msg.value(QStringLiteral("ok")).toBool(),
                             msg.value(QStringLiteral("error")).toString());
    }
}

namespace {
const QString kLicenseRoot = QStringLiteral(":/deskflower/licenses");
} // namespace

QStringList Backend::licenseDocuments() const
{
    // DeskFlower's own license and the notices first, then the licence texts.
    QStringList docs = {QStringLiteral("LICENSE.md"), QStringLiteral("COMMERCIAL-LICENSE.md"),
                        QStringLiteral("THIRD-PARTY-NOTICES.md")};
    const QStringList texts = QDir(kLicenseRoot + QStringLiteral("/licenses")).entryList(QDir::Files, QDir::Name);
    for (const QString &name : texts)
        docs << QStringLiteral("licenses/") + name;
    return docs;
}

QString Backend::licenseDocument(const QString &name) const
{
    if (name.contains(QLatin1String("..")))
        return {};
    QFile f(kLicenseRoot + QLatin1Char('/') + name);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
