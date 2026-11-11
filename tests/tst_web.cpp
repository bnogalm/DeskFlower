#include "certificate.h"
#include "httpserver.h"
#include "phoneserver.h"
#include "webtestutil.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTest>
#include <QUrlQuery>
#include <functional>
#include <memory>

using namespace df::web;
using df::test::Reply;
using df::test::pattern;
using df::test::request;

namespace {
QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A raw TLS client, for what QNetworkAccessManager hides: pipelining, not reading, idling.
std::unique_ptr<QSslSocket> rawClient(quint16 port)
{
    auto socket = std::make_unique<QSslSocket>();
    socket->setPeerVerifyMode(QSslSocket::VerifyNone);
    QObject::connect(socket.get(), &QSslSocket::sslErrors, socket.get(), [s = socket.get()] { s->ignoreSslErrors(); });
    socket->connectToHostEncrypted(QStringLiteral("127.0.0.1"), port);
    return socket;
}

// Reads from the client until `done` says so or the time is up (the server runs on this thread,
// so blocking waits are not an option).
bool readUntil(QSslSocket &socket, QByteArray &into, int timeoutMs, const std::function<bool()> &done)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QTest::qWait(5);
        into += socket.readAll();
    }
    return true;
}
} // namespace

class TestWeb : public QObject
{
    Q_OBJECT

    QTemporaryDir m_config;
    QTemporaryDir m_folder;
    std::unique_ptr<PhoneServer> m_server;
    QNetworkAccessManager m_phone; // keeps the session cookie

    QUrl url(const QString &path, const QUrlQuery &query = {}) const
    {
        QUrl u(QStringLiteral("https://127.0.0.1:%1%2").arg(m_server->port()).arg(path));
        u.setQuery(query);
        return u;
    }

    // The phone's session, as a request line for a raw client.
    QByteArray cookieHeader() const
    {
        QByteArray header = "Cookie: ";
        for (const QNetworkCookie &c : m_phone.cookieJar()->cookiesForUrl(url("/")))
            header += c.toRawForm(QNetworkCookie::NameAndValueOnly) + "; ";
        return header + "\r\n";
    }

private slots:
    void initTestCase()
    {
        PhoneServer::Options o;
        o.configDir = m_config.path();
        o.port = 0;
        const QString folder = m_folder.path();
        o.folder = [folder] { return folder; };
        o.deviceName = QStringLiteral("Test PC");
        m_server = std::make_unique<PhoneServer>(o);
        QString error;
        QVERIFY2(m_server->start(&error), qPrintable(error));
        QVERIFY(QFile::exists(m_config.filePath("phone-cert.pem")));
    }

    void lockedWithoutAccess()
    {
        QNetworkAccessManager stranger;
        QCOMPARE(request(stranger, "GET", url("/")).status, 403);
        QCOMPARE(request(stranger, "GET", url("/api/list")).status, 401);
        QCOMPARE(request(stranger, "GET", url("/", QUrlQuery("t=00112233445566778899aabbccddeeff"))).status, 403);
    }

    void qrTokenGivesASessionOnce()
    {
        const QString token = m_server->newAccessToken();
        QVERIFY(m_server->accessUrls(token).isEmpty()
                || m_server->accessUrls(token).first().endsWith(QStringLiteral("/?t=") + token));
        QSignalSpy sessions(m_server.get(), &PhoneServer::sessionsChanged);
        QSignalSpy rotated(m_server.get(), &PhoneServer::accessTokenChanged);
        const Reply index = request(m_phone, "GET", url("/", QUrlQuery("t=" + token)));
        QCOMPARE(index.status, 200); // after the redirect that drops the token
        QVERIFY(index.body.contains("Upload files"));
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(m_server->sessionCount(), 1);
        // A new code replaces the used one, so the agent's QR keeps working for the next phone.
        QCOMPARE(rotated.size(), 1);
        QVERIFY(!m_server->accessToken().isEmpty() && m_server->accessToken() != token);

        const QByteArray csp = index.raw->rawHeader("Content-Security-Policy");
        QVERIFY(csp.contains("script-src 'self'"));
        QCOMPARE(index.raw->rawHeader("Referrer-Policy"), QByteArray("no-referrer"));

        // The same QR cannot be used twice.
        QNetworkAccessManager other;
        QCOMPARE(request(other, "GET", url("/", QUrlQuery("t=" + token))).status, 403);
        QCOMPARE(request(m_phone, "GET", url("/api/info")).json().value("name").toString(), QString("Test PC"));
    }

