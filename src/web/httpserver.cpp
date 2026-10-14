#include "httpserver.h"

#include <QLoggingCategory>
#include <QSslServer>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>

Q_LOGGING_CATEGORY(lcHttp, "deskflower.phone.http")

namespace df::web {

namespace {
constexpr qint64 kStreamChunk = 64 * 1024;
constexpr qint64 kStreamHighWater = 256 * 1024; // queued bytes before waiting for bytesWritten
constexpr int kHandshakeTimeoutMs = 10 * 1000;
// A refused body (413) up to this size is read and dropped, so the client sees the answer instead
// of a reset connection; beyond it the connection is cut.
constexpr qint64 kMaxDiscardBytes = 64 * 1024 * 1024;
} // namespace

const char *reasonPhrase(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    default: return "Unknown";
    }
}

struct HttpServer::Connection {
    QSslSocket *socket = nullptr;
    QTimer *idle = nullptr;
    QTimer *headers = nullptr; // a request's headers must arrive in time (slow clients hold slots)
    QHostAddress address;
    bool admitted = false; // the gate accepted the body of the request being read
    QByteArray input;
    std::unique_ptr<QIODevice> stream; // response being streamed
    bool closeAfterResponse = false;
    bool sentContinue = false;
    qint64 discard = 0; // bytes of a refused request still to drop before closing
};

HttpServer::HttpServer(Handler handler, qint64 maxBodyBytes, QObject *parent)
    : QObject(parent), m_handler(std::move(handler)), m_maxBody(maxBodyBytes)
{
}

HttpServer::~HttpServer()
{
    const auto connections = m_connections.values();
    for (Connection *c : connections)
        drop(c);
}

bool HttpServer::listen(const QSslConfiguration &tls, const QHostAddress &address, quint16 port, QString *error)
{
    auto *server = new QSslServer(this);
    server->setSslConfiguration(tls);
    server->setHandshakeTimeout(kHandshakeTimeoutMs);
    server->setMaxPendingConnections(16); // TLS handshakes in progress count too
    // Handshake failures are routine: phones refuse the self-signed certificate until the user
    // accepts it.
    connect(server, &QSslServer::errorOccurred, this, [](QSslSocket *, QAbstractSocket::SocketError e) {
        qCDebug(lcHttp) << "Connection error" << e;
    });
    connect(server, &QSslServer::peerVerifyError, this, [](QSslSocket *, const QSslError &) {});
    connect(server, &QTcpServer::pendingConnectionAvailable, this, &HttpServer::onNewConnection);
    if (!server->listen(address, port)) {
        *error = server->errorString();
        delete server;
        return false;
    }
    m_server = server;
    return true;
}

quint16 HttpServer::port() const { return m_server ? m_server->serverPort() : 0; }

void HttpServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto *socket = qobject_cast<QSslSocket *>(m_server->nextPendingConnection());
        if (!socket)
            continue;
        const QHostAddress address = socket->peerAddress();
        const auto fromAddress = std::count_if(m_connections.cbegin(), m_connections.cend(),
                                               [&address](const Connection *c) { return c->address == address; });
        if (m_connections.size() >= kMaxConnections || fromAddress >= kMaxConnectionsPerAddress) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto *c = new Connection;
        c->socket = socket;
        c->address = address;
        c->idle = new QTimer(socket);
        c->idle->setSingleShot(true);
        c->idle->setInterval(kIdleTimeoutMs);
        c->headers = new QTimer(socket);
        c->headers->setSingleShot(true);
        c->headers->setInterval(kHeaderTimeoutMs);
        m_connections.insert(socket, c);
        connect(c->idle, &QTimer::timeout, this, [this, c] { drop(c); });
        connect(c->headers, &QTimer::timeout, this, [this, c] {
            qCDebug(lcHttp) << "Request headers too slow from" << c->address;
            drop(c);
        });
        connect(socket, &QSslSocket::readyRead, this, [this, c] {
            c->idle->start();
            c->input += c->socket->readAll();
            processInput(c);
        });
        connect(socket, &QSslSocket::bytesWritten, this, [this, c] {
            c->idle->start();
            pump(c);
        });
        connect(socket, &QSslSocket::disconnected, this, [this, c] { drop(c); });
        c->idle->start();
        if (socket->bytesAvailable() > 0) {
            c->input += socket->readAll();
            processInput(c);
        }
    }
}

