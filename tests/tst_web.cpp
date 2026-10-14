#include "phoneserver.h"

#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSslSocket>
#include <QTest>
#include <QUrlQuery>

using namespace df::web;

namespace {
struct Reply {
    int status = 0;
    QByteArray body;
    QNetworkReply *raw = nullptr;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

// Self-signed certificate: the client accepts it, like a phone after the warning.
Reply request(QNetworkAccessManager &nam, const QByteArray &verb, const QUrl &url, const QByteArray &body = {})
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QBuffer *buffer = nullptr;
    if (!body.isNull()) {
        buffer = new QBuffer;
        buffer->setData(body);
        buffer->open(QIODevice::ReadOnly);
    }
    QNetworkReply *reply = nam.sendCustomRequest(req, verb, buffer);
    if (buffer)
        buffer->setParent(reply);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply] { reply->ignoreSslErrors(); });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    Reply r{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll(), reply};
    reply->deleteLater();
    return r;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray pattern(int size)
{
    QByteArray b(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        b[i] = char((i * 7 + i / 977) & 0xFF);
    return b;
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

    void revokingDisconnectsPhones()
    {
        m_server->revokeSessions();
        QCOMPARE(m_server->sessionCount(), 0);
        QCOMPARE(request(m_phone, "GET", url("/api/list")).status, 401);
    }
};

QTEST_GUILESS_MAIN(TestWeb)
#include "tst_web.moc"
