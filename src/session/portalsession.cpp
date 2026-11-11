#include "portalsession.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

#include <unistd.h>

Q_LOGGING_CATEGORY(lcPortal, "deskflower.portal")

namespace {

const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kRemoteDesktop = QStringLiteral("org.freedesktop.portal.RemoteDesktop");
const QString kScreenCast = QStringLiteral("org.freedesktop.portal.ScreenCast");
const QString kClipboard = QStringLiteral("org.freedesktop.portal.Clipboard");
const QString kRequest = QStringLiteral("org.freedesktop.portal.Request");
const QString kSession = QStringLiteral("org.freedesktop.portal.Session");

// RemoteDesktop device types and ScreenCast source types / cursor modes.
constexpr uint kKeyboard = 1, kPointer = 2;
constexpr uint kMonitor = 1;
constexpr uint kCursorEmbedded = 2;
// Remember the permission until the user revokes it.
constexpr uint kPersistUntilRevoked = 2;

QString token()
{
    return QStringLiteral("deskflower%1").arg(QRandomGenerator::global()->generate());
}

QPoint readPair(const QVariant &value)
{
    int a = 0, b = 0;
    const QDBusArgument arg = value.value<QDBusArgument>();
    arg.beginStructure();
    arg >> a >> b;
    arg.endStructure();
    return QPoint(a, b);
}

} // namespace

PortalSession::PortalSession(const Options &options, QObject *parent) : QObject(parent), m_options(options)
{
    if (m_options.tokenFile.isEmpty())
        m_options.tokenFile = defaultTokenFile();
}

PortalSession::~PortalSession()
{
    if (!m_session.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall(kService, m_session, kSession, QStringLiteral("Close"));
        QDBusConnection::sessionBus().call(close, QDBus::NoBlock);
    }
}

PortalSession *PortalSession::instance()
{
    static PortalSession *session = new PortalSession(Options{}, qApp);
    return session;
}

QString PortalSession::defaultTokenFile()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("portal-restore-token"));
}

void PortalSession::start()
{
    if (m_state == State::Starting || m_state == State::Started)
        return;
    m_state = State::Starting;
    m_problem.clear();
    m_refused = false;
    m_inputGranted = false;
    m_streams.clear();
    m_clipboardEnabled = false;
    if (!QDBusConnection::sessionBus().isConnected()) {
        fail(tr("No D-Bus session bus (the desktop portal cannot be reached)"));
        return;
    }
    QVariantMap options;
    options.insert(QStringLiteral("session_handle_token"), token());
    request(kRemoteDesktop, QStringLiteral("CreateSession"), {options}, tr("creating the remote desktop session"),
            [this](const QVariantMap &r) {
                const QVariant handle = r.value(QStringLiteral("session_handle"));
                m_session = handle.canConvert<QDBusObjectPath>() && !handle.value<QDBusObjectPath>().path().isEmpty()
                                ? handle.value<QDBusObjectPath>().path()
                                : handle.toString();
                if (m_session.isEmpty()) {
                    fail(tr("The desktop portal created no session"));
                    return;
                }
                QDBusConnection::sessionBus().connect(kService, m_session, kSession, QStringLiteral("Closed"), this,
                                                      SLOT(onClosed(QVariantMap)));
                qCInfo(lcPortal) << "Remote desktop session" << m_session;
                selectDevices();
            });
}

void PortalSession::selectDevices()
{
    QVariantMap options;
    options.insert(QStringLiteral("types"), m_options.input ? (kKeyboard | kPointer) : 0u);
    options.insert(QStringLiteral("persist_mode"), kPersistUntilRevoked);
    const QString restore = readToken();
    if (!restore.isEmpty())
        options.insert(QStringLiteral("restore_token"), restore);
    request(kRemoteDesktop, QStringLiteral("SelectDevices"), {QVariant::fromValue(QDBusObjectPath(m_session)), options},
            tr("choosing the keyboard and mouse"), [this](const QVariantMap &) { selectSources(); });
}

void PortalSession::selectSources()
{
    if (!m_options.screens) {
        requestClipboard();
        return;
    }
    // Every monitor; the cursor is drawn by the viewer from its own position reports, but the
    // embedded one keeps frames meaningful where those are missing. persist_mode belongs to
    // SelectDevices in a remote desktop session.
    QVariantMap options;
    options.insert(QStringLiteral("types"), kMonitor);
    options.insert(QStringLiteral("multiple"), true);
    options.insert(QStringLiteral("cursor_mode"), kCursorEmbedded);
    request(kScreenCast, QStringLiteral("SelectSources"), {QVariant::fromValue(QDBusObjectPath(m_session)), options},
            tr("choosing the screens"), [this](const QVariantMap &) { requestClipboard(); });
}

void PortalSession::requestClipboard()
{
    if (!m_options.clipboard) {
        startSession();
        return;
    }
    // Must come before Start. Older portals have no Clipboard interface: the rest still works.
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kClipboard, QStringLiteral("RequestClipboard"));
    call << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariantMap();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 5000), this);
    const QString session = m_session;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, session](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            qCWarning(lcPortal) << "The desktop portal has no clipboard:" << w->error().message();
        if (m_state == State::Starting && m_session == session)
            startSession();
    });
}

