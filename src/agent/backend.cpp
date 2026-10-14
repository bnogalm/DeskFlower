#include "backend.h"

#include "thumbnailprovider.h"

#include <QBuffer>
#include <QCborArray>
#include <QCborValue>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLocalSocket>
#include <QProcess>
#include <QVariantMap>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winsvc.h>
#elif defined(Q_OS_MACOS)
#include <unistd.h>
#endif

namespace {
// Thumbnails are captured at most 480x300; anything far larger is refused undecoded.
constexpr int kMaxThumbnailSide = 1024;
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
        setServiceStartError({});
        emit serviceConnectedChanged();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        emit serviceConnectedChanged();
        m_reconnect.start();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        // Launching the agent starts a service that the user stopped (by quitting) last time.
        if (!m_triedStart && m_socket.error() == QLocalSocket::ServerNotFoundError) {
            m_triedStart = true;
            startService();
        }
        m_reconnect.start();
    });

    m_reconnect.setInterval(1000);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Backend::connectToService);

    connect(&m_clipboard, &ClipboardBridge::changed, this, [this](const QCborMap &formats, const QStringList &files) {
        QCborMap a;
        a.insert(QStringLiteral("f"), formats);
        a.insert(QStringLiteral("files"), QCborArray::fromStringList(files));
        command(QStringLiteral("clip"), a);
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] { m_clipboard.setEnabled(false); });

    connectToService();
}

void Backend::replyToService(qint64 requestId, QCborMap reply)
{
    reply.insert(QStringLiteral("rid"), requestId);
    command(QStringLiteral("agentReply"), reply);
}

QStringList Backend::fetchClipboardFiles(QString *error) const
{
    return fetchFromService(QStringLiteral("clipFetch"), {}, QStringLiteral("clipFetched"), error);
}