    // Phones open the camera only for a single kind in accept (several: the gallery), and a
    // picker without capture for files.
    void cameraButtonsOpenTheCamera()
    {
        const QByteArray page = request(m_phone, "GET", url("/")).body;
        static const QRegularExpression inputTag(QStringLiteral("<input[^>]*type=\"file\"[^>]*>"));
        static const QRegularExpression accept(QStringLiteral("accept=\"([^\"]*)\""));
        QStringList captures;
        bool plainPicker = false;
        for (auto it = inputTag.globalMatch(QString::fromUtf8(page)); it.hasNext();) {
            const QString tag = it.next().captured();
            const QString kinds = accept.match(tag).captured(1);
            if (tag.contains(QLatin1String("capture="))) {
                QVERIFY2(!kinds.isEmpty() && !kinds.contains(QLatin1Char(',')), qPrintable(tag));
                captures << kinds;
            } else {
                plainPicker = true;
            }
        }
        QCOMPARE(captures, QStringList({QStringLiteral("image/*"), QStringLiteral("video/*")}));
        QVERIFY(plainPicker);
    }

    void chunkedUploadWithoutOverwriting()
    {
        const QByteArray content = pattern(9 * 1024 * 1024 + 123);
        const qint64 chunk = 4 * 1024 * 1024;
        Reply last;
        for (qint64 off = 0; off < content.size(); off += chunk) {
            QUrlQuery q;
            q.addQueryItem("path", "");
            q.addQueryItem("name", "vídeo.mp4");
            q.addQueryItem("offset", QString::number(off));
            q.addQueryItem("total", QString::number(content.size()));
            last = request(m_phone, "PUT", url("/api/upload", q), content.mid(off, chunk));
            QCOMPARE(last.status, 200);
        }
        QVERIFY(last.json().value("done").toBool());
        QCOMPARE(readFile(m_folder.filePath("vídeo.mp4")), content);

        // Same name again: kept as "vídeo (2).mp4".
        QUrlQuery q("path=&name=v%C3%ADdeo.mp4&offset=0&total=3");
        const Reply second = request(m_phone, "PUT", url("/api/upload", q), "abc");
        QCOMPARE(second.json().value("name").toString(), QString("vídeo (2).mp4"));
        QCOMPARE(readFile(m_folder.filePath("vídeo (2).mp4")), QByteArray("abc"));

        // A chunk at the wrong offset is refused with the offset to resume from.
        const Reply part1 = request(m_phone, "PUT", url("/api/upload", QUrlQuery("name=r.bin&offset=0&total=10")), "12345");
        QCOMPARE(part1.status, 200);
        const Reply wrong = request(m_phone, "PUT", url("/api/upload", QUrlQuery("name=r.bin&offset=7&total=10")), "890");
        QCOMPARE(wrong.status, 409);
        QCOMPARE(wrong.json().value("expected").toInteger(), 5);
        QCOMPARE(request(m_phone, "PUT", url("/api/upload", QUrlQuery("name=r.bin&offset=5&total=10")), "67890").status,
                 200);
        QCOMPARE(readFile(m_folder.filePath("r.bin")), QByteArray("1234567890"));
    }

    void listDownloadAndThumbnails()
    {
        QDir(m_folder.path()).mkpath("photos");
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(m_folder.filePath("photos/beach.png")));

        const Reply root = request(m_phone, "GET", url("/api/list"));
        QCOMPARE(root.status, 200);
        const QJsonArray entries = root.json().value("entries").toArray();
        QVERIFY(entries.size() >= 3);
        QCOMPARE(entries.first().toObject().value("name").toString(), QString("photos")); // folders first
        for (const QJsonValue &e : entries)
            QVERIFY(!e.toObject().value("name").toString().endsWith(".part"));

        const Reply sub = request(m_phone, "GET", url("/api/list", QUrlQuery("path=photos")));
        QCOMPARE(sub.json().value("path").toString(), QString("photos"));