void PortalSession::startSession()
{
    QVariantMap options;
    request(kRemoteDesktop, QStringLiteral("Start"),
            {QVariant::fromValue(QDBusObjectPath(m_session)), QString(), options}, tr("starting the remote desktop session"),
            [this](const QVariantMap &r) {
                m_clipboardEnabled = r.value(QStringLiteral("clipboard_enabled")).toBool();
                const QString restore = r.value(QStringLiteral("restore_token")).toString();
                if (!restore.isEmpty())
                    writeToken(restore);
                m_streams.clear();
                if (r.contains(QStringLiteral("streams"))) {
                    const QDBusArgument arg = r.value(QStringLiteral("streams")).value<QDBusArgument>();
                    arg.beginArray();
                    while (!arg.atEnd()) {
                        Stream s;
                        QVariantMap props;
                        arg.beginStructure();
                        arg >> s.node >> props;
                        arg.endStructure();
                        if (props.contains(QStringLiteral("position")) && props.contains(QStringLiteral("size")))
                            s.geometry = QRect(readPair(props.value(QStringLiteral("position"))),
                                               QSize(readPair(props.value(QStringLiteral("size"))).x(),
                                                     readPair(props.value(QStringLiteral("size"))).y()));
                        m_streams.append(s);
                    }
                    arg.endArray();
                }
                // Without the keyboard and mouse (the user unticked them), the clipboard and the
                // screens still work: the input backend reports the problem (inputGranted).
                const uint devices = r.value(QStringLiteral("devices")).toUInt();
                m_inputGranted = (devices & kKeyboard) || (devices & kPointer);
                if (m_options.input && !m_inputGranted)
                    qCWarning(lcPortal) << "The desktop did not allow controlling the keyboard and mouse";
                m_state = State::Started;
                qCInfo(lcPortal) << "Remote desktop session started: devices" << devices << "streams"
                                 << m_streams.size() << "clipboard" << m_clipboardEnabled;
                emit started();
            });
}

bool PortalSession::worthRestarting() const
{
    return m_state == State::Closed || (m_state == State::Failed && !m_refused);
}

int PortalSession::connectToEis()
{
    if (m_state != State::Started || !m_inputGranted)
        return -1;
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kRemoteDesktop, QStringLiteral("ConnectToEIS"));
    call << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariantMap();
    const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(call);
    if (!reply.isValid()) {
        qCWarning(lcPortal) << "ConnectToEIS failed:" << reply.error().message();
        return -1;
    }
    return dup(reply.value().fileDescriptor());
}

int PortalSession::openPipeWireRemote()
{
    if (m_state != State::Started)
        return -1;
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kScreenCast, QStringLiteral("OpenPipeWireRemote"));
    call << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariantMap();
    const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(call);
    if (!reply.isValid()) {
        qCWarning(lcPortal) << "OpenPipeWireRemote failed:" << reply.error().message();
        return -1;
    }
    return dup(reply.value().fileDescriptor());
}

void PortalSession::request(const QString &interface, const QString &method, QVariantList args, const QString &step,
                            Callback callback)
{
    // The Request object's path is predictable: subscribe to its Response before calling, so that
    // an immediate answer is not missed.
    const QString handleToken = token();
    for (QVariant &a : args) {
        if (a.metaType() == QMetaType::fromType<QVariantMap>()) {
            QVariantMap options = a.toMap();
            options.insert(QStringLiteral("handle_token"), handleToken);
            a = options;
        }
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handleToken);
    m_pending[path] = {step, std::move(callback)};
    bus.connect(kService, path, kRequest, QStringLiteral("Response"), this, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, interface, method);
    call.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path, step](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError()) {
            m_pending.erase(path);
            fail(tr("The desktop portal failed while %1: %2").arg(step, reply.error().message()));
            return;
        }
        // An older portal may answer on another path than predicted.
        const QString actual = reply.value().path();
        if (!actual.isEmpty() && actual != path) {
            auto it = m_pending.find(path);
            if (it != m_pending.end()) {
                m_pending[actual] = std::move(it->second);
                m_pending.erase(it);
                QDBusConnection::sessionBus().connect(kService, actual, kRequest, QStringLiteral("Response"), this,
                                                      SLOT(onResponse(uint, QVariantMap)));
            }
        }
    });
}

void PortalSession::onResponse(uint response, const QVariantMap &results)
{
    const QString path = message().path();
    auto it = m_pending.find(path);
    if (it == m_pending.end())
        return;
    auto [step, callback] = std::move(it->second);
    m_pending.erase(it);
    QDBusConnection::sessionBus().disconnect(kService, path, kRequest, QStringLiteral("Response"), this,
                                             SLOT(onResponse(uint, QVariantMap)));
    if (m_state != State::Starting)
        return;
    if (response == 1) {
        m_refused = true;
        fail(tr("The remote desktop permission was refused (%1)").arg(step));
        return;
    }
    if (response != 0) {
        fail(tr("The desktop portal failed while %1").arg(step));
        return;
    }
    callback(results);
}

void PortalSession::onClosed(const QVariantMap &)
{
    if (message().path() != m_session)
        return;
    qCWarning(lcPortal) << "The desktop closed the remote desktop session";
    m_session.clear();
    m_state = State::Closed;
    m_problem = tr("The desktop ended the remote desktop session");
    emit closed();
}

void PortalSession::fail(const QString &problem)
{
    qCWarning(lcPortal).noquote() << problem;
    m_state = State::Failed;
    m_problem = problem;
    if (!m_session.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall(kService, m_session, kSession, QStringLiteral("Close"));
        QDBusConnection::sessionBus().call(close, QDBus::NoBlock);
        m_session.clear();
    }
    emit failed(problem);
}

QString PortalSession::readToken() const
{
    QFile f(m_options.tokenFile);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(1024)).trimmed();
}

void PortalSession::writeToken(const QString &token) const
{
    QDir().mkpath(QFileInfo(m_options.tokenFile).absolutePath());
    QSaveFile f(m_options.tokenFile);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(token.toUtf8());
    f.commit();
}
