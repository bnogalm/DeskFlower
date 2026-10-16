#include "phoneserver.h"

#include "certificate.h"
#include "httpserver.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSet>
#include <QSslSocket>
#include <QUrl>
#include <QUrlQuery>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#include <algorithm>
#include <utility>
#endif

Q_LOGGING_CATEGORY(lcPhone, "deskflower.phone")

namespace df::web {

namespace {
const QByteArray kCookie("df_session");

QString randomHex(int bytes)
{
    QByteArray b(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), bytes / 4);
    return QString::fromLatin1(b.toHex());
}

QString hashed(const QString &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QByteArray resource(const QString &name)
{
    QFile f(QStringLiteral(":/deskflower/web/") + name);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Headers on every response: strict CSP (no inline code), no sniffing, no referrer (the access
// token travels in the first URL), no framing.
HttpResponse respond(int status, QByteArray body = {}, const QByteArray &contentType = {})
{
    HttpResponse r(status, std::move(body));
    if (!contentType.isEmpty())
        r.header("Content-Type", contentType);
    r.header("Content-Security-Policy",
             "default-src 'self'; img-src 'self' blob:; style-src 'self'; script-src 'self'; "
             "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    r.header("X-Content-Type-Options", "nosniff");
    r.header("Referrer-Policy", "no-referrer");
    r.header("Cache-Control", "no-store");
    return r;
}

// What a phone's browser may open from the shared folder: media, plain text and PDF. Anything else
// (HTML, SVG, scripts...) could run as this site, with the phone's session, so it is only ever
// downloaded. Decided by the type (name first, then content); nosniff stops the browser from
// guessing otherwise.
bool opensInBrowser(const QMimeType &type)
{
    static const QStringList kTypes = {QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                                       QStringLiteral("image/gif"),  QStringLiteral("image/webp"),
                                       QStringLiteral("image/bmp"),  QStringLiteral("image/avif"),
                                       QStringLiteral("image/heic"), QStringLiteral("image/heif"),
                                       QStringLiteral("text/plain"), QStringLiteral("application/pdf")};
    const QString name = type.name();
    return kTypes.contains(name) || name.startsWith(QLatin1String("video/")) || name.startsWith(QLatin1String("audio/"));
}

HttpResponse json(const QJsonObject &obj, int status = 200)
{
    return respond(status, QJsonDocument(obj).toJson(QJsonDocument::Compact), "application/json");
}

HttpResponse jsonError(int status, const QString &message)
{
    QJsonObject o;
    o.insert(QStringLiteral("error"), message);
    return json(o, status);
}

HttpResponse html(const QString &name, int status = 200)
{
    return respond(status, resource(name), "text/html; charset=utf-8");
}

// IPv4 addresses of interfaces that have a default gateway: that is the network the phone is on.
// Host-only and virtual adapters (VirtualBox, Hyper-V, WSL...) have none.
QSet<QString> addressesWithGateway()
{
    QSet<QString> out;
#ifdef Q_OS_WIN
    ULONG size = 16 * 1024;
    QByteArray buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(int(size));
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    }
    if (rc != NO_ERROR)
        return out;
    for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress)
            continue;
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            const QHostAddress ip(u->Address.lpSockaddr);
            if (ip.protocol() == QAbstractSocket::IPv4Protocol)
                out.insert(ip.toString());
        }
    }
#elif defined(Q_OS_LINUX)
    // Interfaces carrying the default route (destination 00000000 in /proc/net/route).
    QFile routes(QStringLiteral("/proc/net/route"));
    QSet<QString> names;
    if (routes.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : routes.readAll().split('\n')) {
            const QList<QByteArray> cols = line.simplified().split(' ');
            if (cols.size() > 2 && cols.at(1) == "00000000")
                names.insert(QString::fromLatin1(cols.at(0)));
        }
    }
    for (const QString &name : names) {
        for (const QNetworkAddressEntry &e : QNetworkInterface::interfaceFromName(name).addressEntries()) {
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                out.insert(e.ip().toString());
        }
    }
#endif
    return out;
}
} // namespace

PhoneServer::PhoneServer(Options options, QObject *parent) : QObject(parent), m_options(std::move(options))
{
    // An expired QR code is replaced, so the one on screen always works.
    m_tokenTimer.setSingleShot(true);
    connect(&m_tokenTimer, &QTimer::timeout, this, [this] {
        if (isRunning()) {
            newAccessToken();
            emit accessTokenChanged();
        }
    });
}