        const Reply file = request(m_phone, "GET", url("/api/file", QUrlQuery("path=photos/beach.png&dl=1")));
        QCOMPARE(file.status, 200);
        QCOMPARE(file.body, readFile(m_folder.filePath("photos/beach.png")));
        QVERIFY(file.raw->rawHeader("Content-Disposition").startsWith("attachment"));

        const Reply thumb = request(m_phone, "GET", url("/api/thumb", QUrlQuery("path=photos/beach.png")));
        QCOMPARE(thumb.status, 200);
        const QImage preview = QImage::fromData(thumb.body, "JPG");
        QCOMPARE(preview.size(), QSize(320, 240));
    }

    void onlySafeFilesOpenInTheBrowser()
    {
        // A page or a drawing in the shared folder would run as this site: always a download.
        for (const char *name : {"page.html", "drawing.svg"}) {
            QFile f(m_folder.filePath(QString::fromLatin1(name)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<svg xmlns='http://www.w3.org/2000/svg'><script>alert(1)</script></svg>");
            f.close();
            const Reply r = request(m_phone, "GET", url("/api/file", QUrlQuery(QStringLiteral("path=%1").arg(name))));
            QCOMPARE(r.status, 200);
            QVERIFY(r.raw->rawHeader("Content-Disposition").startsWith("attachment"));
            QCOMPARE(r.raw->rawHeader("Content-Type"), QByteArray("application/octet-stream"));
        }
        // A photo opens, sandboxed.
        const Reply photo = request(m_phone, "GET", url("/api/file", QUrlQuery("path=photos/beach.png")));
        QCOMPARE(photo.status, 200);
        QVERIFY(photo.raw->rawHeader("Content-Disposition").startsWith("inline"));
        QCOMPARE(photo.raw->rawHeader("Content-Type"), QByteArray("image/png"));
        QVERIFY2(photo.raw->rawHeader("Content-Security-Policy").contains("sandbox"),
                 photo.raw->rawHeader("Content-Security-Policy").constData());
    }

    void pathsStayInsideTheSharedFolder()
    {
        QTemporaryDir outside;
        QFile secret(outside.filePath("secret.txt"));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("top secret");
        secret.close();
        const QString rel = QDir(m_folder.path()).relativeFilePath(secret.fileName()); // "../xyz/secret.txt"

        for (const QString &path : {rel, QStringLiteral("photos/../../x"), QStringLiteral("C:/Windows/win.ini"),
                                    QStringLiteral("/etc/passwd")}) {
            QUrlQuery q;
            q.addQueryItem("path", path);
            const Reply r = request(m_phone, "GET", url("/api/file", q));
            QVERIFY2(r.status == 404 && !r.body.contains("top secret"), qPrintable(path));
        }
        // Upload names cannot escape either.
        const Reply evil = request(m_phone, "PUT", url("/api/upload", QUrlQuery("name=..%2F..%2Fevil.txt&offset=0&total=1")), "x");
        QCOMPARE(evil.status, 200);
        QVERIFY(QFile::exists(m_folder.filePath("evil.txt")));
        QCOMPARE(PhoneServer::sanitizeFileName("a<b>:c?.txt"), QString("a_b__c_.txt"));
        QVERIFY(PhoneServer::sanitizeFileName("..").isEmpty());
    }

    void textGoesToTheClipboard()
    {
        QSignalSpy text(m_server.get(), &PhoneServer::textReceived);
        QCOMPARE(request(m_phone, "POST", url("/api/text"), "hola desde el móvil").status, 200);
        QCOMPARE(text.size(), 1);
        QCOMPARE(text.first().at(0).toString(), QString("hola desde el móvil"));
    }

    void listingIsCheapToPoll()
    {
        auto list = [&](const QByteArray &tag) {
            QNetworkRequest req(url("/api/list"));
            if (!tag.isEmpty())
                req.setRawHeader("If-None-Match", tag);
            QNetworkReply *reply = m_phone.get(req);
            QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply] { reply->ignoreSslErrors(); });
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            const auto r = std::make_pair(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                                          reply->rawHeader("ETag"));
            reply->deleteLater();
            return r;
        };
        const auto first = list({});
        QCOMPARE(first.first, 200);
        QVERIFY(!first.second.isEmpty());
        QCOMPARE(list(first.second).first, 304); // unchanged
        QFile f(m_folder.filePath("new-on-the-computer.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hi");
        f.close();
        const auto changed = list(first.second);
        QCOMPARE(changed.first, 200); // a file appeared: the page redraws
        QVERIFY(changed.second != first.second);
        QVERIFY(QFile::remove(f.fileName()));
    }

    void strangersCannotSendBodies()
    {
        // Headers announcing a large upload, without a session: refused before any body is read.
        QSslSocket socket;
        socket.setPeerVerifyMode(QSslSocket::VerifyNone);
        socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), m_server->port());
        connect(&socket, &QSslSocket::sslErrors, &socket, [&socket] { socket.ignoreSslErrors(); });
        QTRY_VERIFY(socket.isEncrypted()); // the server runs on this thread: no blocking waits
        socket.write("PUT /api/upload?name=x.bin&offset=0&total=5000000 HTTP/1.1\r\nHost: x\r\n"
                     "Content-Length: 5000000\r\n\r\n");
        QByteArray answer;
        QTRY_VERIFY(!(answer += socket.readAll()).isEmpty());
        QVERIFY2(answer.startsWith("HTTP/1.1 401"), answer.constData());
        QTRY_VERIFY(socket.state() == QAbstractSocket::UnconnectedState);
    }

    void protocolLimits()
    {
        QCOMPARE(request(m_phone, "DELETE", url("/api/list")).status, 405);
        QCOMPARE(request(m_phone, "GET", url("/nothing-here")).status, 404);
        // Larger than one upload chunk: refused before it is read into memory.
        const QByteArray tooBig(int(PhoneServer::kMaxChunk) + 1, 'x');
        QCOMPARE(request(m_phone, "PUT", url("/api/upload", QUrlQuery("name=big.bin&offset=0&total=9000000")), tooBig)
                     .status,
                 413);
        QVERIFY(!QFile::exists(m_folder.filePath("big.bin")));
        // The connection is usable again afterwards (a new one if the server closed it).
        QCOMPARE(request(m_phone, "GET", url("/api/info")).status, 200);
    }

    void pipelinedRequestsAreAnsweredAtTheClientsPace()
    {
        // A client that pipelines requests and never reads the answers must not make the
        // server queue them all: responses are produced as the client takes them (the kernel
        // slows the sender down), and every request is answered once it does read.
        TlsIdentity identity;
        QString error;
        const QStringList ips{QStringLiteral("127.0.0.1")};
        const QStringList names{QStringLiteral("localhost")};
        QVERIFY2(loadOrCreateTlsIdentity(m_config.path(), ips, names, &identity, &error), qPrintable(error));
        QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
        tls.setLocalCertificate(identity.certificate);
        tls.setPrivateKey(identity.key);
        tls.setPeerVerifyMode(QSslSocket::VerifyNone);
        int handled = 0;
        const QByteArray body(64 * 1024, 'b');
        HttpServer server([&handled, &body](const HttpRequest &) { ++handled; return HttpResponse(200, body); }, 1024);
        QVERIFY2(server.listen(tls, QHostAddress::LocalHost, 0, &error), qPrintable(error));

        auto client = rawClient(server.port());
        QTRY_VERIFY(client->isEncrypted());
        client->setReadBufferSize(1024); // takes nothing
        const int n = 2000;
        const QByteArray request = "GET /x HTTP/1.1\r\nHost: x\r\n\r\n";
        QByteArray requests;
        for (int i = 0; i < n; ++i)
            requests += request;
        client->write(requests);
        QTest::qWait(2000);
        QVERIFY2(handled > 0 && handled < n / 4, qPrintable(QString::number(handled)));

        client->setReadBufferSize(0);
        const QByteArray head = "HTTP/1.1 200 OK\r\nContent-Length: 65536\r\nConnection: keep-alive\r\n\r\n";
        const qint64 expected = qint64(n) * (head.size() + body.size());
        qint64 received = 0;
        QByteArray first;
        QElapsedTimer t;
        t.start();
        while (received < expected && t.elapsed() < 60000) {
            QTest::qWait(5);
            const QByteArray chunk = client->readAll();
            if (first.size() < head.size())
                first += chunk.left(head.size());
            received += chunk.size();
        }
        QCOMPARE(received, expected);
        QVERIFY(first.startsWith(head));
        QCOMPARE(handled, n);
    }

    void aGrowingFileDoesNotExceedItsAnnouncedLength()
    {
        // The phone taps a file still being copied into the folder: the body stops at the
        // length announced, so the next response on the connection starts where the browser
        // expects it instead of inside the surplus.
        const QByteArray part = pattern(8 * 1024 * 1024);
        QFile f(m_folder.filePath("growing.bin"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(part);
        f.close();

        auto client = rawClient(m_server->port());
        QTRY_VERIFY(client->isEncrypted());
        client->setReadBufferSize(16 * 1024); // a slow phone: the server waits with most of it
        client->write("GET /api/file?path=growing.bin&dl=1 HTTP/1.1\r\nHost: x\r\n" + cookieHeader() + "\r\n");
        QTRY_VERIFY(client->bytesAvailable() > 0);
        QTest::qWait(300);
        QVERIFY(f.open(QIODevice::Append)); // the copy goes on
        f.write(part);
        f.close();

        client->setReadBufferSize(0);
        QByteArray response;
        qint64 bodyStart = -1;
        qint64 length = -1;
        QVERIFY(readUntil(*client, response, 30000, [&] {
            if (bodyStart < 0) {
                const qsizetype headerEnd = response.indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return false;
                static const QRegularExpression contentLength(QStringLiteral("Content-Length: (\\d+)"));
                length = contentLength.match(QString::fromLatin1(response.left(headerEnd))).captured(1).toLongLong();
                bodyStart = headerEnd + 4;
            }
            return response.size() >= bodyStart + length;
        }));
        QCOMPARE(length, qint64(part.size()));
        QCOMPARE(response.mid(bodyStart, part.size()), part);

        // The connection is in step: the next request's answer follows at once.
        client->write("GET /api/info HTTP/1.1\r\nHost: x\r\n" + cookieHeader() + "\r\n");
        QByteArray next = response.mid(bodyStart + length);
        QVERIFY(readUntil(*client, next, 5000, [&] { return next.contains("\r\n\r\n"); }));
        QVERIFY2(next.startsWith("HTTP/1.1 200"), next.left(40).constData());
        QVERIFY(QFile::remove(f.fileName()));
    }

    void strangersAreDisconnectedWhenIdle()
    {
        // A connection whose requests carry no session is closed after a few idle seconds, so
        // that strangers cannot hold the slots with cheap requests; a phone's keeps its minute.
        auto stranger = rawClient(m_server->port());
        auto phone = rawClient(m_server->port());
        QTRY_VERIFY(stranger->isEncrypted() && phone->isEncrypted());
        stranger->write("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        phone->write("GET /api/info HTTP/1.1\r\nHost: x\r\n" + cookieHeader() + "\r\n");
        QByteArray answer;
        QVERIFY(readUntil(*stranger, answer, 5000, [&] { return answer.contains("\r\n\r\n"); }));
        QVERIFY2(answer.startsWith("HTTP/1.1 403"), answer.left(40).constData());
        answer.clear();
        QVERIFY(readUntil(*phone, answer, 5000, [&] { return answer.contains("\r\n\r\n"); }));
        QVERIFY2(answer.startsWith("HTTP/1.1 200"), answer.left(40).constData());

        QElapsedTimer t;
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(stranger->state() == QAbstractSocket::UnconnectedState, 9000);
        QVERIFY2(t.elapsed() >= 3000, qPrintable(QString::number(t.elapsed()))); // not at once: idle
        QCOMPARE(phone->state(), QAbstractSocket::ConnectedState);
        QTest::qWait(2000); // well past the stranger's idle time in total
        QCOMPARE(phone->state(), QAbstractSocket::ConnectedState);
        phone->write("GET /api/info HTTP/1.1\r\nHost: x\r\n" + cookieHeader() + "\r\n");
        answer.clear();
        QVERIFY(readUntil(*phone, answer, 5000, [&] { return answer.contains("\r\n\r\n"); }));
        QVERIFY2(answer.startsWith("HTTP/1.1 200"), answer.left(40).constData());
    }

    void revokingDisconnectsPhones()
    {
        m_server->revokeSessions();
        QCOMPARE(m_server->sessionCount(), 0);
        QCOMPARE(request(m_phone, "GET", url("/api/list")).status, 401);
    }
};

QTEST_GUILESS_MAIN(TestWeb)
#include "tst_web.moc"
