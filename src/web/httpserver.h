#pragma once

// Minimal HTTPS/1.1 server for the phone web app, on Qt Network's QSslServer (LGPLv3). Qt HTTP
// Server is GPLv3-only and cannot ship with Deskflower (THIRD-PARTY-NOTICES.md).
//
// Scope: what a browser on the local network needs. Persistent connections, one request at a time
// per connection, Content-Length bodies (no chunked uploads), responses from memory or streamed
// from a device. Limits on header size, body size, connections (in total and per address), header
// time and idle time protect the service, and a request body is only read once the gate accepts
// the request's headers (the phone server: a valid session), so strangers cannot make it buffer
// uploads.
//
// Memory is bounded by the client's own pace: requests are read from the socket only while the
// answers to the previous ones are going out (kStreamHighWater queued at most) and no response is
// being streamed, so a client that pipelines requests without reading, or keeps sending during a
// download, is slowed down by the kernel (the socket's read buffer is capped) instead of filling
// this process. A streamed body is cut at the announced length and the connection is closed if
// the device ends early. Connections whose requests carry no valid session (the gate) are closed
// after a short idle time, and when the table is full the longest idle one makes room.

#include <QByteArray>
#include <QElapsedTimer>
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
    static constexpr int kMaxConnectionsPerAddress = 16;
    static constexpr int kIdleTimeoutMs = 60 * 1000;
    static constexpr int kUnauthenticatedIdleMs = 5 * 1000; // no valid session on the last request
    static constexpr int kHeaderTimeoutMs = 15 * 1000; // first byte of a request -> end of its headers

    // Decides from the headers (no body yet) whether a request carries a valid session: one with
    // a body may send it only then (refused ones get 401 and the connection is closed), and a
    // connection whose last request had none is closed after kUnauthenticatedIdleMs.
    using Gate = std::function<bool(const HttpRequest &)>;

    // maxBodyBytes: larger requests are answered with 413.
    explicit HttpServer(Handler handler, qint64 maxBodyBytes, QObject *parent = nullptr);
    void setBodyGate(Gate gate) { m_gate = std::move(gate); }
    ~HttpServer() override;

    bool listen(const QSslConfiguration &tls, const QHostAddress &address, quint16 port, QString *error);
    quint16 port() const;

private:
    struct Connection;

    void onNewConnection();
    static qint64 queued(const Connection *c);
    static bool canRead(const Connection *c);
    void resume(Connection *c);
    void processInput(Connection *c);
    // linger: closing is left to the caller (a refused body is being drained).
    void send(Connection *c, HttpResponse response, bool keepAlive, bool linger = false);
    void pump(Connection *c);
    void drop(Connection *c);
    void dropLater(Connection *c);
    void touch(Connection *c);

    Handler m_handler;
    Gate m_gate;
    qint64 m_maxBody;
    QSslServer *m_server = nullptr;
    QHash<QSslSocket *, Connection *> m_connections;
    QElapsedTimer m_clock; // for Connection::lastActivity
};

const char *reasonPhrase(int status);

} // namespace df::web