PhoneServer::~PhoneServer() = default;

// --- Lifecycle ---------------------------------------------------------------------------------

QStringList PhoneServer::lanAddresses()
{
    static const QStringList kVirtual = {QStringLiteral("vethernet"), QStringLiteral("virtualbox"),
                                         QStringLiteral("vmware"), QStringLiteral("hyper-v"), QStringLiteral("wsl"),
                                         QStringLiteral("docker"), QStringLiteral("loopback"),
                                         QStringLiteral("bluetooth"), QStringLiteral("tailscale"),
                                         QStringLiteral("zerotier"), QStringLiteral("wireguard")};
    const QSet<QString> routed = addressesWithGateway();
    QStringList best, preferred, others;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack) || iface.type() == QNetworkInterface::Virtual)
            continue;
        const QString name = (iface.humanReadableName() + QLatin1Char(' ') + iface.name()).toLower();
        if (std::any_of(kVirtual.begin(), kVirtual.end(), [&](const QString &v) { return name.contains(v); }))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLinkLocal())
                continue;
            const bool physical =
                iface.type() == QNetworkInterface::Wifi || iface.type() == QNetworkInterface::Ethernet;
            (routed.contains(ip.toString()) ? best : physical ? preferred : others).append(ip.toString());
        }
    }
    return best + preferred + others;
}

bool PhoneServer::start(QString *error)
{
    if (m_http)
        return true;
    // Qt's OpenSSL backend supports server-side TLS with the keys we generate on every platform.
    if (QSslSocket::availableBackends().contains(QStringLiteral("openssl")))
        QSslSocket::setActiveBackend(QStringLiteral("openssl"));
    if (!QSslSocket::supportsSsl()) {
        *error = QStringLiteral("TLS is not available (OpenSSL libraries missing)");
        return false;
    }
    qCInfo(lcPhone) << "TLS backend" << QSslSocket::activeBackend();

    const QString host = QHostInfo::localHostName();
    QStringList ips = lanAddresses();
    ips << QStringLiteral("127.0.0.1");
    TlsIdentity identity;
    if (!loadOrCreateTlsIdentity(m_options.configDir, ips, {host, host + QStringLiteral(".local")}, &identity, error))
        return false;

    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setLocalCertificate(identity.certificate);
    tls.setPrivateKey(identity.key);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);

    auto http = std::make_unique<HttpServer>([this](const HttpRequest &r) { return handle(r); }, kMaxChunk);
    // Only signed-in phones may send bodies (uploads, text).
    http->setBodyGate([this](const HttpRequest &r) { return authorized(r); });
    QString listenError;
    if (!http->listen(tls, QHostAddress::AnyIPv4, m_options.port, &listenError)) {
        *error = QStringLiteral("Cannot listen on HTTPS port %1: %2").arg(m_options.port).arg(listenError);
        return false;
    }
    m_port = http->port();
    m_http = std::move(http);
    qCInfo(lcPhone) << "Phone access on HTTPS port" << m_port;
    return true;
}

void PhoneServer::stop()
{
    m_http.reset();
    m_token.clear();
    m_tokenTimer.stop();
    revokeSessions();
}

QString PhoneServer::newAccessToken()
{
    m_token = randomHex(16);
    m_tokenExpires = QDateTime::currentDateTimeUtc().addMSecs(kTokenValidityMs);
    m_tokenTimer.start(kTokenValidityMs);
    return m_token;
}

QStringList PhoneServer::accessUrls(const QString &token) const
{
    QStringList urls;
    for (const QString &ip : lanAddresses())
        urls << QStringLiteral("https://%1:%2/?t=%3").arg(ip).arg(m_port).arg(token);
    return urls;
}

int PhoneServer::sessionCount() const
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return int(std::count_if(m_sessions.begin(), m_sessions.end(), [&](const Session &s) { return s.expires > now; }));
}

void PhoneServer::revokeSessions()
{
    if (m_sessions.isEmpty())
        return;
    m_sessions.clear();
    emit sessionsChanged();
}

// --- Helpers -----------------------------------------------------------------------------------

