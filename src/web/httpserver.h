#pragma once

// Minimal HTTPS/1.1 server for the phone web app, on Qt Network's QSslServer (LGPLv3). Qt HTTP
// Server is GPLv3-only and cannot ship with Deskflower (THIRD-PARTY-NOTICES.md).
//
// Scope: what a browser on the local network needs. Persistent connections, one request at a time
// per connection, Content-Length bodies (no chunked uploads), responses from memory or streamed
// from a device. Limits on header size, body size, connections and idle time protect the service.

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QIODevice>
#include <QObject>
#include <QSslConfiguration>
#include <QUrlQuery>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class QSslServer;
class QSslSocket;
class QTimer;

namespace df::web {

struct HttpRequest {
    QByteArray method;               // "GET", "PUT"...
    QString path;                    // decoded, without the query
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers; // lower-case names; repeated headers joined with ", "
    QByteArray body;
    QHostAddress remoteAddress;

    QByteArray header(const QByteArray &lowerName) const { return headers.value(lowerName); }
    QString queryValue(const QString &key) const { return query.queryItemValue(key, QUrl::FullyDecoded); }
};

struct HttpResponse {
    int status = 200;
    std::vector<std::pair<QByteArray, QByteArray>> headers;
    QByteArray body;
    std::unique_ptr<QIODevice> stream; // when set, sent instead of body (open, sequential reads)

    HttpResponse() = default;
    HttpResponse(int status, QByteArray body = {}) : status(status), body(std::move(body)) {}
    HttpResponse &header(QByteArray name, QByteArray value)
    {
        headers.emplace_back(std::move(name), std::move(value));
        return *this;
    }
};

class HttpServer : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    static constexpr int kMaxHeaderBytes = 16 * 1024;
    static constexpr int kMaxConnections = 64;
    static constexpr int kIdleTimeoutMs = 60 * 1000;

    // maxBodyBytes: larger requests are answered with 413.
    explicit HttpServer(Handler handler, qint64 maxBodyBytes, QObject *parent = nullptr);
    ~HttpServer() override;

    bool listen(const QSslConfiguration &tls, const QHostAddress &address, quint16 port, QString *error);
    quint16 port() const;

private:
    struct Connection;

    void onNewConnection();
    void processInput(Connection *c);
    // linger: closing is left to the caller (a refused body is being drained).
    void send(Connection *c, HttpResponse response, bool keepAlive, bool linger = false);
    void pump(Connection *c);
    void drop(Connection *c);

    Handler m_handler;
    qint64 m_maxBody;
    QSslServer *m_server = nullptr;
    QHash<QSslSocket *, Connection *> m_connections;
};

const char *reasonPhrase(int status);

} // namespace df::web
