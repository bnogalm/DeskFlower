#pragma once

// HTTPS web app for phones on the local network: browse, download and upload files in the shared
// folder, and send text to the computer's clipboard.
//
// Access: the agent shows a QR code with https://<lan ip>:<port>/?t=<token>. The token is single
// use and valid for 10 minutes, then replaced (accessTokenChanged); it is exchanged for a session
// cookie (HttpOnly, Secure, SameSite=Lax, 12 h). Sessions can be revoked from the agent.
//
// API (session cookie required):
//   GET  /api/info                         {name}
//   GET  /api/list?path=<rel>              {path, entries: [{name, dir, size, mtime}]}
//   GET  /api/file?path=<rel>[&dl=1]       file content (streamed from disk)
//   GET  /api/thumb?path=<rel>             JPEG preview of an image
//   PUT  /api/upload?path=<dir>&name=<n>&offset=<o>&total=<t>   one chunk (<= 8 MB) of a file
//   POST /api/text                         UTF-8 text for the clipboard
//   POST /api/logout
// Paths are relative to the shared folder; anything resolving outside it is rejected.

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

namespace df::web {

class HttpServer;
struct HttpRequest;
struct HttpResponse;

class PhoneServer : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kDefaultPort = 24802;
    static constexpr qint64 kMaxChunk = 8 * 1024 * 1024;
    static constexpr qint64 kMaxUpload = 8LL * 1024 * 1024 * 1024;
    static constexpr int kTokenValidityMs = 10 * 60 * 1000;
    static constexpr int kSessionHours = 12;

    struct Options {
        QString configDir;                // certificate storage
        quint16 port = kDefaultPort;      // 0: any free port
        std::function<QString()> folder;  // shared folder (absolute)
        QString deviceName;
        // Optional: the shared folder is read and written with the rights this guard gives while
        // it lives (a service running as LocalSystem acts as the signed-in user). A null guard
        // refuses the request.
        std::function<std::shared_ptr<void>()> asUser;
    };

    explicit PhoneServer(Options options, QObject *parent = nullptr);
    ~PhoneServer() override;

    bool start(QString *error);
    void stop();
    bool isRunning() const { return m_http != nullptr; }
    quint16 port() const { return m_port; }

    // New single-use access token (replaces the previous one) and the URLs that carry it.
    QString newAccessToken();
    QString accessToken() const { return m_token; }
    QStringList accessUrls(const QString &token) const;
    int sessionCount() const;
    void revokeSessions();

    // IPv4 addresses for the QR code: interfaces with a default gateway first (the network phones
    // are on), then other Wi-Fi/Ethernet ones; virtual adapters are skipped.
    static QStringList lanAddresses();
    // A single file name for an uploaded file: no separators, no characters Windows rejects.
    static QString sanitizeFileName(const QString &name);

signals:
    void textReceived(const QString &text);
    void fileUploaded(const QString &path);
    void sessionsChanged();
    // The access token was used or expired and has been replaced: the QR code must be updated.
    void accessTokenChanged();

private:
    struct Session {
        QDateTime expires;
        QString client;
    };

    HttpResponse handle(const HttpRequest &request);
    bool authorized(const HttpRequest &request) const;
    QString sessionId(const HttpRequest &request) const;
    // Absolute path inside the shared folder, or empty if the relative path is unsafe.
    QString resolve(const QString &relative, bool mustExist = true) const;

    HttpResponse serveIndex(const HttpRequest &request);
    HttpResponse serveAsset(const HttpRequest &request);
    HttpResponse serveInfo(const HttpRequest &request);
    HttpResponse serveList(const HttpRequest &request);
    HttpResponse serveFile(const HttpRequest &request);
    HttpResponse serveThumb(const HttpRequest &request);
    HttpResponse receiveUpload(const HttpRequest &request);
    HttpResponse receiveText(const HttpRequest &request);
    HttpResponse logout(const HttpRequest &request);

    Options m_options;
    std::unique_ptr<HttpServer> m_http;
    quint16 m_port = 0;
    QString m_token;
    QDateTime m_tokenExpires;
    QTimer m_tokenTimer;
    QHash<QString, Session> m_sessions; // key: SHA-256 of the cookie value
    QCache<QString, QByteArray> m_thumbs{32 * 1024 * 1024}; // cost in bytes
};

} // namespace df::web