void HttpServer::processInput(Connection *c)
{
    if (c->discard > 0) {
        const qint64 n = std::min<qint64>(c->discard, c->input.size());
        c->input.remove(0, n);
        c->discard -= n;
        if (c->discard == 0)
            c->socket->disconnectFromHost();
        return;
    }
    // One request at a time: pipelined ones wait in the buffer until the response is out.
    while (!c->stream && !c->closeAfterResponse) {
        const qsizetype headerEnd = c->input.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (c->input.size() > kMaxHeaderBytes)
                send(c, HttpResponse(431), false);
            else if (!c->input.isEmpty() && !c->headers->isActive())
                c->headers->start();
            return;
        }
        c->headers->stop();
        if (headerEnd > kMaxHeaderBytes)
            return send(c, HttpResponse(431), false);

        const QList<QByteArray> lines = c->input.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() != 3 || !requestLine.at(2).startsWith("HTTP/1."))
            return send(c, HttpResponse(requestLine.size() == 3 ? 505 : 400), false);

        HttpRequest request;
        request.method = requestLine.at(0);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                return send(c, HttpResponse(400), false);
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            auto it = request.headers.find(name);
            if (it == request.headers.end())
                request.headers.insert(name, value);
            else
                *it += ", " + value;
        }

        if (request.headers.contains("transfer-encoding"))
            return send(c, HttpResponse(501), false); // browsers send Content-Length for our uploads
        qint64 length = 0;
        if (request.headers.contains("content-length")) {
            bool ok = false;
            length = request.header("content-length").toLongLong(&ok);
            if (!ok || length < 0)
                return send(c, HttpResponse(400), false);
        }
        const qint64 total = headerEnd + 4 + length;
        const QByteArray target = requestLine.at(1);
        const qsizetype q = target.indexOf('?');
        request.path = QUrl::fromPercentEncoding(q < 0 ? target : target.left(q));
        if (q >= 0)
            request.query = QUrlQuery(QString::fromUtf8(target.mid(q + 1)));
        request.remoteAddress = c->address;
        if (length > 0 && !c->admitted) {
            if (m_gate && !m_gate(request))
                return send(c, HttpResponse(401), false);
            c->admitted = true;
        }
        if (length > m_maxBody) {
            if (length > kMaxDiscardBytes)
                return send(c, HttpResponse(413), false);
            send(c, HttpResponse(413), false, /*linger=*/true);
            c->discard = total;
            return processInput(c); // drops what is already buffered
        }
        if (c->input.size() < total) {
            if (!c->sentContinue && request.header("expect").compare("100-continue", Qt::CaseInsensitive) == 0) {
                c->sentContinue = true;
                c->socket->write("HTTP/1.1 100 Continue\r\n\r\n");
            }
            return; // wait for the rest of the body
        }
        c->sentContinue = false;
        c->admitted = false;

        request.body = c->input.mid(headerEnd + 4, length);
        c->input.remove(0, total);

        const QByteArray connection = request.header("connection").toLower();
        const bool keepAlive = requestLine.at(2) == "HTTP/1.1" ? !connection.contains("close")
                                                               : connection.contains("keep-alive");
        send(c, m_handler(request), keepAlive);
    }
}

void HttpServer::send(Connection *c, HttpResponse response, bool keepAlive, bool linger)
{
    const qint64 length = response.stream ? response.stream->size() : response.body.size();
    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reasonPhrase(response.status) + "\r\n";
    for (const auto &[name, value] : response.headers)
        head += name + ": " + value + "\r\n";
    head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    head += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    c->closeAfterResponse = !keepAlive;
    c->socket->write(head);
    if (response.stream) {
        c->stream = std::move(response.stream);
        pump(c);
        return;
    }
    c->socket->write(response.body);
    if (!keepAlive && !linger)
        c->socket->disconnectFromHost(); // after the queued bytes
}

void HttpServer::pump(Connection *c)
{
    if (!c->stream)
        return;
    while (c->socket->bytesToWrite() < kStreamHighWater) {
        const QByteArray chunk = c->stream->read(kStreamChunk);
        if (chunk.isEmpty()) {
            c->stream.reset();
            if (c->closeAfterResponse)
                c->socket->disconnectFromHost();
            else
                QTimer::singleShot(0, this, [this, socket = c->socket] { // next pipelined request
                    if (Connection *still = m_connections.value(socket))
                        processInput(still);
                });
            return;
        }
        c->socket->write(chunk);
    }
}

void HttpServer::drop(Connection *c)
{
    if (!m_connections.remove(c->socket))
        return;
    c->idle->stop();
    c->headers->stop();
    c->socket->disconnect(this);
    c->socket->abort();
    c->socket->deleteLater(); // also deletes the idle timer
    delete c;
}

} // namespace df::web