QString PhoneServer::sessionId(const HttpRequest &request) const
{
    // Repeated Cookie headers arrive joined with ", ".
    QByteArray cookies = request.header("cookie");
    cookies.replace(',', ';');
    for (const QByteArray &part : cookies.split(';')) {
        const QByteArray trimmed = part.trimmed();
        if (trimmed.startsWith(kCookie + '='))
            return QString::fromLatin1(trimmed.mid(kCookie.size() + 1));
    }
    return {};
}

bool PhoneServer::authorized(const HttpRequest &request) const
{
    const QString id = sessionId(request);
    if (id.isEmpty())
        return false;
    auto it = m_sessions.constFind(hashed(id));
    return it != m_sessions.constEnd() && it->expires > QDateTime::currentDateTimeUtc();
}

QString PhoneServer::resolve(const QString &relative, bool mustExist) const
{
    const QString root = QDir(m_options.folder()).canonicalPath();
    if (root.isEmpty())
        return {};
    QString rel = relative;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QStringList parts;
    for (const QString &part : rel.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".") || part == QLatin1String("..") || part.contains(QLatin1Char(':')))
            return {};
        parts << part;
    }
    const QString candidate = parts.isEmpty() ? root : QDir(root).filePath(parts.join(QLatin1Char('/')));
    if (!mustExist)
        return candidate;
    // Canonical path: symbolic links must not lead outside the shared folder either.
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    if (canonical.isEmpty() || !(canonical == root || canonical.startsWith(root + QLatin1Char('/'))))
        return {};
    return canonical;
}

QString PhoneServer::sanitizeFileName(const QString &name)
{
    QString out;
    for (const QChar c : name.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1)) {
        if (c.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(c))
            out += QLatin1Char('_');
        else
            out += c;
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')))
        out.chop(1); // Windows strips trailing dots
    if (out.isEmpty() || out == QLatin1String(".") || out == QLatin1String(".."))
        return {};
    return out.left(200);
}

// --- Routes ------------------------------------------------------------------------------------

HttpResponse PhoneServer::handle(const HttpRequest &r)
{
    using Handler = HttpResponse (PhoneServer::*)(const HttpRequest &);
    struct Route {
        const char *path;
        const char *method;
        Handler handler;
        bool guarded; // session cookie required
        bool files;   // reads or writes the shared folder
    };
    static const Route kRoutes[] = {
        {"/", "GET", &PhoneServer::serveIndex, false, false},
        {"/app.js", "GET", &PhoneServer::serveAsset, false, false},
        {"/app.css", "GET", &PhoneServer::serveAsset, false, false},
        {"/api/info", "GET", &PhoneServer::serveInfo, true, false},
        {"/api/list", "GET", &PhoneServer::serveList, true, true},
        {"/api/file", "GET", &PhoneServer::serveFile, true, true},
        {"/api/thumb", "GET", &PhoneServer::serveThumb, true, true},
        {"/api/upload", "PUT", &PhoneServer::receiveUpload, true, true},
        {"/api/text", "POST", &PhoneServer::receiveText, true, false},
        {"/api/logout", "POST", &PhoneServer::logout, false, false},
    };
    for (const Route &route : kRoutes) {
        if (r.path != QLatin1String(route.path))
            continue;
        if (r.method != route.method)
            return std::move(jsonError(405, QStringLiteral("Method not allowed")).header("Allow", route.method));
        if (route.guarded && !authorized(r))
            return jsonError(401, QStringLiteral("Scan the QR code again"));
        // Files are opened with the signed-in user's rights, never the service's: a link inside
        // the shared folder cannot lead anywhere that user could not go. A download keeps the file
        // opened here.
        std::shared_ptr<void> asUser;
        if (route.files && m_options.asUser) {
            asUser = m_options.asUser();
            if (!asUser)
                return jsonError(503, QStringLiteral("Nobody is signed in on the computer"));
        }
        return (this->*route.handler)(r);
    }
    return jsonError(404, QStringLiteral("Not found"));
}

HttpResponse PhoneServer::serveAsset(const HttpRequest &r)
{
    const bool js = r.path.endsWith(QLatin1String(".js"));
    return respond(200, resource(r.path.mid(1)), js ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8");
}

HttpResponse PhoneServer::serveInfo(const HttpRequest &)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), m_options.deviceName);
    return json(o);
}

HttpResponse PhoneServer::receiveText(const HttpRequest &r)
{
    const QString text = QString::fromUtf8(r.body.left(1024 * 1024));
    if (!text.isEmpty())
        emit textReceived(text);
    return json(QJsonObject{{QStringLiteral("ok"), true}});
}