QStringList Backend::fetchFromService(const QString &cmd, QCborMap args, const QString &replyEvent, QString *error) const
{
    // Called while an application waits for the pasted files (inside its clipboard request): no
    // event loop, so a separate connection with blocking reads.
    QLocalSocket socket;
    socket.connectToServer(m_serverName);
    if (!socket.waitForConnected(3000)) {
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
    while (elapsed.elapsed() < kPasteTimeoutMs) {
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
            if (msg.value(QStringLiteral("ev")).toString() != replyEvent)
                continue; // state, thumbnails...: what every new connection gets
            if (!msg.value(QStringLiteral("ok")).toBool()) {
                *error = msg.value(QStringLiteral("error")).toString();
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
    for (const QVariant &v : m_devices) {
        const QVariantMap d = v.toMap();
        if (d.value(QStringLiteral("id")).toString() == m_masterId)
            return d.value(QStringLiteral("name")).toString();
    }
    return {};
}

void Backend::setRemoteView(const QString &deviceId, bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), deviceId);
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setRemoteView"), a);
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

void Backend::updateFrom(const QString &peerId)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    command(QStringLiteral("update"), a);
}

double Backend::updateProgress() const
{
    const double total = m_updates.value(QStringLiteral("total")).toDouble();
    return total > 0 ? m_updates.value(QStringLiteral("received")).toDouble() / total : 0.0;
}

void Backend::setAutostart(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setAgentAutostart"), a);
}

void Backend::setClipboardSharing(bool on)
{
    QCborMap a;
    a.insert(QStringLiteral("on"), on);
    command(QStringLiteral("setClipboardSharing"), a);
}

void Backend::setLayout(const QVariantList &packs)
{
    QCborArray list;
    for (const QVariant &v : packs) {
        const QVariantMap p = v.toMap();
        QCborMap m;
        m.insert(QStringLiteral("id"), p.value(QStringLiteral("id")).toString());
        m.insert(QStringLiteral("x"), qRound(p.value(QStringLiteral("x")).toDouble()));
        m.insert(QStringLiteral("y"), qRound(p.value(QStringLiteral("y")).toDouble()));
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
    QProcess::startDetached(QStringLiteral("launchctl"),
                            {QStringLiteral("kickstart"),
                             QStringLiteral("gui/%1/com.deskflower.service").arg(::getuid())});
#else
    QProcess::startDetached(QStringLiteral("systemctl"),
                            {QStringLiteral("--user"), QStringLiteral("start"), QStringLiteral("deskflower.service")});
#endif
    m_reconnect.start(); // connects as soon as it listens
}

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

void Backend::pair(const QString &peerId)
{
    QCborMap a;
    a.insert(QStringLiteral("id"), peerId);
    command(QStringLiteral("pair"), a);
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
        m_selfVersion = self.value(QStringLiteral("version")).toString();
        m_updates = msg.value(QStringLiteral("updates")).toMap().toVariantMap();
        m_exploring = msg.value(QStringLiteral("exploring")).toBool();
        m_clipboardSharing = msg.value(QStringLiteral("clipboard")).toBool();
        m_autostartSupported = msg.value(QStringLiteral("autostartSupported")).toBool();
        m_autostart = msg.value(QStringLiteral("autostart")).toBool();
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
            const QVariantMap peer = v.toMap().toVariantMap();
            (peer.value(QStringLiteral("trusted")).toBool() ? m_trusted : m_nearby).append(peer);
        }
        const QCborMap ws = msg.value(QStringLiteral("workspace")).toMap();
        m_masterId = ws.value(QStringLiteral("master")).toString();
        m_activeId = ws.value(QStringLiteral("active")).toString();
        m_sessionRunning = ws.value(QStringLiteral("session")).toBool();
        m_devices.clear();
        for (const QCborValue &v : ws.value(QStringLiteral("devices")).toArray()) {
            QVariantMap device = v.toMap().toVariantMap();
            const QString deviceId = device.value(QStringLiteral("id")).toString();
            QVariantList screens;
            for (const QVariant &sv : device.value(QStringLiteral("screens")).toList()) {
                QVariantMap screen = sv.toMap();
                screen.insert(QStringLiteral("thumbKey"),
                              thumbKey(deviceId, screen.value(QStringLiteral("i")).toString()));
                screens.append(screen);
            }
            device.insert(QStringLiteral("screens"), screens);
            m_devices.append(device);
        }
        emit stateChanged();
    } else if (ev == QLatin1String("pairPin")) {
        emit pinShown(msg.value(QStringLiteral("id")).toString(), msg.value(QStringLiteral("name")).toString(),
                      msg.value(QStringLiteral("pin")).toString());
    } else if (ev == QLatin1String("pairPrompt")) {
        emit pinRequested(msg.value(QStringLiteral("id")).toString(), msg.value(QStringLiteral("name")).toString());
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
        for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray()) {
            const QCborArray item = v.toArray();
            spec.files.append(ClipboardBridge::ListedFile{item.at(0).toString(), item.at(1).toInteger()});
        }
        spec.count = int(msg.value(QStringLiteral("n")).toInteger());
        QCborMap fetchArgs;
        fetchArgs.insert(QStringLiteral("o"), qint64(offer));
        m_fileDrag.hover(
            spec,
            [this, fetchArgs](QString *error) { // on the drag's thread, while the drop target waits
                return fetchFromService(QStringLiteral("dragFetch"), fetchArgs, QStringLiteral("dragFetched"), error);
            },
            [this, rid](bool dropped, bool transferred) {
                QCborMap r;
                r.insert(QStringLiteral("dropped"), dropped);
                r.insert(QStringLiteral("transferred"), transferred);
                replyToService(rid, r);
            });
    } else if (ev == QLatin1String("dragHoverEnd")) {
        m_fileDrag.endHover(msg.value(QStringLiteral("drop")).toBool());
    } else if (ev == QLatin1String("dragBadge")) {
        m_fileDrag.setBadge(msg.value(QStringLiteral("text")).toString());
    } else if (ev == QLatin1String("clipConfig")) {
        m_clipboard.setEnabled(msg.value(QStringLiteral("on")).toBool());
    } else if (ev == QLatin1String("clipSet")) {
        QStringList files;
        for (const QCborValue &v : msg.value(QStringLiteral("files")).toArray())
            files.append(v.toString());
        const int pending = int(msg.value(QStringLiteral("pendingFiles")).toInteger());
        QList<ClipboardBridge::ListedFile> listed;
        for (const QCborValue &v : msg.value(QStringLiteral("pendingList")).toArray()) {
            const QCborArray item = v.toArray();
            listed.append(ClipboardBridge::ListedFile{item.at(0).toString(), item.at(1).toInteger()});
        }
        auto fetch = [this](QString *error) { return fetchClipboardFiles(error); };
        if (pending > 0 && ClipboardBridge::supportsDeferredFiles() && listed.size() == pending)
            m_clipboard.applyVirtualFiles(listed, fetch); // names visible, nothing transferred yet
        else if (pending > 0 && ClipboardBridge::supportsDeferredFiles())
            m_clipboard.applyDeferredFiles(pending, fetch);
        else
            m_clipboard.apply(msg.value(QStringLiteral("f")).toMap(), files);
    } else if (ev == QLatin1String("notice")) {
        emit notice(msg.value(QStringLiteral("message")).toString(), msg.value(QStringLiteral("problem")).toBool());
    } else if (ev == QLatin1String("error")) {
        emit serviceError(msg.value(QStringLiteral("message")).toString());
    } else if (ev == QLatin1String("quit")) {
        QCoreApplication::quit(); // an update is being installed; the service starts the new agent
    } else if (ev == QLatin1String("updateResult")) {
        emit updateFinished(msg.value(QStringLiteral("ok")).toBool(), msg.value(QStringLiteral("message")).toString());
    } else if (ev == QLatin1String("pairResult")) {
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
