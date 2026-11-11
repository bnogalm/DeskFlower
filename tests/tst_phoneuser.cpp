// Phone sessions belong to the Windows user who granted them (they must not follow a fast user
// switch to another user's files), and /api/file serves single byte ranges (206) for media
// playback and seeking. The other phone-server tests live in tst_web.cpp.

#include "phoneserver.h"
#include "webtestutil.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

using namespace df::web;
using df::test::Reply;
using df::test::pattern;
using df::test::request;

class TestPhoneUser : public QObject
{
    Q_OBJECT

    QTemporaryDir m_config;
    QTemporaryDir m_folder;
    QString m_user = QStringLiteral("S-1-5-21-user-A");
    std::unique_ptr<PhoneServer> m_server;
    QNetworkAccessManager m_phone; // keeps the session cookie

    QUrl url(const QString &path, const QUrlQuery &query = {}) const
    {
        QUrl u(QStringLiteral("https://127.0.0.1:%1%2").arg(m_server->port()).arg(path));
        u.setQuery(query);
        return u;
    }

    // Scans the QR token into a session cookie on m_phone.
    bool signIn()
    {
        const QString token = m_server->newAccessToken();
        return request(m_phone, "GET", url("/", QUrlQuery("t=" + token))).status == 200;
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
        o.userId = [this] { return m_user; };
        m_server = std::make_unique<PhoneServer>(o);
        QString error;
        QVERIFY2(m_server->start(&error), qPrintable(error));
    }

    // A session granted by user A stops working once user B is at the console, and works again
    // when A returns.
    void sessionIsBoundToTheUserWhoGrantedIt()
    {
        m_user = QStringLiteral("S-1-5-21-user-A");
        QVERIFY(signIn());
        QCOMPARE(request(m_phone, "GET", url("/api/list")).status, 200);

        m_user = QStringLiteral("S-1-5-21-user-B"); // a different user switches in
        QCOMPARE(request(m_phone, "GET", url("/api/list")).status, 401);

        m_user = QStringLiteral("S-1-5-21-user-A"); // A comes back
        QCOMPARE(request(m_phone, "GET", url("/api/list")).status, 200);
    }

    void singleRangeIsServed()
    {
        m_user = QStringLiteral("S-1-5-21-user-A");
        QVERIFY(signIn());
        const QByteArray content = pattern(10000);
        QFile f(m_folder.filePath("clip.bin"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();

        // A whole-file request still advertises range support.
        const Reply full = request(m_phone, "GET", url("/api/file", QUrlQuery("path=clip.bin")));
        QCOMPARE(full.status, 200);
        QCOMPARE(full.raw->rawHeader("Accept-Ranges"), QByteArray("bytes"));
        QCOMPARE(full.body, content);

        // A single range: 206 with Content-Range and exactly those bytes.
        const Reply ranged = request(m_phone, "GET", url("/api/file", QUrlQuery("path=clip.bin")),
                                     {}, {{"Range", "bytes=100-199"}});
        QCOMPARE(ranged.status, 206);
        QCOMPARE(ranged.raw->rawHeader("Content-Range"), QByteArray("bytes 100-199/10000"));
        QCOMPARE(ranged.body, content.mid(100, 100));
        QCOMPARE(ranged.body.size(), 100);

        // An open-ended range runs to the end of the file.
        const Reply tail = request(m_phone, "GET", url("/api/file", QUrlQuery("path=clip.bin")),
                                   {}, {{"Range", "bytes=9990-"}});
        QCOMPARE(tail.status, 206);
        QCOMPARE(tail.raw->rawHeader("Content-Range"), QByteArray("bytes 9990-9999/10000"));
        QCOMPARE(tail.body, content.mid(9990));

        // A suffix range: the last N bytes.
        const Reply suffix = request(m_phone, "GET", url("/api/file", QUrlQuery("path=clip.bin")),
                                     {}, {{"Range", "bytes=-10"}});
        QCOMPARE(suffix.status, 206);
        QCOMPARE(suffix.body, content.right(10));

        // Multi-range is served whole (no amplification), a range past the end is 416.
        const Reply multi = request(m_phone, "GET", url("/api/file", QUrlQuery("path=clip.bin")),
                                    {}, {{"Range", "bytes=0-9,20-29"}});
        QCOMPARE(multi.status, 200);
        QCOMPARE(multi.body.size(), content.size());
        const Reply bad = request(m_phone, "GET", url("/api/file", QUrlQuery("path=clip.bin")),
                                  {}, {{"Range", "bytes=20000-20099"}});
        QCOMPARE(bad.status, 416);
        QCOMPARE(bad.raw->rawHeader("Content-Range"), QByteArray("bytes */10000"));
    }
};

QTEST_GUILESS_MAIN(TestPhoneUser)
#include "tst_phoneuser.moc"