HttpResponse PhoneServer::logout(const HttpRequest &r)
{
    if (m_sessions.remove(hashed(sessionId(r))))
        emit sessionsChanged();
    return json(QJsonObject{{QStringLiteral("ok"), true}});
}

HttpResponse PhoneServer::serveIndex(const HttpRequest &request)
{
    const QString token = request.queryValue(QStringLiteral("t"));
    if (!token.isEmpty()) {
        const bool valid = !m_token.isEmpty() && token.size() == m_token.size()
                           && QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256)
                                  == QCryptographicHash::hash(m_token.toUtf8(), QCryptographicHash::Sha256)
                           && QDateTime::currentDateTimeUtc() < m_tokenExpires;
        if (!valid)
            return html(QStringLiteral("locked.html"), 403);
        newAccessToken(); // single use: the next phone scans a new code
        emit accessTokenChanged();
        const QString id = randomHex(32);
        m_sessions.insert(hashed(id), Session{QDateTime::currentDateTimeUtc().addSecs(kSessionHours * 3600),
                                              request.remoteAddress.toString()});
        emit sessionsChanged();
        HttpResponse response = respond(303, {}, "text/plain");
        // Lax, not Strict: the scan opens the browser from the camera app, a cross-site navigation,
        // and browsers would then withhold a Strict cookie from the redirect below, showing the
        // "link expired" page. Lax still keeps it off cross-site POST/PUT and subresources.
        response.header("Set-Cookie", QStringLiteral("%1=%2; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=%3")
                                   .arg(QString::fromLatin1(kCookie), id)
                                   .arg(kSessionHours * 3600)
                                   .toLatin1());
        response.header("Location", "/"); // drop the token from the address bar
        return response;
    }
    if (!authorized(request))
        return html(QStringLiteral("locked.html"), 403);
    return html(QStringLiteral("index.html"));
}

HttpResponse PhoneServer::serveList(const HttpRequest &request)
{
    const QString dirPath = resolve(request.queryValue(QStringLiteral("path")));
    if (dirPath.isEmpty() || !QFileInfo(dirPath).isDir())
        return jsonError(404, QStringLiteral("No such folder"));
    QDir dir(dirPath);
    QJsonArray entries;
    const auto infos = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                         QDir::DirsFirst | QDir::Time);
    for (const QFileInfo &info : infos) {
        if (info.fileName().endsWith(QLatin1String(".part")) && info.fileName().startsWith(QLatin1Char('.')))
            continue; // uploads in progress
        QJsonObject e;
        e.insert(QStringLiteral("name"), info.fileName());
        e.insert(QStringLiteral("dir"), info.isDir());
        e.insert(QStringLiteral("size"), info.isDir() ? 0 : info.size());
        e.insert(QStringLiteral("mtime"), info.lastModified().toMSecsSinceEpoch());
        entries.append(e);
    }
    const QString root = QDir(m_options.folder()).canonicalPath();
    QJsonObject o;
    o.insert(QStringLiteral("path"), QDir(root).relativeFilePath(dirPath) == QLatin1String(".")
                                         ? QString()
                                         : QDir(root).relativeFilePath(dirPath));
    o.insert(QStringLiteral("entries"), entries);
    // The page polls the folder every few seconds: an unchanged listing costs a 304.
    HttpResponse response = json(o);
    const QByteArray etag =
        '"' + QCryptographicHash::hash(response.body, QCryptographicHash::Sha256).toHex().left(32) + '"';
    if (request.header("if-none-match") == etag)
        response = HttpResponse(304);
    response.header("ETag", etag);
    return response;
}

HttpResponse PhoneServer::serveFile(const HttpRequest &request)
{
    const QString path = resolve(request.queryValue(QStringLiteral("path")));
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return jsonError(404, QStringLiteral("No such file"));
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly))
        return jsonError(403, QStringLiteral("Cannot read the file"));
    const QString name = QFileInfo(path).fileName();
    const QMimeType type = QMimeDatabase().mimeTypeForFile(path);
    const bool open = opensInBrowser(type) && request.queryValue(QStringLiteral("dl")) != QLatin1String("1");
    // Downloads go as a neutral type: the browser saves them, it never renders them.
    HttpResponse response = respond(200, {}, open ? type.name().toLatin1() : QByteArray("application/octet-stream"));
    response.header("Content-Disposition", QByteArray(open ? "inline" : "attachment")
                                               + "; filename*=UTF-8''" + QUrl::toPercentEncoding(name));
    if (open && type.name() != QLatin1String("application/pdf")) // browsers refuse sandboxed PDFs
        response.header("Content-Security-Policy", "sandbox");
    response.stream = std::move(file); // streamed from disk
    return response;
}

