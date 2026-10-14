#include "filetransfer.h"
#include "securestream.h"
#include "testutil.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray randomBytes(int n, quint32 seed)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator rng(seed);
    for (int i = 0; i < n; ++i)
        b[i] = char(rng.bounded(256));
    return b;
}
} // namespace

class TestTransfer : public QObject
{
    Q_OBJECT
private slots:
    void sanitize()
    {
        QCOMPARE(FileTransfer::sanitizeRelativePath("photos/2024/a.jpg"), QString("photos/2024/a.jpg"));
        QCOMPARE(FileTransfer::sanitizeRelativePath("photos\\a.jpg"), QString("photos/a.jpg"));
        QVERIFY(FileTransfer::sanitizeRelativePath("../evil").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("a/../../evil").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("/etc/passwd").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("C:/Windows/x").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("a//b").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("").isEmpty());
    }

    void encryptedStreamBetweenPairedDevices()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));

        QList<QByteArray> got;
        SecureStream *accepted = nullptr;
        connect(b.get(), &Node::streamAccepted, this, [&](SecureStream *s) {
            accepted = s;
            connect(s, &SecureStream::received, this, [&](const QByteArray &f) { got.append(f); });
        });
        std::unique_ptr<SecureStream> client(a->openStream(b->id()));
        QVERIFY(client);
        QSignalSpy established(client.get(), &SecureStream::established);
        QTRY_COMPARE(established.size(), 1);
        QTRY_VERIFY(accepted);
        QCOMPARE(accepted->peerId(), a->id());

        // 8 MB in 64 KB frames arrives intact and in order.
        const QByteArray payload = randomBytes(8 * 1024 * 1024, 7);
        for (int off = 0; off < payload.size(); off += SecureStream::kMaxFrame)
            QVERIFY(client->send(payload.mid(off, SecureStream::kMaxFrame)));
        QVERIFY(!client->send(QByteArray(SecureStream::kMaxFrame + 1, 'x')));
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), payload.size() / SecureStream::kMaxFrame, 20000);
        QCOMPARE(got.join(), payload);
    }

    void untrustedOrImpersonatingPeersAreRejected()
    {
        QVERIFY(crypto::init());
        const crypto::KeyPair server = crypto::generateKeyPair();
        const crypto::KeyPair client = crypto::generateKeyPair();
        const crypto::KeyPair other = crypto::generateKeyPair();

        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost));
        QList<SecureStream *> serverSide;
        QByteArray trustedKey; // what the server accepts
        connect(&listener, &QTcpServer::newConnection, this, [&] {
            auto trust = [&](const QByteArray &key) -> std::optional<QByteArray> {
                return key == trustedKey ? std::optional<QByteArray>("peer") : std::nullopt;
            };
            serverSide.append(SecureStream::accept(listener.nextPendingConnection(), server, trust, this));
        });

        auto attempt = [&](const crypto::KeyPair &local, const QByteArray &expectedServerKey) {
            std::unique_ptr<SecureStream> s(SecureStream::connectTo(local, expectedServerKey, "server",
                                                                    QHostAddress::LocalHost, listener.serverPort()));
            QSignalSpy established(s.get(), &SecureStream::established);
            QSignalSpy closed(s.get(), &SecureStream::closed);
            (void)QTest::qWaitFor([&] { return established.size() || closed.size(); }, 12000);
            return established.size() == 1;
        };

        trustedKey = client.publicKey;
        QVERIFY(attempt(client, server.publicKey));   // trusted client, genuine server
        QVERIFY(!attempt(other, server.publicKey));   // client the server does not trust
        QVERIFY(!attempt(client, other.publicKey));   // server is not who the client expects
        qDeleteAll(serverSide);
    }

    void fetchFilesFoldersAndBlobs()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());

        const QByteArray big = randomBytes(3 * 1024 * 1024 + 123, 1);
        writeFile(source.filePath("report.pdf"), big);
        writeFile(source.filePath("photos/2024/a.jpg"), randomBytes(70000, 2));
        writeFile(source.filePath("photos/b.txt"), "hello");
        writeFile(source.filePath("photos/empty.txt"), "");

        Offer offer;
        offer.blobs.insert("image/png", randomBytes(500000, 3));
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("report.pdf"), source.filePath("photos")}, 1 << 30, 1000,
                                &error));
        QCOMPARE(offer.files.size(), 4);
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.blobs.value("image/png"), offer.blobs.value("image/png"));
        QCOMPARE(readFile(target.filePath("report.pdf")), big);
        QCOMPARE(readFile(target.filePath("photos/2024/a.jpg")), randomBytes(70000, 2));
        QCOMPARE(readFile(target.filePath("photos/b.txt")), QByteArray("hello"));
        QVERIFY(QFileInfo::exists(target.filePath("photos/empty.txt")));
        QStringList tops = result.files;
        tops.sort();
        QCOMPARE(tops, QStringList({target.filePath("photos"), target.filePath("report.pdf")}));
    }

    void limitsAndUnknownOffers()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("big.bin"), QByteArray(200000, 'x'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        QVERIFY(!Offer::addPaths(&offer, {source.filePath("big.bin")}, 100, 10, &error)); // too large to offer
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        auto cb = [&](const FetchResult &r) {
            result = r;
            done = true;
        };
        fb.fetch(a->id(), id, target.path(), true, 1000, cb); // receiver limit
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QVERIFY(!QFileInfo::exists(target.filePath("big.bin")));

        done = false;
        fb.fetch(a->id(), id, target.path(), false, 1000, cb); // without files it fits
        QTRY_VERIFY(done);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.files.isEmpty());

        done = false;
        fb.fetch(a->id(), id + 1, target.path(), true, 1 << 30, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("No such offer"));
    }
};

QTEST_GUILESS_MAIN(TestTransfer)
#include "tst_transfer.moc"