HttpResponse PhoneServer::serveThumb(const HttpRequest &request)
{
    const QString path = resolve(request.queryValue(QStringLiteral("path")));
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return jsonError(404, QStringLiteral("No such file"));
    const QString key = path + QLatin1Char('|') + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    if (const QByteArray *cached = m_thumbs.object(key))
        return respond(200, *cached, "image/jpeg");
    QImageReader reader(path);
    reader.setAutoTransform(true); // EXIF orientation of phone photos
    const QSize size = reader.size();
    if (size.isValid())
        reader.setScaledSize(size.scaled(320, 320, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return jsonError(415, QStringLiteral("Not an image"));
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 75);
    m_thumbs.insert(key, new QByteArray(jpeg), int(jpeg.size()));
    return respond(200, jpeg, "image/jpeg");
}

HttpResponse PhoneServer::receiveUpload(const HttpRequest &request)
{
    const QString dirPath = resolve(request.queryValue(QStringLiteral("path")));
    const QString name = sanitizeFileName(request.queryValue(QStringLiteral("name")));
    bool okOffset = false, okTotal = false;
    const qint64 offset = request.queryValue(QStringLiteral("offset")).toLongLong(&okOffset);
    const qint64 total = request.queryValue(QStringLiteral("total")).toLongLong(&okTotal);
    const QByteArray &chunk = request.body;
    if (dirPath.isEmpty() || !QFileInfo(dirPath).isDir() || name.isEmpty() || !okOffset || !okTotal || offset < 0
        || total < 0 || total > kMaxUpload || chunk.size() > kMaxChunk || offset + chunk.size() > total)
        return jsonError(400, QStringLiteral("Invalid upload"));

    // Chunks are appended to a hidden temporary file, keyed by session and name, so an interrupted
    // upload can resume and a half-written file never shows up as a real one.
    const QString partName = QStringLiteral(".%1-%2.part").arg(hashed(sessionId(request) + name).left(16), name.left(80));
    const QString partPath = QDir(dirPath).filePath(partName);
    QFile part(partPath);
    const qint64 have = part.exists() ? part.size() : 0;
    if (offset != have) {
        QJsonObject o;
        o.insert(QStringLiteral("error"), QStringLiteral("Unexpected offset"));
        o.insert(QStringLiteral("expected"), have);
        return json(o, 409);
    }
    if (!part.open(QIODevice::Append) || part.write(chunk) != chunk.size())
        return jsonError(507, QStringLiteral("Cannot write the file"));
    part.close();
#ifdef Q_OS_WIN
    if (offset == 0)
        SetFileAttributesW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(partPath).utf16()),
                           FILE_ATTRIBUTE_HIDDEN);
#endif

    QJsonObject o;
    if (offset + chunk.size() < total) {
        o.insert(QStringLiteral("received"), offset + chunk.size());
        return json(o);
    }
    // Complete: give it its final name without overwriting anything.
    const QFileInfo wanted(QDir(dirPath).filePath(name));
    QString finalPath = wanted.absoluteFilePath();
    for (int n = 2; QFileInfo::exists(finalPath); ++n) {
        const QString base = wanted.completeBaseName();
        const QString suffix = wanted.suffix().isEmpty() ? QString() : QLatin1Char('.') + wanted.suffix();
        finalPath = QDir(dirPath).filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix));
    }
#ifdef Q_OS_WIN
    SetFileAttributesW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(partPath).utf16()),
                       FILE_ATTRIBUTE_NORMAL);
#endif
    if (!QFile::rename(partPath, finalPath))
        return jsonError(500, QStringLiteral("Cannot finish the upload"));
    qCInfo(lcPhone) << "Received a file from a phone";
    emit fileUploaded(finalPath);
    o.insert(QStringLiteral("done"), true);
    o.insert(QStringLiteral("name"), QFileInfo(finalPath).fileName());
    return json(o);
}

} // namespace df::web
