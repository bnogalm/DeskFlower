#include "filetransfer.h"
#include "fsutil.h"
#include "securestream.h"
#include "testutil.h"
#include "transferworker.h"
#include "wire.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace df;
using test::makeJunction;

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

// Random bytes fast enough for tens of megabytes.
QByteArray randomBlob(qsizetype n, quint32 seed)
{
    QList<quint32> words((n + 3) / 4);
    QRandomGenerator(seed).fillRange(words.data(), words.size());
    return QByteArray(reinterpret_cast<const char *>(words.constData()), n);
}

// An offer of `fileCount` files spread over `dirCount` folders under "tree", every file the same
// small source file (the publisher reads only what it serves; the receiver writes them all).
Offer syntheticTree(const QString &sourceFile, qint64 size, int dirCount, int fileCount)
{
    Offer offer;
    offer.dirs.append(QStringLiteral("tree"));
    for (int d = 0; d < dirCount; ++d)
        offer.dirs.append(QStringLiteral("tree/d%1").arg(d));
    for (int f = 0; f < fileCount; ++f)
        offer.files.append(Offer::File{sourceFile, QStringLiteral("tree/d%1/f%2.txt").arg(f % std::max(dirCount, 1)).arg(f), size});
    return offer;
}

QByteArray randomBytes(int n, quint32 seed)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator rng(seed);
    for (int i = 0; i < n; ++i)
        b[i] = char(rng.bounded(256));
    return b;
}

// Frames as the transfer protocol sends them (1 = CBOR control, 3 = data since protocol 3).
QByteArray controlFrame(const QCborMap &msg) { return char(1) + QCborValue(msg).toCbor(); }
QByteArray dataFrame(const QByteArray &data) { return char(3) + data; }
QCborMap manifestItem(const char *kind, const QString &name, qint64 size)
{
    QCborMap item;
    item.insert(QStringLiteral("k"), QString::fromLatin1(kind));
    item.insert(QStringLiteral("n"), name);
    item.insert(QStringLiteral("s"), size);
    return item;
}
} // namespace

class TestTransfer : public QObject
{
    Q_OBJECT
private slots:
    // DESKFLOWER_TEST_WRITERS=1 or 8: the whole run with that many writers per fetch (the cases
    // that choose their own keep them).
    void initTestCase()
    {
        if (const int writers = qEnvironmentVariableIntValue("DESKFLOWER_TEST_WRITERS"); writers > 0) {
            FileTransfer::setDefaultWriterThreadsForTests(writers);
            qInfo() << "Every fetch with" << writers << "writers";
        }
    }

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

        // Windows reserved device names, on their own or with any extension.
        for (const char *name : {"NUL", "CON", "con", "PRN", "AUX", "COM1", "lpt9", "NUL.txt", "a/COM1/b", "CON.tar.gz"})
            QVERIFY2(FileTransfer::sanitizeRelativePath(name).isEmpty(), name);
        // ...but names that only start like one are fine.
        QCOMPARE(FileTransfer::sanitizeRelativePath("COM0"), QString("COM0"));
        QCOMPARE(FileTransfer::sanitizeRelativePath("CONSOLE"), QString("CONSOLE"));
        QCOMPARE(FileTransfer::sanitizeRelativePath("NULabc"), QString("NULabc"));
        // Trailing dot or space (Win32 strips them, so "a." would collide with "a").
        QVERIFY(FileTransfer::sanitizeRelativePath("a.").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("a ").isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath("ok/bad.").isEmpty());
        // Control characters and over-long components.
        QVERIFY(FileTransfer::sanitizeRelativePath(QStringLiteral("a\tb")).isEmpty());
        QVERIFY(FileTransfer::sanitizeRelativePath(QString(256, QLatin1Char('x'))).isEmpty());
        QCOMPARE(FileTransfer::sanitizeRelativePath(QString(255, QLatin1Char('x'))).size(), 255);
    }

    // A failed or cancelled transfer leaves no half-written files on disk.
    void partialFilesAreRemovedOnFailure()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());

        writeFile(source.filePath("big.bin"), randomBytes(16 * 1024 * 1024, 9));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        const QString dest = target.filePath("dl");
        bool done = false;
        FetchResult result;
        const quint64 handle = fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        // Cancel once the file has started being written (but before it finishes).
        QTRY_VERIFY(done || QFileInfo::exists(QDir(dest).filePath("big.bin")));
        if (!done) {
            fb.cancel(handle);
            QVERIFY(done); // at once
            QVERIFY(!result.ok);
            QVERIFY(result.cancelled);
            // Removed by the fetch's worker, off this thread.
            QTRY_VERIFY2(!QFileInfo(dest).exists(), "the partly-written download folder was left behind");
        }
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
        // The accepting side trusts the stream only once a frame from the client decrypts.
        QTest::qWait(100);
        QVERIFY(!accepted);

        // 8 MB in 64 KB frames arrives intact and in order.
        const QByteArray payload = randomBytes(8 * 1024 * 1024, 7);
        for (int off = 0; off < payload.size(); off += SecureStream::kMaxFrame)
            QVERIFY(client->send(payload.mid(off, SecureStream::kMaxFrame)));
        QVERIFY(!client->send(QByteArray(SecureStream::kMaxFrame + 1, 'x')));
        QTRY_VERIFY(accepted);
        QCOMPARE(accepted->peerId(), a->id());
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
            (void)test::waitFor([&] { return established.size() || closed.size(); }, 12000);
            return established.size() == 1;
        };

        trustedKey = client.publicKey;
        QVERIFY(attempt(client, server.publicKey));   // trusted client, genuine server
        QVERIFY(!attempt(other, server.publicKey));   // client the server does not trust
        QVERIFY(!attempt(client, other.publicKey));   // server is not who the client expects
        qDeleteAll(serverSide);
    }

    void replayedHandshakeIsNotAccepted()
    {
        QVERIFY(crypto::init());
        const crypto::KeyPair server = crypto::generateKeyPair();
        const crypto::KeyPair client = crypto::generateKeyPair();
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost));
        SecureStream *serverSide = nullptr;
        connect(&listener, &QTcpServer::newConnection, this, [&] {
            auto trust = [&](const QByteArray &key) -> std::optional<QByteArray> {
                return key == client.publicKey ? std::optional<QByteArray>("peer") : std::nullopt;
            };
            serverSide = SecureStream::accept(listener.nextPendingConnection(), server, trust, this);
        });

        // A recorded first handshake message from the trusted client, sent again by someone
        // without its keys: the server answers, but nothing that follows can decrypt.
        noise::IKInitiator initiator(client, server.publicKey, QByteArray("deskflower/tcp/1"));
        const QByteArray message1 = initiator.writeMessage1({});
        auto framed = [](const QByteArray &frame) {
            char len[4];
            qToBigEndian<quint32>(quint32(frame.size()), len);
            return QByteArray(len, 4) + frame;
        };
        QTcpSocket replay;
        replay.connectToHost(QHostAddress::LocalHost, listener.serverPort());
        QVERIFY(replay.waitForConnected(5000));
        replay.write(framed(message1));
        QTRY_VERIFY(serverSide);
        QSignalSpy established(serverSide, &SecureStream::established);
        QSignalSpy closed(serverSide, &SecureStream::closed);
        QTRY_VERIFY(replay.bytesAvailable() > 0); // message 2
        replay.write(framed(QByteArray(64, 'x')));
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(established.size(), 0);
        delete serverSide;
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

    void guestsFetchOnlyWhatTheyAreLetInto()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        a->setTrust(b->id(), TrustLevel::Guest);
        FileTransfer fa(a.get()), fb(b.get());
        Offer offer;
        offer.blobs.insert("image/png", randomBytes(1000, 4));
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        auto cb = [&](const FetchResult &r) {
            result = r;
            done = true;
        };
        fb.fetch(a->id(), id, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);

        fa.allowPeer(id, b->id(), true); // e.g. files dropped on it
        done = false;
        fb.fetch(a->id(), id, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY2(result.ok, qPrintable(result.error));

        fa.allowPeer(id, b->id(), false);
        done = false;
        fb.fetch(a->id(), id, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
    }

    // A gated offer (the sealed clipboard) is served to nobody, trusted peers included, until the
    // publisher lets them in one by one; an ungated offer is unchanged for trusted peers.
    void gatedOffersNeedAnAllowedPeer()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b)); // B is a trusted (non-guest) device of A
        FileTransfer fa(a.get()), fb(b.get());

        Offer gated;
        gated.gated = true;
        gated.blobs.insert("image/png", randomBytes(1000, 11));
        const quint64 id = fa.publish(gated);

        FetchResult result;
        bool done = false;
        auto cb = [&](const FetchResult &r) {
            result = r;
            done = true;
        };
        // A trusted peer is refused from a gated offer until let in.
        fb.fetch(a->id(), id, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("No such offer"));

        fa.allowPeer(id, b->id(), true);
        done = false;
        fb.fetch(a->id(), id, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY2(result.ok, qPrintable(result.error));

        fa.allowPeer(id, b->id(), false);
        done = false;
        fb.fetch(a->id(), id, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);

        // An ungated offer is unchanged for a trusted peer (fetched without any allow).
        Offer ungated;
        ungated.blobs.insert("image/png", randomBytes(1000, 12));
        const quint64 open = fa.publish(ungated);
        done = false;
        fb.fetch(a->id(), open, target.path(), false, 1 << 20, cb);
        QTRY_VERIFY(done);
        QVERIFY2(result.ok, qPrintable(result.error));
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

    // A hostile sender answers the fetch itself: announced sizes cannot wrap the sum past the
    // limit, no item may exceed it, and the bytes actually sent are counted against it too.
    void hostileManifestsAreRefused()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fb(b.get());

        // A has no FileTransfer: the test answers every "get" with the manifest and data of the case.
        QCborArray items;
        QList<QByteArray> data;
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"), items);
                stream->send(controlFrame(manifest));
                for (const QByteArray &chunk : std::as_const(data))
                    stream->send(dataFrame(chunk));
                QCborMap end;
                end.insert(QStringLiteral("t"), QStringLiteral("end"));
                stream->send(controlFrame(end));
            });
        });
        auto fetchWith = [&](qint64 maxBytes) {
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), 1, target.path(), true, maxBytes, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            (void)test::waitFor([&] { return done; }, 15000);
            return result;
        };

        // Honest, as a control: the blob arrives.
        items = {manifestItem("blob", "a", 5)};
        data = {"hello"};
        FetchResult r = fetchWith(1000);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.blobs.value("a"), QByteArray("hello"));

        // Two items of 2^62 bytes under a limit that admits one: their sum wraps negative, which
        // must not pass as "below the limit".
        items = {manifestItem("blob", "a", qint64(1) << 62), manifestItem("blob", "b", qint64(1) << 62)};
        data = {};
        r = fetchWith((qint64(1) << 62) + 10);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Transfer larger than allowed"));

        // One item just over the limit.
        items = {manifestItem("blob", "a", 1001)};
        r = fetchWith(1000);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Transfer larger than allowed"));

        // Within the limit but larger than the destination's free space (a petabyte): refused
        // before anything is written.
        items = {manifestItem("file", "huge.bin", qint64(1) << 50)};
        data = {};
        r = fetchWith(qint64(1) << 60);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.startsWith("Not enough free disk space"), qPrintable(r.error));
        QVERIFY(!QFileInfo::exists(target.filePath("huge.bin")));

        // Sizes that fit, but more bytes than announced actually sent.
        items = {manifestItem("blob", "a", 5)};
        data = {"hello", "more"};
        r = fetchWith(1000);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("More data than announced"));

        // A file named after a device, or escaping the target, is refused before anything is written.
        items = {manifestItem("file", "../escape.txt", 3)};
        data = {"abc"};
        r = fetchWith(1000);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Unsafe or invalid item in manifest"));
        items = {manifestItem("file", "NUL", 3)};
        r = fetchWith(1000);
        QVERIFY(!r.ok);
        QVERIFY(!QFileInfo::exists(target.filePath("NUL")));
        QVERIFY(!QFileInfo::exists(QDir(target.path()).filePath("../escape.txt")));
    }

    // The service only serves and receives files as the console user (the injected impersonation
    // guard): with nobody signed in nothing is offered, served or written, and an offered path that
    // goes through a link is refused.
    void nothingIsServedOrReceivedWithoutAUser()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("doc.txt"), "some text");
        QString error;

        // Offering with a user: fine. Without one: refused.
        Offer offer;
        QVERIFY(fa.addOfferPaths(&offer, {source.filePath("doc.txt")}, 1 << 30, 10, &error));
        fa.setImpersonator([] { return std::shared_ptr<void>(); });
        Offer none;
        QVERIFY(!fa.addOfferPaths(&none, {source.filePath("doc.txt")}, 1 << 30, 10, &error));
        QCOMPARE(error, QString("Nobody is signed in"));

        // Serving an offer made earlier, after the user signed out: refused at the open.
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        auto cb = [&](const FetchResult &r) {
            result = r;
            done = true;
        };
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Nobody is signed in"));
        QVERIFY(!QFileInfo::exists(target.filePath("doc.txt")));

        // The same when the offer starts with blobs (a clipboard copy with files): the serving
        // pass begins on a blob and reaches the file in the same loop, still as nobody.
        Offer mixed = offer;
        mixed.blobs.insert(QStringLiteral("text/plain"), QByteArray("copied text"));
        const quint64 mixedId = fa.publish(mixed);
        done = false;
        fb.fetch(a->id(), mixedId, target.path(), true, 1 << 30, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Nobody is signed in"));
        QVERIFY(!QFileInfo::exists(target.filePath("doc.txt")));

        // Receiving without a user: nothing is written.
        fa.setImpersonator({}); // the default no-op guard again
        fb.setImpersonator([] { return std::shared_ptr<void>(); });
        done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, cb);
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Nobody is signed in to receive files"));
        QVERIFY(!QFileInfo::exists(target.filePath("doc.txt")));
        fb.setImpersonator({});
        done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, cb);
        QTRY_VERIFY(done);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath("doc.txt")), QByteArray("some text"));

        // A chosen path through a junction (the user's "My Documents", a redirected folder) is
        // offered: it is read with that user's rights, which are the boundary.
        QTemporaryDir outside;
        writeFile(outside.filePath("mine.txt"), "the user's own");
        QVERIFY(makeJunction(source.filePath("link"), outside.path()));
        Offer linked;
        QVERIFY2(fa.addOfferPaths(&linked, {source.filePath("link/mine.txt")}, 1 << 30, 10, &error), qPrintable(error));
        QCOMPARE(linked.files.size(), 1);
        QVERIFY(linked.skipped.isEmpty());
    }

    // Folders travel as entries of their own: nested trees, and empty folders at any depth (a
    // copied or dragged empty folder, or one holding only empty folders), are created by the
    // receiver, and the top-level items include folder-only ones.
    void nestedAndEmptyFoldersArrive()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("project/src/main.cpp"), "int main() {}");
        QVERIFY(QDir().mkpath(source.filePath("project/build/empty/deeper")));
        QVERIFY(QDir().mkpath(source.filePath("project/docs")));
        QVERIFY(QDir().mkpath(source.filePath("lonely")));

        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("project"), source.filePath("lonely")}, 1 << 30, 100, &error));
        QCOMPARE(offer.files.size(), 1);
        QStringList dirs = offer.dirs;
        dirs.sort();
        QCOMPARE(dirs, QStringList({"lonely", "project", "project/build", "project/build/empty",
                                    "project/build/empty/deeper", "project/docs", "project/src"}));
        // Parents come before their children (the order the list is shown and created in).
        for (const QString &d : offer.dirs) {
            const QString parent = d.section(QLatin1Char('/'), 0, -2);
            if (!parent.isEmpty())
                QVERIFY2(offer.dirs.indexOf(parent) < offer.dirs.indexOf(d), qPrintable(d));
        }
        const QList<ListedFile> listing = offer.listing();
        QCOMPARE(listing.size(), 8);
        QCOMPARE(listedDirCount(listing), 7);
        QVERIFY(listing.first().dir);
        QVERIFY(!listing.last().dir);

        // Folders are counted apart, against the same bound as the files.
        Offer tooMany;
        QVERIFY(!Offer::addPaths(&tooMany, {source.filePath("project")}, 1 << 30, 3, &error));
        QCOMPARE(error, QString("More than 3 folders"));

        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath("project/src/main.cpp")), QByteArray("int main() {}"));
        QVERIFY(QFileInfo(target.filePath("project/build/empty/deeper")).isDir());
        QVERIFY(QFileInfo(target.filePath("project/docs")).isDir());
        QVERIFY(QFileInfo(target.filePath("lonely")).isDir());
        QStringList tops = result.files;
        tops.sort();
        QCOMPARE(tops, QStringList({target.filePath("lonely"), target.filePath("project")}));

        // A folder entry is sanitised like a file name: one escaping the target is refused.
        QTemporaryDir dirC;
        auto c = test::makeNode(dirC.path());
        QVERIFY(test::pairNodes(*b, *c));
        connect(c.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"), QCborArray{manifestItem("dir", "../escaped", 0)});
                stream->send(controlFrame(manifest));
            });
        });
        done = false;
        fb.fetch(c->id(), 1, target.filePath("evil"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Unsafe or invalid item in manifest"));
        QVERIFY(!QFileInfo::exists(target.filePath("escaped")));
    }

    // Links met inside a chosen folder are left out and reported (they could lead anywhere),
    // without failing the rest; hidden files are offered like any other.
    void linksInsideAFolderAreSkipped()
    {
        QTemporaryDir source, outside;
        writeFile(outside.filePath("elsewhere.txt"), "not part of the folder");
        writeFile(source.filePath("folder/real.txt"), "real");
        writeFile(source.filePath("folder/.hidden"), "hidden");
        QVERIFY(makeJunction(source.filePath("folder/link"), outside.path()));

        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("folder")}, 1 << 30, 100, &error));
        QCOMPARE(offer.skipped, QStringList({"folder/link"}));
        QStringList names;
        for (const Offer::File &f : offer.files)
            names << f.relativePath;
        names.sort();
        QCOMPARE(names, QStringList({"folder/.hidden", "folder/real.txt"}));
        QCOMPARE(offer.dirs, QStringList({"folder"}));
    }

    // A junction planted where the manifest will make a folder: the fetch fails before anything
    // goes through it, and the cleanup removes the junction, not what it points to.
    void junctionAtUncreatedSubfolderIsRefused()
    {
#ifndef Q_OS_WIN
        QSKIP("Links are checked on Windows, where the service runs as LocalSystem");
#else
        QTemporaryDir dirA, dirB, source, target, outside;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("top/sub/x.txt"), "through the junction?");
        writeFile(source.filePath("top/y.txt"), "y");
        writeFile(outside.filePath("precious.txt"), "keep me");
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("top")}, 1 << 30, 100, &error));
        const quint64 id = fa.publish(offer);
        const QString dest = target.filePath("dl");
        QVERIFY(QDir().mkpath(dest + "/top"));
        QVERIFY(makeJunction(dest + "/top/sub", outside.path()));

        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("The destination goes through a link"));
        QTRY_VERIFY(!fb.isActiveTarget(dest));
        QVERIFY(!QFileInfo::exists(outside.filePath("x.txt")));
        QCOMPARE(readFile(outside.filePath("precious.txt")), QByteArray("keep me"));
        QTRY_VERIFY(!QFileInfo::exists(dest)); // cleaned up, junction and all
        QCOMPARE(readFile(outside.filePath("precious.txt")), QByteArray("keep me"));
#endif
    }

    // While a fetch writes, its folders (and the destination itself) cannot be renamed or swapped
    // for a junction; once it is over they can.
    void receivedFolderIsPinnedMidTransfer()
    {
#ifndef Q_OS_WIN
        QSKIP("Held open on Windows only");
#else
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("one.txt"), QByteArray(1000, 'o'));
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 1000, 4, 200));
        fb.setWriteDelayForTests(10);
        const QString dest = target.filePath("dl");
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || QFileInfo::exists(dest + "/tree/d1/f1.txt"), 15000);
        QVERIFY(!done);
        auto renames = [](const QString &from, const QString &to) {
            return MoveFileExW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(from).utf16()),
                               reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(to).utf16()), 0)
                   != 0;
        };
        QVERIFY(!renames(dest + "/tree/d1", dest + "/tree/moved"));
        QVERIFY(!renames(dest + "/tree", dest + "/moved"));
        QVERIFY(!renames(dest, target.filePath("moved")));
        fb.setWriteDelayForTests(0);
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(QDir(dest + "/tree/d1").entryList(QDir::Files).size(), 50);
        QVERIFY(renames(dest + "/tree/d1", dest + "/tree/moved"));
        QVERIFY(renames(dest, target.filePath("moved")));
#endif
    }

#ifdef Q_OS_WIN
    // Files with the system attribute and shortcuts (.lnk) are ordinary files: offered with their
    // own size (Qt reports a shortcut's target's type and size), and transferred byte for byte, at
    // the top level and inside a folder.
    void systemFilesAndShortcutsAreTransferred()
    {
        QTemporaryDir dirA, dirB, source, target, outside;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(outside.filePath("big-target.bin"), randomBytes(50000, 5));
        writeFile(source.filePath("folder/system.dat"), "system file");
        QVERIFY(SetFileAttributesW(reinterpret_cast<const wchar_t *>(
                                       QDir::toNativeSeparators(source.filePath("folder/system.dat")).utf16()),
                                   FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN));
        QVERIFY(test::makeShortcut(source.filePath("folder/to-folder.lnk"), outside.path()));
        QVERIFY(test::makeShortcut(source.filePath("to-file.lnk"), outside.filePath("big-target.bin")));
        const QByteArray folderLnk = readFile(source.filePath("folder/to-folder.lnk"));
        const QByteArray fileLnk = readFile(source.filePath("to-file.lnk"));
        QVERIFY(!folderLnk.isEmpty() && !fileLnk.isEmpty());

        Offer offer;
        QString error;
        QVERIFY(fa.addOfferPaths(&offer, {source.filePath("folder"), source.filePath("to-file.lnk")}, 1 << 30, 100,
                                 &error));
        QVERIFY(offer.skipped.isEmpty());
        QCOMPARE(offer.dirs, QStringList({"folder"}));
        QMap<QString, qint64> sizes;
        for (const Offer::File &f : offer.files)
            sizes.insert(f.relativePath, f.size);
        QCOMPARE(sizes.value("folder/system.dat", -1), qint64(11));
        QCOMPARE(sizes.value("folder/to-folder.lnk", -1), qint64(folderLnk.size()));
        QCOMPARE(sizes.value("to-file.lnk", -1), qint64(fileLnk.size()));

        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath("folder/system.dat")), QByteArray("system file"));
        QCOMPARE(readFile(target.filePath("folder/to-folder.lnk")), folderLnk);
        QCOMPARE(readFile(target.filePath("to-file.lnk")), fileLnk);
    }
#endif

    // A receiver of another protocol (0.1.22-0.1.30 send get v 2 without pv; earlier ones no v at
    // all; a newer one another pv) is answered with an error that names the device to update, and
    // no reader thread is started for it.
    void olderReceiverIsRefusedCleanly()
    {
        QTemporaryDir dirA, dirB, source;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get());
        writeFile(source.filePath("folder/a.txt"), "a");
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("folder")}, 1 << 30, 100, &error));
        const quint64 id = fa.publish(offer);

        auto replyTo = [&](std::optional<int> v, std::optional<int> pv) {
            std::unique_ptr<SecureStream> s(b->openStream(a->id()));
            QList<QByteArray> frames;
            bool closed = false;
            connect(s.get(), &SecureStream::established, s.get(), [&] {
                QCborMap get;
                get.insert(QStringLiteral("t"), QStringLiteral("get"));
                get.insert(QStringLiteral("o"), qint64(id));
                get.insert(QStringLiteral("files"), true);
                if (v)
                    get.insert(QStringLiteral("v"), *v);
                if (pv)
                    get.insert(QStringLiteral("pv"), *pv);
                s->send(controlFrame(get));
            });
            connect(s.get(), &SecureStream::received, s.get(), [&](const QByteArray &f) { frames.append(f); });
            connect(s.get(), &SecureStream::closed, s.get(), [&] { closed = true; });
            (void)test::waitFor([&] { return closed; }, 10000);
            return frames;
        };
        const QString older = QStringLiteral("%1 uses a newer transfer protocol (version %2). The protocol has changed: "
                                             "update this device.")
                                  .arg(a->name())
                                  .arg(wire::kProtocolVersion);
        for (const auto &[v, pv] : QList<std::pair<std::optional<int>, std::optional<int>>>{
                 {2, std::nullopt}, {std::nullopt, std::nullopt}, {3, 2}}) {
            const QList<QByteArray> frames = replyTo(v, pv);
            QCOMPARE(frames.size(), 1); // the error alone: no manifest, no data
            QCOMPARE(quint8(frames.at(0).at(0)), quint8(1));
            const QCborMap reply = QCborValue::fromCbor(frames.at(0).mid(1)).toMap();
            QCOMPARE(reply.value(QStringLiteral("t")).toString(), QString("error"));
            QCOMPARE(reply.value(QStringLiteral("r")).toString(), older);
        }
        const QList<QByteArray> newer = replyTo(3, wire::kProtocolVersion + 1);
        QCOMPARE(newer.size(), 1);
        QVERIFY(QCborValue::fromCbor(newer.at(0).mid(1)).toMap().value(QStringLiteral("r")).toString().contains(
            QStringLiteral("uses an older transfer protocol (version %1)").arg(wire::kProtocolVersion)));
        QCOMPARE(fa.liveUploadsForTests(), 0);
        // This device's own get is served.
        const QList<QByteArray> current = replyTo(3, wire::kProtocolVersion);
        QVERIFY(current.size() >= 2);
        const QCborMap manifest = QCborValue::fromCbor(current.at(0).mid(1)).toMap();
        QCOMPARE(manifest.value(QStringLiteral("t")).toString(), QString("manifest"));
        QCOMPARE(manifest.value(QStringLiteral("pv")).toInteger(), wire::kProtocolVersion);
    }

    // A publisher of 0.1.22-0.1.30 answers this device's get (it reads v 3 as "folders") with a
    // manifest without pv and data frames of kind 2: refused at the manifest, before anything is
    // written, for a fetch and for a list fetch alike; data of kind 2 after a current manifest is
    // refused too.
    void olderPublisherIsRefusedCleanly()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fb(b.get());
        QCborMap get;
        bool currentManifest = false; // the manifest says pv 3, the data is of kind 2
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                get = QCborValue::fromCbor(frame.mid(1)).toMap();
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                if (currentManifest)
                    manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"),
                                QCborArray{manifestItem("dir", "docs", 0), manifestItem("file", "docs/a.txt", 3)});
                manifest.insert(QStringLiteral("more"), false);
                stream->send(controlFrame(manifest));
                stream->send(char(2) + QByteArray("abc"));
                QCborMap end;
                end.insert(QStringLiteral("t"), QStringLiteral("end"));
                stream->send(controlFrame(end));
            });
        });
        const QString dest = target.filePath("dl");
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), 42, dest, true, 1 << 20, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("%1 uses an older transfer protocol (version 2). The protocol has changed: "
                                              "update it.")
                                   .arg(a->name()));
        // What was asked: protocol 3, with the v an older publisher reads.
        QCOMPARE(get.value(QStringLiteral("t")).toString(), QString("get"));
        QCOMPARE(get.value(QStringLiteral("pv")).toInteger(), wire::kProtocolVersion);
        QCOMPARE(get.value(QStringLiteral("v")).toInteger(), 3);
        QTRY_VERIFY(!fb.isActiveTarget(dest));
        QVERIFY(!QFileInfo::exists(dest)); // nothing written, nothing left

        bool listed = false, ok = true, refused = false;
        QString error;
        fb.fetchList(a->id(), 42, 1 << 20, [&](bool o, bool r, const QString &e, const QList<ListedFile> &) {
            listed = true;
            ok = o;
            refused = r;
            error = e;
        });
        QTRY_VERIFY_WITH_TIMEOUT(listed, 15000);
        QVERIFY(!ok);
        QVERIFY(refused);
        QVERIFY2(error.contains(QStringLiteral("older transfer protocol (version 2)")), qPrintable(error));

        currentManifest = true;
        done = false;
        fb.fetch(a->id(), 42, dest, true, 1 << 20, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Data of an older transfer protocol"));
        QTRY_VERIFY(!fb.isActiveTarget(dest));
        QTRY_VERIFY(!QFileInfo::exists(dest)); // the folders it made are removed again
    }

    // A peer whose hello advertises another protocol (or none: 0.1.30 and before) is refused up
    // front: the fetch and the list fetch fail after the call returns, without opening a stream or
    // starting a worker; the peer is still connected (input, clipboard text and updates go on).
    void fetchToIncompatiblePeerFailsBeforeConnecting()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        a->setHelloOverrideForTests([](QCborMap &hello) {
            hello.insert(QStringLiteral("v"), QStringLiteral("0.1.30"));
            hello.remove(QStringLiteral("pv"));
        });
        QVERIFY(test::pairNodes(*a, *b));
        QTRY_COMPARE(b->peerProtocol(a->id()), 2);
        FileTransfer fa(a.get()), fb(b.get());
        int accepted = 0;
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *) { ++accepted; });
        Offer offer;
        offer.blobs.insert(QStringLiteral("image/png"), QByteArray(10, 'p'));
        const quint64 id = fa.publish(offer);
        const QString expected = QStringLiteral("%1 uses an older transfer protocol (version 2). The protocol has "
                                                "changed: update it.")
                                     .arg(a->name());
        QCOMPARE(fb.incompatibleReason(a->id()), expected);
        QVERIFY(fb.incompatibleReason(b->id()).isEmpty()); // itself
        QSignalSpy finished(&fb, &FileTransfer::transferFinished);
        bool done = false;
        FetchResult result;
        const QString dest = target.filePath("dl");
        fb.fetch(a->id(), id, dest, true, 1 << 20,
                 [&](const FetchResult &r) {
                     result = r;
                     done = true;
                 },
                 QStringLiteral("clipboard"));
        QVERIFY(!done); // never inside fetch
        QVERIFY(!fb.isActiveTarget(dest)); // no worker
        QTRY_VERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, expected);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(3).toString(), QString("failed"));

        bool listed = false;
        bool refused = false;
        QString error;
        fb.fetchList(a->id(), id, 1 << 20, [&](bool, bool r, const QString &e, const QList<ListedFile> &) {
            listed = true;
            refused = r;
            error = e;
        });
        QVERIFY(!listed);
        QTRY_VERIFY(listed);
        QVERIFY(refused);
        QCOMPARE(error, expected);
        QTest::qWait(200);
        QCOMPARE(accepted, 0); // no stream was opened
        QVERIFY(test::viewOf(*b, a->id())->connected);

        // Updated (its hello now says protocol 3): transfers work again.
        a->setHelloOverrideForTests({});
        a->setCapability(QLatin1String(kDragCapability), true); // resends the hello
        QTRY_VERIFY(b->transfersCompatible(a->id()));
        done = false;
        fb.fetch(a->id(), id, dest, false, 1 << 20, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.blobs.value(QStringLiteral("image/png")), QByteArray(10, 'p'));
    }

    // Data frames are packed (protocol 3): every one full (kMaxFrame bytes with its kind byte)
    // across item boundaries, only the last shorter, the bytes exactly the items' in manifest
    // order; sizes at the frame's edges and empty files between items included.
    void packedFramesCrossItems()
    {
        QTemporaryDir dirA, dirB, source;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get());
        const int f = SecureStream::kMaxFrame;
        Offer offer;
        offer.blobs.insert(QStringLiteral("a"), randomBlob(1, 1));
        offer.blobs.insert(QStringLiteral("b"), randomBlob(f - 1, 2));
        offer.blobs.insert(QStringLiteral("c"), randomBlob(f, 3));
        offer.blobs.insert(QStringLiteral("d"), randomBlob(f + 1, 4));
        QByteArray expected;
        for (const QByteArray &blob : std::as_const(offer.blobs))
            expected += blob;
        const QList<qint64> sizes = {0, 5, 0, 0, f - 2, 1, 0, 3 * f + 7, 0};
        for (int i = 0; i < sizes.size(); ++i) {
            const QByteArray content = randomBlob(sizes.at(i), 10 + i);
            const QString path = source.filePath(QStringLiteral("f%1.bin").arg(i));
            writeFile(path, content);
            offer.files.append(Offer::File{path, QStringLiteral("f%1.bin").arg(i), sizes.at(i)});
            expected += content;
        }
        const quint64 id = fa.publish(offer);

        std::unique_ptr<SecureStream> s(b->openStream(a->id()));
        QList<QByteArray> data;
        bool ended = false;
        connect(s.get(), &SecureStream::established, s.get(), [&] {
            QCborMap get;
            get.insert(QStringLiteral("t"), QStringLiteral("get"));
            get.insert(QStringLiteral("o"), qint64(id));
            get.insert(QStringLiteral("files"), true);
            get.insert(QStringLiteral("v"), 3);
            get.insert(QStringLiteral("pv"), wire::kProtocolVersion);
            s->send(controlFrame(get));
        });
        connect(s.get(), &SecureStream::received, s.get(), [&](const QByteArray &frame) {
            if (frame.isEmpty())
                return;
            if (quint8(frame[0]) == 3)
                data.append(frame);
            else if (QCborValue::fromCbor(frame.mid(1)).toMap().value(QStringLiteral("t")).toString() == "end")
                ended = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(ended, 15000);
        QByteArray got;
        for (qsizetype i = 0; i < data.size(); ++i) {
            QVERIFY(data.at(i).size() <= f);
            if (i + 1 < data.size())
                QCOMPARE(data.at(i).size(), f); // all full but the last
            got += data.at(i).mid(1);
        }
        QCOMPARE(got.size(), expected.size());
        QVERIFY(got == expected);
        const qsizetype bound = (expected.size() + f - 2) / (f - 1) + 1;
        QVERIFY2(data.size() <= bound, qPrintable(QStringLiteral("%1 frames, at most %2").arg(data.size()).arg(bound)));
    }

    // Empty files before, between and after packed data arrive as empty files, the others whole.
    void zeroLengthItemsBetweenPackedData()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        const QList<qint64> sizes = {0, 5, 0, 0, 70000, 0, 3, SecureStream::kMaxFrame - 1, 0};
        Offer offer;
        offer.dirs = {QStringLiteral("z")};
        offer.blobs.insert(QStringLiteral("blob"), randomBlob(10, 99));
        for (int i = 0; i < sizes.size(); ++i) {
            const QString path = source.filePath(QStringLiteral("z/f%1.bin").arg(i));
            writeFile(path, randomBlob(sizes.at(i), 50 + i));
            offer.files.append(Offer::File{path, QStringLiteral("z/f%1.bin").arg(i), sizes.at(i)});
        }
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.blobs.value(QStringLiteral("blob")), randomBlob(10, 99));
        for (int i = 0; i < sizes.size(); ++i) {
            const QString path = target.filePath(QStringLiteral("z/f%1.bin").arg(i));
            QVERIFY2(QFileInfo::exists(path), qPrintable(path));
            QCOMPARE(readFile(path), randomBlob(sizes.at(i), 50 + i));
        }
    }

    // A broken or hostile sender of packed frames is refused, and nothing it sent is left: data
    // past the last item (in the last frame), data before the manifest, data frames of the old
    // kind from a current peer, and an end before the announced bytes.
    void hostilePackedStream()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fb(b.get());
        QList<QByteArray> frames; // what the fake publisher answers
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                for (const QByteArray &f : std::as_const(frames))
                    stream->send(f);
            });
        });
        auto manifest = [](const QCborArray &items) {
            QCborMap m;
            m.insert(QStringLiteral("t"), QStringLiteral("manifest"));
            m.insert(QStringLiteral("pv"), wire::kProtocolVersion);
            m.insert(QStringLiteral("items"), items);
            m.insert(QStringLiteral("more"), false);
            return controlFrame(m);
        };
        const QByteArray end = controlFrame(QCborMap{{QStringLiteral("t"), QStringLiteral("end")}});
        const QCborArray items{manifestItem("file", "one.txt", 3), manifestItem("file", "two.txt", 4)};
        const QString dest = target.filePath("dl");
        auto fetch = [&] {
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), 1, dest, true, 1 << 20, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            (void)test::waitFor([&] { return done; }, 15000);
            (void)test::waitFor([&] { return !fb.isActiveTarget(dest); }, 15000);
            return result;
        };

        frames = {manifest(items), dataFrame("onetwo!"), end}; // honest, as a control
        FetchResult r = fetch();
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(readFile(dest + "/one.txt"), QByteArray("one"));
        QCOMPARE(readFile(dest + "/two.txt"), QByteArray("two!"));
        QVERIFY(fs::removeTree(dest));

        frames = {manifest(items), dataFrame("onetwo!x"), end};
        r = fetch();
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("More data than announced"));
        QVERIFY(!QFileInfo::exists(dest));

        frames = {dataFrame("one"), manifest(items), dataFrame("two!"), end};
        r = fetch();
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Data before manifest"));
        QVERIFY(!QFileInfo::exists(dest));

        frames = {manifest(items), char(2) + QByteArray("onetwo!"), end};
        r = fetch();
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Data of an older transfer protocol"));
        QVERIFY(!QFileInfo::exists(dest));

        frames = {manifest(items), dataFrame("onetw"), end};
        r = fetch();
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Transfer ended early"));
        QVERIFY(!QFileInfo::exists(dest));
    }

    // A manifest too large for one frame (many files with long names) is split.
    void largeManifestsAreSplit()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        const QString longName(180, QLatin1Char('n'));
        for (int i = 0; i < 500; ++i)
            writeFile(source.filePath(QStringLiteral("many/%1-%2.txt").arg(i).arg(longName)), QByteArray::number(i));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("many")}, 1 << 30, 1000, &error));
        QCOMPARE(offer.files.size(), 500);
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        QSignalSpy progress(&fb, &FileTransfer::transferProgress);
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(QDir(target.filePath("many")).entryList(QDir::Files).size(), 500);
        QCOMPARE(readFile(target.filePath(QStringLiteral("many/499-%1.txt").arg(longName))), QByteArray("499"));
        QVERIFY(progress.isEmpty()); // a fetch without a kind is not one the user sees
    }

    // The manifest frames, put together by hand around items encoded once, are byte for byte what
    // a QCborMap per frame gives ({t, pv, items, more}), split or not.
    void manifestFramesMatchTheCborEncoding()
    {
        auto cborFrames = [](const Offer &offer) {
            QList<QCborMap> items;
            for (auto b = offer.blobs.constBegin(); b != offer.blobs.constEnd(); ++b)
                items.append(manifestItem("blob", b.key(), b.value().size()));
            for (const QString &d : offer.dirs)
                items.append(manifestItem("dir", d, 0));
            for (const Offer::File &f : offer.files)
                items.append(manifestItem("file", f.relativePath, f.size));
            QList<QByteArray> frames;
            QCborArray chunk;
            qsizetype chunkBytes = 0;
            auto flush = [&](bool more) {
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"), chunk);
                manifest.insert(QStringLiteral("more"), more);
                frames.append(controlFrame(manifest));
                chunk = QCborArray();
                chunkBytes = 0;
            };
            for (const QCborMap &m : items) {
                const qsizetype bytes = QCborValue(m).toCbor().size();
                if (!chunk.isEmpty() && chunkBytes + bytes > SecureStream::kMaxFrame - 64) // kManifestSlack
                    flush(true);
                chunk.append(m);
                chunkBytes += bytes;
            }
            flush(false);
            return frames;
        };
        Offer small;
        small.blobs.insert(QStringLiteral("text/plain"), QByteArray("hello"));
        small.blobs.insert(QStringLiteral("image/png"), QByteArray(300000, 'p'));
        small.dirs = QStringList{QStringLiteral("folder"), QStringLiteral("folder/empty")};
        small.files.append(Offer::File{QStringLiteral("/x/a.txt"), QStringLiteral("folder/a.txt"), 1});
        small.files.append(Offer::File{QStringLiteral("/x/b.bin"), QStringLiteral("folder/été.bin"), 5000000000});
        Offer large = syntheticTree(QStringLiteral("/x/one.txt"), 70000, 3000, 20000);
        large.blobs.insert(QStringLiteral("text/uri-list"), QByteArray(40, 'u'));
        const Offer empty;
        for (const Offer *offer : std::initializer_list<const Offer *>{&small, &large, &empty}) {
            const QList<QByteArray> expected = cborFrames(*offer);
            const QList<QByteArray> frames = FileTransfer::manifestFramesForTests(*offer);
            QCOMPARE(frames.size(), expected.size());
            for (qsizetype i = 0; i < frames.size(); ++i) {
                QVERIFY(frames.at(i).size() <= SecureStream::kMaxFrame);
                QCOMPARE(frames.at(i), expected.at(i));
            }
        }
        QVERIFY(FileTransfer::manifestFramesForTests(large).size() > 1); // the split case is tested
    }

    // A fetch with a kind is one the user sees: its progress is reported (rate-limited, once when
    // the manifest arrives, and complete at the end) and its end, and it can be cancelled.
    void visibleFetchesReportProgressAndCanBeCancelled()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("folder/one.bin"), randomBytes(2 * 1024 * 1024, 6));
        writeFile(source.filePath("folder/two.bin"), randomBytes(1024 * 1024, 7));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("folder")}, 1 << 30, 100, &error));
        const quint64 id = fa.publish(offer);

        QSignalSpy progress(&fb, &FileTransfer::transferProgress);
        QSignalSpy finished(&fb, &FileTransfer::transferFinished);
        bool done = false;
        FetchResult result;
        const quint64 handle = fb.fetch(a->id(), id, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        }, QStringLiteral("clipboard"));
        QVERIFY(fb.isVisibleFetch(handle));
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(!fb.isVisibleFetch(handle));
        QVERIFY(progress.size() >= 2); // connecting, then the manifest, then complete
        const QList<QVariant> last = progress.last();
        QCOMPARE(last.at(0).toULongLong(), handle);
        QCOMPARE(last.at(1).toByteArray(), a->id());
        QCOMPARE(last.at(2).toString(), QString("clipboard"));
        QCOMPARE(last.at(3).toLongLong(), qint64(3 * 1024 * 1024)); // done
        QCOMPARE(last.at(4).toLongLong(), qint64(3 * 1024 * 1024)); // total
        QCOMPARE(last.at(5).toInt(), 2);                             // files done
        QCOMPARE(last.at(6).toInt(), 2);                             // files announced
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(3).toString(), QString("done"));

        // Cancelled by the user: "cancelled", the callback says so, nothing is left behind.
        progress.clear();
        finished.clear();
        done = false;
        const quint64 second = fb.fetch(a->id(), id, target.filePath("dl2"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        }, QStringLiteral("drag"));
        fb.cancel(second);
        QVERIFY(done);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Cancelled"));
        QVERIFY(result.cancelled);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(3).toString(), QString("cancelled"));
        QTRY_VERIFY(!QFileInfo::exists(target.filePath("dl2")));
    }

    // The receiver's disk work runs on a worker thread, as the user in batches: the thread that
    // owns the FileTransfer (the service's main thread) never impersonates, and far fewer guards
    // are taken than files are written.
    void receiverWorkIsOffTheMainThread()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<int> calls{0};
        std::mutex threadsMutex;
        std::set<std::thread::id> threads;
        FileTransfer fa(a.get()), fb(b.get());
        fb.setImpersonator([&] {
            calls++;
            std::lock_guard<std::mutex> lock(threadsMutex);
            threads.insert(std::this_thread::get_id());
            return std::shared_ptr<void>(std::make_shared<int>(0));
        });
        writeFile(source.filePath("one.txt"), QByteArray(100, 'x'));
        constexpr int kFiles = 2000;
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 100, 400, kFiles));

        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 120000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath(QStringLiteral("dl/tree/d%1/f%2.txt").arg((kFiles - 1) % 400).arg(kFiles - 1))),
                 QByteArray(100, 'x'));
        QCOMPARE(QDir(target.filePath("dl/tree")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 400);
        qInfo() << "Impersonations:" << calls.load() << "for" << kFiles << "files";
        QVERIFY(calls.load() > 0);
        QVERIFY2(calls.load() < kFiles / 4, "impersonated per file rather than per batch");
        std::lock_guard<std::mutex> lock(threadsMutex);
        QVERIFY(!threads.count(std::this_thread::get_id()));
    }

    // Nobody signed in any more in the middle of a transfer: it stops at the next batch.
    void impersonatorRevokedMidTransfer()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<bool> revoked{false};
        FileTransfer fa(a.get()), fb(b.get());
        fb.setImpersonator([&] { return revoked ? std::shared_ptr<void>() : std::shared_ptr<void>(std::make_shared<int>(0)); });
        fb.setWriteDelayForTests(5);
        writeFile(source.filePath("big.bin"), QByteArray(8 * 1024 * 1024, 'r'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        const QString dest = target.filePath("dl");
        fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || QFileInfo::exists(QDir(dest).filePath("big.bin")), 20000);
        QVERIFY(!done);
        revoked = true;
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("Nobody is signed in to receive files"));
    }

    // The console user changes in the middle of a transfer: it stops, and deletes nothing (it
    // would be deleting as the new user).
    void userChangeMidTransferStopsWithoutCleanup()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<bool> switched{false};
        FileTransfer fa(a.get()), fb(b.get());
        fb.setUserIdentity([&] { return switched ? QStringLiteral("S-1-5-21-2") : QStringLiteral("S-1-5-21-1"); });
        fb.setWriteDelayForTests(5);
        writeFile(source.filePath("big.bin"), QByteArray(8 * 1024 * 1024, 'u'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        const QString dest = target.filePath("dl");
        fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || QFileInfo::exists(QDir(dest).filePath("big.bin")), 20000);
        QVERIFY(!done);
        switched = true;
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("The signed-in user changed"));
        QTRY_VERIFY(!fb.isActiveTarget(dest)); // the worker is done
        QVERIFY(QFileInfo::exists(QDir(dest).filePath("big.bin")));
        QVERIFY(QFileInfo(QDir(dest).filePath("big.bin")).size() < 8 * 1024 * 1024);
    }

    // A slow disk: the stream is paused while the worker's queue is full, so what waits in memory
    // stays bounded, and the file arrives intact.
    void receiverBackPressurePausesReads()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriteDelayForTests(200); // a very slow disk until reading was paused
        // More than the writers' budget and the receive queue hold together.
        const QByteArray content = randomBlob(24 * 1024 * 1024, 21);
        writeFile(source.filePath("big.bin"), content);
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || fb.readPausesForTests() > 0, 30000);
        QVERIFY(!done);
        QTest::qWait(500); // held there: the queue stays bounded while the sender waits
        fb.setWriteDelayForTests(1);
        QTRY_VERIFY_WITH_TIMEOUT(done, 120000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(QCryptographicHash::hash(readFile(target.filePath("dl/big.bin")), QCryptographicHash::Sha256),
                 QCryptographicHash::hash(content, QCryptographicHash::Sha256));
        qInfo() << "Read pauses:" << fb.readPausesForTests() << "peak queued:" << fb.peakQueuedBytesForTests();
        QVERIFY(fb.peakQueuedBytesForTests() >= FileTransfer::kReceiveQueueHigh);
        QVERIFY(fb.peakQueuedBytesForTests() <= FileTransfer::kReceiveQueueHigh + SecureStream::kMaxFrame + 1);
        // The writers' queues stay within their budget too (one frame's worth of slack).
        QVERIFY(fb.peakWriterQueuedForTests() <= FileTransfer::kWriterQueueHigh + SecureStream::kMaxFrame);
    }

    // --- Parallel writers ----------------------------------------------------------------------

    // Many small files in many folders, written by four writers: every one arrives whole.
    void parallelWritersManySmallFiles()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriterThreadsForTests(4);
        Offer offer;
        offer.dirs.append(QStringLiteral("tree"));
        for (int d = 0; d < 200; ++d)
            offer.dirs.append(QStringLiteral("tree/d%1").arg(d));
        for (int f = 0; f < 2000; ++f) {
            const QString rel = QStringLiteral("tree/d%1/f%2.bin").arg(f % 200).arg(f);
            const qint64 size = (f * 7919) % 20000;
            writeFile(source.filePath(rel), randomBlob(size, quint32(f)));
            offer.files.append(Offer::File{source.filePath(rel), rel, size});
        }
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        QSignalSpy progress(&fb, &FileTransfer::transferProgress);
        fb.fetch(a->id(), id, target.path(), true, 1 << 30,
                 [&](const FetchResult &r) {
                     result = r;
                     done = true;
                 },
                 QStringLiteral("clipboard"));
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(fb.lastWriterCountForTests(), 4);
        QCOMPARE(result.files, QStringList({target.filePath("tree")}));
        for (int f = 0; f < 2000; ++f) {
            const QString rel = QStringLiteral("tree/d%1/f%2.bin").arg(f % 200).arg(f);
            QCOMPARE(readFile(target.filePath(rel)), randomBlob((f * 7919) % 20000, quint32(f)));
        }
        // The last progress counts every file.
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(5).toInt(), 2000);
        QCOMPARE(progress.last().at(6).toInt(), 2000);
    }

    // The bytes of each file are written in order by one writer, whatever the others do: files
    // of every size (empty, inside one frame, across many) on a slow disk, checked by hash.
    void parallelWritersKeepPerFileOrder()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriterThreadsForTests(4);
        fb.setWriteDelayForTests(1);
        Offer offer;
        QList<QByteArray> hashes;
        for (int f = 0; f < 80; ++f) {
            const qint64 size = f % 9 == 0 ? 0 : (f % 3 == 0 ? 300000 + f * 1013 : f * 977);
            const QByteArray content = randomBlob(size, 1000 + f);
            const QString rel = QStringLiteral("o/%1.bin").arg(f);
            writeFile(source.filePath(rel), content);
            offer.files.append(Offer::File{source.filePath(rel), rel, size});
            hashes.append(QCryptographicHash::hash(content, QCryptographicHash::Sha256));
        }
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QVERIFY2(result.ok, qPrintable(result.error));
        for (int f = 0; f < 80; ++f) {
            QCOMPARE(QCryptographicHash::hash(readFile(target.filePath(QStringLiteral("o/%1.bin").arg(f))),
                                              QCryptographicHash::Sha256),
                     hashes.at(f));
        }
    }

    // The same path twice in a manifest (two files of one name copied from different folders):
    // both go to one writer, in order, so the second replaces the first whole, never both written
    // into one file at once.
    void parallelWritersSamePathTwice()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriterThreadsForTests(4);
        fb.setWriteDelayForTests(1);
        const QByteArray first = randomBlob(2 * 1024 * 1024, 71);
        const QByteArray second = randomBlob(100000, 72);
        writeFile(source.filePath("one/same.bin"), first);
        writeFile(source.filePath("two/same.bin"), second);
        Offer offer;
        offer.files.append(Offer::File{source.filePath("one/same.bin"), QStringLiteral("same.bin"), first.size()});
        for (int f = 0; f < 8; ++f) { // others between, so the least busy writer would differ
            const QString rel = QStringLiteral("o%1.bin").arg(f);
            writeFile(source.filePath(rel), randomBlob(5000, 80 + f));
            offer.files.append(Offer::File{source.filePath(rel), rel, 5000});
        }
        offer.files.append(Offer::File{source.filePath("two/same.bin"), QStringLiteral("same.bin"), second.size()});
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.path(), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath("same.bin")), second);
        for (int f = 0; f < 8; ++f)
            QCOMPARE(readFile(target.filePath(QStringLiteral("o%1.bin").arg(f))), randomBlob(5000, 80 + f));
    }

    // One writer fails (a folder sits where its file goes): the error names the file, the other
    // writers stop, and what was written is removed once the fetch is over.
    void parallelWriterFailureStopsAllAndCleansUp()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriterThreadsForTests(4);
        writeFile(source.filePath("one.txt"), QByteArray(3000, 'x'));
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 3000, 10, 400));
        const QString dest = target.filePath("dl");
        QVERIFY(QDir().mkpath(dest + "/tree/d7/f257.txt")); // f257 goes into d7
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("A received file could not be written: tree/d7/f257.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(dest), 20000);
        QVERIFY(!QFileInfo::exists(dest));
    }

    // Cancelled while four writers are at work on a slow disk: the callback runs at once, the
    // writers stop, and nothing is left.
    void cancelWithParallelWritersCleansUp()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriterThreadsForTests(4);
        fb.setWriteDelayForTests(20);
        writeFile(source.filePath("one.txt"), QByteArray(2000, 'c'));
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 2000, 20, 2000));
        const QString dest = target.filePath("dl");
        FetchResult result;
        bool done = false;
        const quint64 h = fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || QFileInfo::exists(dest + "/tree/d3/f3.txt"), 20000);
        QVERIFY(!done);
        QElapsedTimer t;
        t.start();
        fb.cancel(h);
        QVERIFY(done);
        QVERIFY(result.cancelled);
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(dest), 10000);
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!QFileInfo::exists(dest));
    }

    // The console user changes while four writers write: every writer stops (the change seen by
    // one is seen by all), the fetch fails, and nothing is deleted as somebody else.
    void userChangeMidTransferStopsAllWriters()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<bool> switched{false};
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriterThreadsForTests(4);
        fb.setUserIdentity([&] { return switched ? QStringLiteral("S-1-5-21-2") : QStringLiteral("S-1-5-21-1"); });
        fb.setWriteDelayForTests(10);
        writeFile(source.filePath("one.txt"), QByteArray(2000, 'u'));
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 2000, 20, 2000));
        const QString dest = target.filePath("dl");
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || QFileInfo::exists(dest + "/tree/d3/f3.txt"), 20000);
        QVERIFY(!done);
        switched = true;
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY(!result.ok);
        QCOMPARE(result.error, QString("The signed-in user changed"));
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(dest), 10000);
        QVERIFY(QFileInfo::exists(dest + "/tree/d3/f3.txt")); // not deleted as somebody else
    }

    // A fetch of a few files, or of large ones, has one writer; many small ones several.
    void singleWriterForFewOrLargeFiles()
    {
        if (qEnvironmentVariableIntValue("DESKFLOWER_TEST_WRITERS") > 0)
            QSKIP("The number of writers is forced for this run");
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        auto fetchOf = [&](const Offer &offer, const QString &dest) {
            const quint64 id = fa.publish(offer);
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), id, target.filePath(dest), true, qint64(1) << 40, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            (void)test::waitFor([&] { return done; }, 60000);
            return result.ok;
        };
        writeFile(source.filePath("small.txt"), QByteArray(100, 's'));
        writeFile(source.filePath("large.bin"), QByteArray(2 * 1024 * 1024, 'l'));
        QVERIFY(fetchOf(syntheticTree(source.filePath("small.txt"), 100, 2, 10), "few"));
        QCOMPARE(fb.lastWriterCountForTests(), 1);
        QVERIFY(fetchOf(syntheticTree(source.filePath("large.bin"), 2 * 1024 * 1024, 2, 80), "large"));
        QCOMPARE(fb.lastWriterCountForTests(), 1);
        QVERIFY(fetchOf(syntheticTree(source.filePath("small.txt"), 100, 4, 200), "many"));
        QVERIFY(fb.lastWriterCountForTests() >= 2);
        QVERIFY(fb.lastWriterCountForTests() <= FileTransfer::kMaxWriters);
    }

    // Destroying the receiver with fetches whose writers are at work on a slow disk returns within
    // its bounded wait.
    void destructorWithLiveParallelJobsReturnsQuickly()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get());
        auto fb = std::make_unique<FileTransfer>(b.get());
        fb->setWriterThreadsForTests(4);
        fb->setWriteDelayForTests(30);
        writeFile(source.filePath("one.txt"), QByteArray(2000, 'd'));
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 2000, 20, 2000));
        bool called = false;
        fb->fetch(a->id(), id, target.filePath("one"), true, 1 << 30, [&](const FetchResult &) { called = true; });
        fb->fetch(a->id(), id, target.filePath("two"), true, 1 << 30, [&](const FetchResult &) { called = true; });
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target.filePath("one/tree/d3/f3.txt"))
                                     && QFileInfo::exists(target.filePath("two/tree/d3/f3.txt")),
                                 20000);
        QElapsedTimer t;
        t.start();
        fb.reset();
        QVERIFY2(t.elapsed() < FileTransfer::kShutdownWaitMs + 1000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!called);
        QTest::qWait(500); // the writers end alone, touching nothing that is gone
    }

    // The sender sends everything, the end included, and closes while the receiver is paused:
    // nothing is lost, the end neither (the worker, not the closed stream, ends the fetch).
    void endFrameNotLostWhenSenderClosesWhilePaused()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fb(b.get());
        fb.setWriteDelayForTests(200); // a very slow disk until reading was paused
        // More than the writers' budget and the receive queue hold together.
        const QByteArray content = randomBlob(24 * 1024 * 1024, 22);
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"), QCborArray{manifestItem("file", "big.bin", content.size())});
                manifest.insert(QStringLiteral("more"), false);
                stream->send(controlFrame(manifest));
                for (qsizetype off = 0; off < content.size(); off += FileTransfer::kChunk)
                    stream->send(dataFrame(content.mid(off, FileTransfer::kChunk)));
                QCborMap end;
                end.insert(QStringLiteral("t"), QStringLiteral("end"));
                stream->send(controlFrame(end));
                stream->close();
            });
        });
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), 1, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || fb.readPausesForTests() > 0, 30000);
        QVERIFY(!done);
        QTest::qWait(1000); // the sender has sent everything it could and closed meanwhile
        fb.setWriteDelayForTests(1);
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath("dl/big.bin")), content);
    }

    // The worker's preparation (making room) may take longer than the idle timeout: the timer
    // only runs from the get on. Nor does the publisher wait through it: the stream (which the
    // publisher holds under its handshake timeout until the get, SecureStream::kHandshakeTimeoutMs)
    // is opened only once the preparation is over.
    void slowPrepareDoesNotTimeOut()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setIdleTimeoutForTests(300); // 30 s scaled down: the preparation takes "35 s"
        writeFile(source.filePath("doc.txt"), "prepared");
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("doc.txt")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        std::atomic<bool> prepared{false};
        int streamsDuringPrepare = 0;
        int streams = 0;
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *) {
            ++streams;
            if (!prepared.load())
                ++streamsDuringPrepare;
        });
        FetchOptions options;
        options.prepareAsUser = [&prepared] {
            std::this_thread::sleep_for(std::chrono::milliseconds(350 * 3));
            prepared.store(true);
        };
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        }, options);
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath("dl/doc.txt")), QByteArray("prepared"));
        QCOMPARE(streamsDuringPrepare, 0);
        QCOMPARE(streams, 1);
    }

    // A cancel in the middle of creating a large folder tree stops the worker at the next folder
    // and removes what it created.
    void cancelMidFolderTreeStopsQuickly()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("one.txt"), "1");
        constexpr int kDirs = 50000;
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 1, kDirs, 1));

        FetchResult result;
        bool done = false;
        const QString dest = target.filePath("dl");
        const quint64 handle = fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        const QDir tree(QDir(dest).filePath("tree"));
        QTRY_VERIFY_WITH_TIMEOUT(done || tree.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >= 300, 60000);
        QVERIFY(!done);
        QElapsedTimer timer;
        timer.start();
        fb.cancel(handle);
        QVERIFY(done);
        QVERIFY(result.cancelled);
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(dest), 30000);
        qInfo() << "Stopped and cleaned up in" << timer.elapsed() << "ms";
        QVERIFY(!QFileInfo::exists(dest));
        QVERIFY(timer.elapsed() < 20000);
    }

    // A cancel racing the worker's success: whichever settles first, nothing is left behind and
    // the callback says "cancelled".
    void cancelRacingSuccessCleansUp()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        constexpr int kSize = 200 * 1024;
        writeFile(source.filePath("a.bin"), randomBlob(kSize, 23));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("a.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        // before: the cancel wins, the worker cleans up; after: the worker won, its late success
        // is cleaned up.
        for (const bool cancelWins : {true, false}) {
            fb.setFinishPausesForTests(cancelWins ? 800 : 0, cancelWins ? 0 : 800);
            const QString dest = target.filePath(cancelWins ? "first" : "second");
            FetchResult result;
            bool done = false;
            const quint64 handle = fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(QDir(dest).filePath("a.bin")).size() == kSize, 20000);
            if (!cancelWins)
                QTest::qWait(200); // the end is handled: the worker settled it and pauses
            QVERIFY(!done);
            fb.cancel(handle);
            QVERIFY(done);
            QVERIFY(result.cancelled);
            // Counted as at work until the cleanup is over (both ways, done by the worker): a
            // prune or another fetch leaves the folder alone meanwhile.
            QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(dest), 10000);
            QVERIFY2(!QFileInfo::exists(dest), qPrintable(dest));
        }
    }

    // A failed query of the user's identity (empty) is neither recorded as the first user nor
    // taken for a change of user; a real change still is.
    void emptyIdentityIsUnknown()
    {
        QStringList answers{"", "", "S-1-5-21-A", "", "", "S-1-5-21-A", "S-1-5-21-B"};
        int asked = 0;
        detail::AsUserBatch batch([] { return std::shared_ptr<void>(std::make_shared<int>(0)); },
                                  [&] { return asked < answers.size() ? answers.at(asked++) : QString(); });
        using Status = detail::AsUserBatch::Status;
        QCOMPARE(batch.acquire(), Status::Ok); // "", "": unknown, not recorded
        batch.release();
        QCOMPARE(batch.acquire(), Status::Ok); // A: recorded
        batch.release();
        QCOMPARE(batch.acquire(), Status::Ok); // "", "": unknown, no change
        batch.release();
        QCOMPARE(batch.acquire(), Status::Ok); // A again
        batch.release();
        QCOMPARE(batch.acquire(), Status::UserChanged); // B
        QVERIFY(batch.userChanged());
        QCOMPARE(asked, answers.size());
    }

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // On a file system that ignores case, two paths that differ only in case would be the same
    // file (the second truncating the first): refused before anything is written.
    void caseOnlyDuplicatesAreRefused()
    {
        QTemporaryDir dirA, dirB, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fb(b.get());
        QCborArray items;
        QList<QByteArray> data;
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"), items);
                manifest.insert(QStringLiteral("more"), false);
                stream->send(controlFrame(manifest));
                for (const QByteArray &chunk : std::as_const(data))
                    stream->send(dataFrame(chunk));
                QCborMap end;
                end.insert(QStringLiteral("t"), QStringLiteral("end"));
                stream->send(controlFrame(end));
            });
        });
        auto fetchInto = [&](const QString &dest) {
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), 1, dest, true, 1 << 30, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            (void)test::waitFor([&] { return done; }, 15000);
            return result;
        };

        items = {manifestItem("dir", "Photos", 0), manifestItem("file", "Photos/a.jpg", 3),
                 manifestItem("file", "photos/A.JPG", 3)};
        data = {"abc", "def"};
        FetchResult r = fetchInto(target.filePath("files"));
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QString("Two copied items differ only in upper and lower case: photos/A.JPG"));
        QVERIFY(!QFileInfo::exists(target.filePath("files")));

        items = {manifestItem("dir", "Docs", 0), manifestItem("dir", "docs", 0)};
        data = {};
        r = fetchInto(target.filePath("folders"));
        QVERIFY(!r.ok);
        QVERIFY2(r.error.startsWith("Two copied items differ only in upper and lower case"), qPrintable(r.error));

        // The same path twice is no clash (two copied folders of the same name).
        items = {manifestItem("dir", "Docs", 0), manifestItem("dir", "Docs", 0), manifestItem("file", "Docs/x.txt", 1)};
        data = {"x"};
        r = fetchInto(target.filePath("same"));
        QVERIFY2(r.ok, qPrintable(r.error));
    }
#endif

    // The free space a fetch counts on excludes what the other fetches still have to write.
    void freeSpaceCountsOtherLiveJobs()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        constexpr qint64 kMB = 1024 * 1024;
        // The reserve kept free (64 MB), plus 10 MB: room for one 6 MB copy, not two.
        fb.setFreeSpaceForTests([](const QString &) { return 64 * kMB + 10 * kMB; });
        fb.setWriteDelayForTests(50);
        writeFile(source.filePath("six.bin"), QByteArray(int(6 * kMB), 's'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("six.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        QSignalSpy progress(&fb, &FileTransfer::transferProgress);
        bool firstDone = false;
        const QString first = target.filePath("first");
        const quint64 h1 = fb.fetch(a->id(), id, first, true, 1 << 30, [&](const FetchResult &) { firstDone = true; },
                                    QStringLiteral("clipboard"));
        auto manifestIn = [&] {
            for (const QList<QVariant> &p : std::as_const(progress)) {
                if (p.at(0).toULongLong() == h1 && p.at(4).toLongLong() == 6 * kMB)
                    return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(manifestIn(), 20000);

        FetchResult result;
        bool done = false;
        auto cb = [&](const FetchResult &r) {
            result = r;
            done = true;
        };
        fb.fetch(a->id(), id, target.filePath("second"), true, 1 << 30, cb);
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY(!result.ok);
        QVERIFY2(result.error.startsWith("Not enough free disk space"), qPrintable(result.error));
        QVERIFY(!firstDone);

        fb.cancel(h1);
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(first), 20000);
        fb.setWriteDelayForTests(0);
        done = false;
        fb.fetch(a->id(), id, target.filePath("third"), true, 1 << 30, cb);
        QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
        QVERIFY2(result.ok, qPrintable(result.error));
    }

    // Both ends in one process share one thread, as the service's main thread routes input and
    // keepalives: 20,000 files in 4,000 folders cross while a 10 ms timer on it keeps ticking.
    // Serving and receiving read and write on threads of their own (the user's rights are never
    // taken on this thread), so no gap comes near a session's keepalive timeout.
    void mainThreadStaysResponsiveDuringLargeTransfer()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        const std::thread::id mainThread = std::this_thread::get_id();
        std::atomic<int> onMain{0};
        std::atomic<int> calls{0};
        auto counting = [&] {
            calls++;
            if (std::this_thread::get_id() == mainThread)
                onMain++;
            return std::shared_ptr<void>(std::make_shared<int>(0));
        };
        FileTransfer fa(a.get()), fb(b.get());
        fa.setImpersonator(counting);
        fb.setImpersonator(counting);
        writeFile(source.filePath("one.txt"), QByteArray(64, 'm'));
        constexpr int kFiles = 20000;
        constexpr int kDirs = 4000;
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 64, kDirs, kFiles));

        QList<qint64> gaps;
        gaps.reserve(100000);
        QElapsedTimer sinceTick;
        QTimer ticker;
        ticker.setTimerType(Qt::PreciseTimer);
        ticker.setInterval(10);
        connect(&ticker, &QTimer::timeout, this, [&] { gaps.append(sinceTick.restart()); });
        FetchResult result;
        bool done = false;
        QElapsedTimer total;
        total.start();
        sinceTick.start();
        ticker.start();
        fb.fetch(a->id(), id, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 300000);
        ticker.stop();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(target.filePath(QStringLiteral("dl/tree/d%1/f%2.txt").arg((kFiles - 1) % kDirs).arg(kFiles - 1))),
                 QByteArray(64, 'm'));
        QCOMPARE(QDir(target.filePath("dl/tree")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), kDirs);
        QVERIFY(!gaps.isEmpty());
        std::sort(gaps.begin(), gaps.end());
        const qint64 largest = gaps.last();
        const qint64 p99 = gaps.at(std::min<qsizetype>(gaps.size() - 1, gaps.size() * 99 / 100));
        qInfo() << "Transfer took" << total.elapsed() << "ms;" << gaps.size() << "ticks, 99th percentile gap" << p99
                << "ms, largest" << largest << "ms;" << calls.load() << "impersonations";
        QVERIFY2(largest < 1000, qPrintable(QStringLiteral("the main thread stalled for %1 ms").arg(largest)));
        QVERIFY(calls.load() > 0);
        QCOMPARE(onMain.load(), 0);
    }

    // A list fetch brings the files and folders of an offer (19,300 entries: a split manifest),
    // folders first, and nothing else: it writes nothing, takes no user's rights, and drops the
    // stream once the manifest is in, so the publisher's upload ends long before its data would.
    void listFetchReturnsManifestAndWritesNothing()
    {
        QTemporaryDir dirA, dirB, source;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<int> receiverImpersonations{0};
        FileTransfer fa(a.get()), fb(b.get());
        fb.setImpersonator([&] {
            receiverImpersonations++;
            return std::shared_ptr<void>(std::make_shared<int>(0));
        });
        constexpr qint64 kSize = 256 * 1024;
        writeFile(source.filePath("one.bin"), QByteArray(int(kSize), 'l'));
        constexpr int kDirs = 299, kFiles = 19000; // plus "tree": 19,300 entries
        Offer offer = syntheticTree(source.filePath("one.bin"), kSize, kDirs, kFiles);
        offer.blobs.insert(QStringLiteral("image/png"), QByteArray(1000, 'p')); // not a listed item
        const quint64 id = fa.publish(offer);
        QVERIFY(FileTransfer::manifestFramesForTests(offer).size() > 1);

        bool done = false;
        bool ok = false;
        bool refused = false;
        QString error;
        QList<ListedFile> items;
        auto cb = [&](bool o, bool r, const QString &e, const QList<ListedFile> &l) {
            done = true;
            ok = o;
            refused = r;
            error = e;
            items = l;
        };
        fb.fetchList(a->id(), id, qint64(1) << 40, cb);
        QVERIFY(!done); // never inside the call
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(ok, qPrintable(error));
        QCOMPARE(items.size(), 1 + kDirs + kFiles);
        QCOMPARE(items.first().path, QString("tree"));
        QVERIFY(items.first().dir);
        QCOMPARE(items.at(kDirs).path, QStringLiteral("tree/d%1").arg(kDirs - 1));
        QVERIFY(items.at(kDirs).dir);
        const ListedFile &last = items.last();
        QCOMPARE(last.path, QStringLiteral("tree/d%1/f%2.txt").arg((kFiles - 1) % kDirs).arg(kFiles - 1));
        QCOMPARE(last.size, kSize);
        QVERIFY(!last.dir);
        // The publisher's upload (4.6 GB announced) is released, and nothing was written here.
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
        QCOMPARE(receiverImpersonations.load(), 0); // no disk access as the user: no folder, no file
        QCOMPARE(fb.peakQueuedBytesForTests(), 0);  // no frame ever went to a fetch's worker

        // The manifest gets a fetch's checks: sizes beyond the bound are refused.
        done = false;
        fb.fetchList(a->id(), id, kSize, cb);
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY(!ok);
        QVERIFY(refused); // the manifest itself: asking again would not change it
        QCOMPARE(error, QString("Transfer larger than allowed"));
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);

        // An unknown offer: the publisher's refusal (not the manifest's).
        done = false;
        fb.fetchList(a->id(), id + 1, qint64(1) << 40, cb);
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY(!ok);
        QVERIFY(!refused);
        QCOMPARE(error, QString("No such offer"));

        // Cancelled: the callback never runs, and the upload goes too.
        done = false;
        const quint64 handle = fb.fetchList(a->id(), id, qint64(1) << 40, cb);
        fb.cancelList(handle);
        QTest::qWait(500);
        QVERIFY(!done);
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
    }

    // A list fetch that cannot complete reports once, after the call returns, and tells a failure
    // of the way there (unreachable, closed halfway through a split manifest, an end before the
    // manifest, silence) from a manifest refused as such (data before it, two paths that differ
    // only in case where case is ignored).
    void listFetchFailuresReportOnce()
    {
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fb(b.get());
        fb.setIdleTimeoutForTests(400);
        enum Mode { CloseHalfway, EndEarly, Silent, DataFirst, CaseTwins };
        Mode mode = CloseHalfway;
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1)
                    return;
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                QCborMap end;
                end.insert(QStringLiteral("t"), QStringLiteral("end"));
                switch (mode) {
                case CloseHalfway:
                    manifest.insert(QStringLiteral("items"), QCborArray{manifestItem("file", "a.txt", 1)});
                    manifest.insert(QStringLiteral("more"), true);
                    stream->send(controlFrame(manifest));
                    stream->abort(QStringLiteral("gone"));
                    break;
                case EndEarly:
                    manifest.insert(QStringLiteral("items"), QCborArray{manifestItem("file", "a.txt", 1)});
                    manifest.insert(QStringLiteral("more"), true);
                    stream->send(controlFrame(manifest));
                    stream->send(controlFrame(end));
                    break;
                case Silent:
                    break;
                case DataFirst:
                    stream->send(dataFrame("x"));
                    break;
                case CaseTwins:
                    manifest.insert(QStringLiteral("items"), QCborArray{manifestItem("file", "dir/a.txt", 1),
                                                                        manifestItem("file", "dir/A.txt", 1)});
                    stream->send(controlFrame(manifest));
                    stream->send(dataFrame("xy"));
                    stream->send(controlFrame(end));
                    break;
                }
            });
        });
        int calls = 0;
        bool ok = false;
        bool refused = false;
        QString error;
        QList<ListedFile> items;
        auto run = [&](const QByteArray &peer) {
            calls = 0;
            fb.fetchList(peer, 7, 1 << 20, [&](bool o, bool r, const QString &e, const QList<ListedFile> &l) {
                ++calls;
                ok = o;
                refused = r;
                error = e;
                items = l;
            });
            if (calls != 0)
                return false; // never inside the call
            if (!test::waitFor([&] { return calls > 0; }, 15000))
                return false;
            QTest::qWait(600); // past the idle timeout: no second report
            return calls == 1;
        };

        QVERIFY(run(QByteArray(32, 'z'))); // no such peer
        QVERIFY(!ok);
        QVERIFY(!refused);
        QCOMPARE(error, QString("Peer not reachable"));

        mode = CloseHalfway;
        QVERIFY(run(a->id()));
        QVERIFY(!ok);
        QVERIFY(!refused);

        mode = EndEarly;
        QVERIFY(run(a->id()));
        QVERIFY(!ok);
        QVERIFY(!refused);
        QCOMPARE(error, QString("Transfer ended early"));

        mode = Silent;
        QVERIFY(run(a->id()));
        QVERIFY(!ok);
        QVERIFY(!refused);
        QCOMPARE(error, QString("Transfer timed out"));

        mode = DataFirst;
        QVERIFY(run(a->id()));
        QVERIFY(!ok);
        QVERIFY(refused);
        QCOMPARE(error, QString("Data before manifest"));

        mode = CaseTwins;
        QVERIFY(run(a->id()));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QVERIFY(!ok);
        QVERIFY(refused);
        QVERIFY2(error.startsWith("Two copied items differ only in upper and lower case"), qPrintable(error));
#else
        QVERIFY2(ok, qPrintable(error));
        QCOMPARE(items.size(), 2);
#endif
    }

    // A FileTransfer destroyed with a list fetch in progress (half a split manifest in) never runs
    // its callback, and drops the stream: the publisher sees its receiver go.
    void listFetchPendingAtDestruction()
    {
        bool gotGet = false;
        bool publisherClosed = false; // before the nodes: their streams may close as they go
        QTemporaryDir dirA, dirB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        connect(a.get(), &Node::streamAccepted, this, [&](SecureStream *stream) {
            connect(stream, &SecureStream::closed, this, [&] { publisherClosed = true; });
            connect(stream, &SecureStream::received, stream, [&, stream](const QByteArray &frame) {
                if (frame.isEmpty() || quint8(frame[0]) != 1 || gotGet)
                    return;
                gotGet = true;
                QCborMap manifest;
                manifest.insert(QStringLiteral("t"), QStringLiteral("manifest"));
                manifest.insert(QStringLiteral("pv"), wire::kProtocolVersion);
                manifest.insert(QStringLiteral("items"), QCborArray{manifestItem("file", "a.txt", 1)});
                manifest.insert(QStringLiteral("more"), true); // and the rest never comes
                stream->send(controlFrame(manifest));
            });
        });
        bool called = false;
        {
            FileTransfer fb(b.get());
            fb.fetchList(a->id(), 7, 1 << 20,
                         [&](bool, bool, const QString &, const QList<ListedFile> &) { called = true; });
            QTRY_VERIFY_WITH_TIMEOUT(gotGet, 15000);
            QTest::qWait(200); // the first manifest frame is in
            QVERIFY(!called);
        }
        QTRY_VERIFY_WITH_TIMEOUT(publisherClosed, 10000);
        QTest::qWait(200);
        QVERIFY(!called);
    }

    // The receiver cancels while the publisher's reader waits for room in its queue: the reader
    // stops, holds no user's rights while it waits or after, and its thread returns.
    void uploadCancelledByReceiverReleasesWorker()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<int> held{0};
        std::atomic<int> calls{0};
        FileTransfer fa(a.get()), fb(b.get());
        fa.setImpersonator([&] {
            calls++;
            held++;
            return std::shared_ptr<void>(new int(0), [&held](int *p) {
                delete p;
                held--;
            });
        });
        fb.setWriteDelayForTests(200); // a very slow disk: the receiver pauses, the publisher waits
        writeFile(source.filePath("big.bin"), randomBlob(48 * 1024 * 1024, 31));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        FetchResult result;
        bool done = false;
        const quint64 handle = fb.fetch(a->id(), id, target.filePath("dl"), true, 1 << 30, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done || fb.readPausesForTests() > 0, 30000);
        QVERIFY(!done);
        QCOMPARE(fa.liveUploadsForTests(), 1);
        QVERIFY(calls.load() > 0);
        // Held up by the receiver, the reader waits for room without the user's rights.
        QTRY_COMPARE_WITH_TIMEOUT(held.load(), 0, 10000);
        QCOMPARE(fa.liveUploadsForTests(), 1);

        fb.cancel(handle);
        QVERIFY(done);
        QVERIFY(result.cancelled);
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
        QCOMPARE(held.load(), 0);
    }

    // On the publisher, the console user changing or signing out in the middle of an upload stops
    // it at the reader's next batch, and the receiver is told why.
    void publisherUserChangeOrSignOutStopsUpload()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        std::atomic<bool> switched{false};
        std::atomic<bool> revoked{false};
        FileTransfer fa(a.get()), fb(b.get());
        fa.setImpersonator([&] { return revoked ? std::shared_ptr<void>() : std::shared_ptr<void>(std::make_shared<int>(0)); });
        fa.setUserIdentity([&] { return switched ? QStringLiteral("S-1-5-21-2") : QStringLiteral("S-1-5-21-1"); });
        // A slow receiver: the publisher's reader is still well inside the file when it changes.
        fb.setWriteDelayForTests(20);
        writeFile(source.filePath("big.bin"), QByteArray(48 * 1024 * 1024, 'p'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        auto stopsWith = [&](const QString &dest, std::atomic<bool> &flag, const QString &expected) {
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), id, dest, true, 1 << 30, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(done || QFileInfo::exists(QDir(dest).filePath("big.bin")), 20000);
            QVERIFY(!done);
            flag = true;
            QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
            QVERIFY(!result.ok);
            QCOMPARE(result.error, expected);
            QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
        };
        stopsWith(target.filePath("changed"), switched, QStringLiteral("The signed-in user changed"));
        stopsWith(target.filePath("signedOut"), revoked, QStringLiteral("Nobody is signed in"));
    }

    // One peer asking again and again for the same offer gets at most kMaxUploadsPerOfferAndPeer
    // uploads at once (each reads the whole offer on a thread of its own), then a clear error.
    void uploadsPerOfferAndPeerAreBounded()
    {
        QTemporaryDir dirA, dirB, source;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get());
        writeFile(source.filePath("big.bin"), QByteArray(32 * 1024 * 1024, 'c'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        auto get = [&](SecureStream *s) {
            QCborMap msg;
            msg.insert(QStringLiteral("t"), QStringLiteral("get"));
            msg.insert(QStringLiteral("o"), qint64(id));
            msg.insert(QStringLiteral("files"), true);
            msg.insert(QStringLiteral("v"), 3);
            msg.insert(QStringLiteral("pv"), wire::kProtocolVersion);
            s->send(controlFrame(msg));
        };
        // Receivers that read nothing: their uploads stay at work.
        std::vector<std::unique_ptr<SecureStream>> held;
        for (int i = 0; i < FileTransfer::kMaxUploadsPerOfferAndPeer; ++i) {
            held.emplace_back(b->openStream(a->id()));
            SecureStream *s = held.back().get();
            QVERIFY(s);
            connect(s, &SecureStream::established, s, [s, get] {
                get(s);
                s->setReadPaused(true);
            });
        }
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), FileTransfer::kMaxUploadsPerOfferAndPeer, 20000);

        std::unique_ptr<SecureStream> extra(b->openStream(a->id()));
        QCborMap reply;
        connect(extra.get(), &SecureStream::established, extra.get(), [&] { get(extra.get()); });
        connect(extra.get(), &SecureStream::received, extra.get(), [&](const QByteArray &frame) {
            if (reply.isEmpty() && !frame.isEmpty() && quint8(frame[0]) == 1)
                reply = QCborValue::fromCbor(frame.mid(1)).toMap();
        });
        QTRY_VERIFY_WITH_TIMEOUT(!reply.isEmpty(), 10000);
        QCOMPARE(reply.value(QStringLiteral("t")).toString(), QString("error"));
        QCOMPARE(reply.value(QStringLiteral("r")).toString(), QString("Too many transfers of these files at once"));
        QCOMPARE(fa.liveUploadsForTests(), FileTransfer::kMaxUploadsPerOfferAndPeer);

        // The receivers go away: their readers stop.
        held.clear();
        extra.reset();
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
    }

    // Destroying the publisher's FileTransfer with uploads at work, one reader waiting for room in
    // its queue (a slow receiver) and one stuck inside a read as the user: the waiting one stops at
    // once, and the destructor returns within its bounded wait even though the stuck one does not.
    void destructorWithLiveUploadsReturnsQuickly()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        auto fa = std::make_unique<FileTransfer>(a.get());
        FileTransfer fb(b.get());
        fb.setWriteDelayForTests(200);
        writeFile(source.filePath("big.bin"), QByteArray(48 * 1024 * 1024, 'w'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa->publish(offer);

        // A reader held up by the receiver: it stops at once, well inside the bounded wait.
        fb.fetch(a->id(), id, target.filePath("waiting"), true, 1 << 30, [](const FetchResult &) {});
        QTRY_VERIFY_WITH_TIMEOUT(fb.readPausesForTests() > 0, 30000);
        QCOMPARE(fa->liveUploadsForTests(), 1);
        QElapsedTimer timer;
        timer.start();
        fa.reset();
        qInfo() << "Destroyed with a waiting reader in" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() < FileTransfer::kShutdownWaitMs);

        // A reader stuck as the user (its first impersonation does not return for a while): the
        // wait ends at its bound, and the reader ends alone later. What it shares with this test
        // is held by value, never by reference: it outlives the wait.
        auto stalled = std::make_shared<std::atomic<int>>(0); // 1: in the stuck call, 2: out of it
        fa = std::make_unique<FileTransfer>(a.get());
        fa->setImpersonator([stalled] {
            int expected = 0;
            if (stalled->compare_exchange_strong(expected, 1)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(FileTransfer::kShutdownWaitMs + 1500));
                stalled->store(2);
            }
            return std::shared_ptr<void>(std::make_shared<int>(0));
        });
        const quint64 second = fa->publish(offer);
        fb.fetch(a->id(), second, target.filePath("stuck"), true, 1 << 30, [](const FetchResult &) {});
        QTRY_COMPARE_WITH_TIMEOUT(stalled->load(), 1, 20000);
        QCOMPARE(fa->liveUploadsForTests(), 1);
        timer.restart();
        fa.reset();
        qInfo() << "Destroyed with a stuck reader in" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() >= FileTransfer::kShutdownWaitMs - 100);
        QVERIFY(timer.elapsed() < FileTransfer::kShutdownWaitMs + 1000);
        QTRY_COMPARE_WITH_TIMEOUT(stalled->load(), 2, 10000);
    }

    // Destroying the FileTransfer with fetches at work (one writing slowly, one still preparing)
    // returns within its bounded wait, and calls no callback.
    void destructorWithLiveJobsReturnsQuickly()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get());
        auto fb = std::make_unique<FileTransfer>(b.get());
        fb->setWriteDelayForTests(50);
        writeFile(source.filePath("big.bin"), QByteArray(8 * 1024 * 1024, 'd'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("big.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        int called = 0;
        fb->fetch(a->id(), id, target.filePath("writing"), true, 1 << 30, [&](const FetchResult &) { ++called; });
        FetchOptions stuck;
        stuck.prepareAsUser = [] { std::this_thread::sleep_for(std::chrono::seconds(5)); };
        fb->fetch(a->id(), id, target.filePath("preparing"), true, 1 << 30, [&](const FetchResult &) { ++called; },
                  stuck);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target.filePath("writing/big.bin")), 20000);
        QElapsedTimer timer;
        timer.start();
        fb.reset();
        qInfo() << "Destroyed in" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() < FileTransfer::kShutdownWaitMs + 1500);
        QTest::qWait(100);
        QCOMPARE(called, 0);
    }

    // Writers are chosen by the number of small files, not by the average size: a tree of small
    // files with one huge file among them gets several; only large files, or a few, get one.
    void writerCountCountsSmallFiles()
    {
        auto sizes = [](int small, qint64 smallSize, int large, qint64 largeSize) {
            QList<qint64> list;
            for (int i = 0; i < small; ++i)
                list.append(smallSize);
            for (int i = 0; i < large; ++i)
                list.append(largeSize);
            return list;
        };
        const qint64 kGiB = qint64(1) << 30;
        QCOMPARE(FileTransfer::writerCountFor(sizes(0, 0, 1, 4 * kGiB)), 1);         // one huge file
        QCOMPARE(FileTransfer::writerCountFor(sizes(0, 0, 80, 2 * 1024 * 1024)), 1); // only large files
        QCOMPARE(FileTransfer::writerCountFor(sizes(10, 100, 0, 0)), 1);              // a few
        QCOMPARE(FileTransfer::writerCountFor(sizes(FileTransfer::kParallelSmallFiles - 1, 100, 1, kGiB)), 1);
        const int many = FileTransfer::writerCountFor(sizes(FileTransfer::kParallelSmallFiles, 100, 0, 0));
        QVERIFY(many >= 2 && many <= FileTransfer::kMaxWriters);
        // 15,000 small files and a 1 GB file: an average of ~70 KB here, but 1000 small files and a
        // 100 GB one average 100 MB: both get several writers now.
        QCOMPARE(FileTransfer::writerCountFor(sizes(1000, 50 * 1024, 1, 100 * kGiB)), many);
        QCOMPARE(FileTransfer::writerCountFor(sizes(15000, 50 * 1024, 1, kGiB)), many);
        QCOMPARE(FileTransfer::writerCountFor(sizes(1000, FileTransfer::kSmallFile, 0, 0)), 1); // 1 MB is not small
        QCOMPARE(FileTransfer::writerCountFor(sizes(1, 1, 1, 1), 3), 3);                         // forced
        QCOMPARE(FileTransfer::writerCountFor(sizes(1, 1, 1, 1), 99), FileTransfer::kMaxWriters);
    }

    // End to end: 70 small files and one of 80 MB (an average above 1 MB, which used to mean one
    // writer) arrive intact with several writers.
    void mixedTreeGetsParallelWriters()
    {
        if (qEnvironmentVariableIntValue("DESKFLOWER_TEST_WRITERS") > 0)
            QSKIP("The number of writers is forced for this run");
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("small.txt"), QByteArray(500, 's'));
        const QByteArray huge = randomBlob(80 * 1024 * 1024, 7);
        writeFile(source.filePath("huge.bin"), huge);
        Offer offer = syntheticTree(source.filePath("small.txt"), 500, 3, 70);
        offer.files.insert(35, Offer::File{source.filePath("huge.bin"), QStringLiteral("tree/huge.bin"), huge.size()});
        QVERIFY(offer.fileBytes() / offer.files.size() >= 1024 * 1024);
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.filePath("mixed"), true, qint64(1) << 40, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 120000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(fb.lastWriterCountForTests() >= 2);
        QVERIFY(readFile(target.filePath("mixed/tree/huge.bin")) == huge);
        QCOMPARE(readFile(target.filePath("mixed/tree/d0/f69.txt")), QByteArray(500, 's'));
    }

    // The publisher reads small files ahead on helper threads, and the data still arrives in
    // manifest order, byte for byte: files of every size (empty ones, ones read ahead, ones above
    // kReadAheadMax read by the reader itself, one larger than the read-ahead budget) interleaved.
    void readAheadKeepsManifestOrder()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        QRandomGenerator rng(42);
        QStringList paths;
        QList<QByteArray> contents;
        for (int i = 0; i < 400; ++i) {
            qsizetype size = 0;
            switch (i % 7) {
            case 0: size = 0; break;
            case 1: size = FileTransfer::kReadAheadMax; break;                 // the largest read ahead
            case 2: size = FileTransfer::kReadAheadMax + 1; break;             // the reader's own
            case 3: size = i == 3 ? 3 * 1024 * 1024 : 70000; break;            // one above the budget
            default: size = qsizetype(rng.bounded(20000)); break;
            }
            const QString rel = QStringLiteral("set/d%1/f%2.bin").arg(i % 9).arg(i);
            contents.append(randomBlob(size, quint32(i + 1)));
            writeFile(source.filePath(rel), contents.last());
            paths.append(rel);
        }
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("set")}, qint64(1) << 40, 100000, &error));
        const quint64 id = fa.publish(offer);
        FetchResult result;
        bool done = false;
        fb.fetch(a->id(), id, target.filePath("dl"), true, qint64(1) << 40, [&](const FetchResult &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 120000);
        QVERIFY2(result.ok, qPrintable(result.error));
        for (int i = 0; i < paths.size(); ++i)
            QVERIFY2(readFile(target.filePath("dl/" + paths.at(i))) == contents.at(i), qPrintable(paths.at(i)));
        const FileTransfer::Counters counters = fa.countersForTests();
        QVERIFY(counters.readAheadFiles >= 300); // every file of kReadAheadMax or less
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
    }

    // A file read ahead that shrank since it was listed fails the upload with the error the reader
    // gives for its own reads, naming it; one that grew is cut at its listed size, as before. Both
    // with and without the read-ahead.
    void readAheadFileChangedSize()
    {
        for (const bool ahead : {true, false}) {
            QTemporaryDir dirA, dirB, source, target;
            auto a = test::makeNode(dirA.path());
            auto b = test::makeNode(dirB.path());
            QVERIFY(test::pairNodes(*a, *b));
            FileTransfer fa(a.get()), fb(b.get());
            fa.setReadAheadForTests(ahead);
            for (int i = 0; i < 100; ++i)
                writeFile(source.filePath(QStringLiteral("set/f%1.txt").arg(i, 3, 10, QLatin1Char('0'))),
                          QByteArray(1000, char('a' + i % 26)));
            Offer offer;
            QString error;
            QVERIFY(Offer::addPaths(&offer, {source.filePath("set")}, qint64(1) << 40, 100000, &error));
            auto fetchOnce = [&](const QString &dest) {
                const quint64 id = fa.publish(offer);
                FetchResult result;
                bool done = false;
                fb.fetch(a->id(), id, target.filePath(dest), true, qint64(1) << 40, [&](const FetchResult &r) {
                    result = r;
                    done = true;
                });
                (void)test::waitFor([&] { return done; }, 60000);
                return result;
            };
            // Grown after listing: its first 1000 bytes are sent.
            writeFile(source.filePath("set/f040.txt"), QByteArray(5000, 'G'));
            FetchResult grown = fetchOnce("grown");
            QVERIFY2(grown.ok, qPrintable(grown.error));
            QCOMPARE(readFile(target.filePath("grown/set/f040.txt")), QByteArray(1000, 'G'));
            // Shrunk after listing: refused, named.
            writeFile(source.filePath("set/f060.txt"), QByteArray(10, 'S'));
            FetchResult shrunk = fetchOnce("shrunk");
            QVERIFY(!shrunk.ok);
            QCOMPARE(shrunk.error, QStringLiteral("File changed while sending: set/f060.txt"));
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(target.filePath("shrunk")), 20000);
            QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
        }
    }

    // A file that can no longer be opened midway through the list fails the upload naming it, with
    // and without the read-ahead; nothing after it is sent.
    void readAheadFileFailsToOpen()
    {
        for (const bool ahead : {true, false}) {
            QTemporaryDir dirA, dirB, source, target;
            auto a = test::makeNode(dirA.path());
            auto b = test::makeNode(dirB.path());
            QVERIFY(test::pairNodes(*a, *b));
            FileTransfer fa(a.get()), fb(b.get());
            fa.setReadAheadForTests(ahead);
            for (int i = 0; i < 200; ++i)
                writeFile(source.filePath(QStringLiteral("set/f%1.txt").arg(i, 3, 10, QLatin1Char('0'))),
                          QByteArray(2000, 'x'));
            Offer offer;
            QString error;
            QVERIFY(Offer::addPaths(&offer, {source.filePath("set")}, qint64(1) << 40, 100000, &error));
            QVERIFY(QFile::remove(source.filePath("set/f120.txt")));
            const quint64 id = fa.publish(offer);
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), id, target.filePath("dl"), true, qint64(1) << 40, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
            QVERIFY(!result.ok);
            QCOMPARE(result.error, QStringLiteral("A file could not be read: set/f120.txt"));
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(target.filePath("dl")), 20000);
            QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
        }
    }

    // A receiver that cancels while the publisher reads ahead: the upload's reader and its helpers
    // stop and return.
    void readAheadStopsOnCancel()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        fb.setWriteDelayForTests(20); // a slow disk: the publisher's queues fill up
        writeFile(source.filePath("one.txt"), QByteArray(30000, 'c'));
        const quint64 id = fa.publish(syntheticTree(source.filePath("one.txt"), 30000, 10, 3000));
        FetchResult result;
        bool done = false;
        const quint64 handle = fb.fetch(a->id(), id, target.filePath("dl"), true, qint64(1) << 40,
                                        [&](const FetchResult &r) {
                                            result = r;
                                            done = true;
                                        });
        QTRY_VERIFY_WITH_TIMEOUT(fa.countersForTests().readAheadFiles >= 20, 30000);
        QCOMPARE(fa.liveUploadsForTests(), 1);
        fb.cancel(handle);
        QVERIFY(done);
        QVERIFY(result.cancelled);
        QTRY_COMPARE_WITH_TIMEOUT(fa.liveUploadsForTests(), 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!fb.isActiveTarget(target.filePath("dl")), 20000);
    }

    // FetchOptions::cleanupAsUser runs after prepareAsUser, alongside the transfer (the stream is
    // asked for without waiting for it), also when the fetch is cancelled before it starts; and a
    // copy that does not fit waits for it before its free space check refuses it.
    void cleanupRunsAlongsideAndFreesSpace()
    {
        QTemporaryDir dirA, dirB, source, target;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        FileTransfer fa(a.get()), fb(b.get());
        writeFile(source.filePath("doc.bin"), QByteArray(1024 * 1024, 'd'));
        Offer offer;
        QString error;
        QVERIFY(Offer::addPaths(&offer, {source.filePath("doc.bin")}, 1 << 30, 10, &error));
        const quint64 id = fa.publish(offer);

        // Alongside: the transfer ends while the cleanup still runs.
        {
            auto prepared = std::make_shared<std::atomic<bool>>(false);
            auto cleaning = std::make_shared<std::atomic<bool>>(false);
            auto cleaned = std::make_shared<std::atomic<bool>>(false);
            auto order = std::make_shared<std::atomic<bool>>(true);
            FetchOptions options;
            options.prepareAsUser = [prepared] { prepared->store(true); };
            options.cleanupAsUser = [prepared, cleaning, cleaned, order] {
                order->store(prepared->load());
                cleaning->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                cleaned->store(true);
            };
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), id, target.filePath("along"), true, 1 << 30, [&](const FetchResult &r) {
                result = r;
                done = true;
            }, options);
            QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
            QVERIFY2(result.ok, qPrintable(result.error));
            QVERIFY(!cleaned->load()); // the transfer did not wait for it
            QTRY_VERIFY_WITH_TIMEOUT(cleaned->load(), 20000);
            QVERIFY(order->load());
        }

        // Cancelled during the preparation: the cleanup runs all the same.
        {
            auto cleaned = std::make_shared<std::atomic<bool>>(false);
            FetchOptions options;
            options.prepareAsUser = [] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); };
            options.cleanupAsUser = [cleaned] { cleaned->store(true); };
            bool done = false;
            const quint64 handle = fb.fetch(a->id(), id, target.filePath("cancelled"), true, 1 << 30,
                                            [&](const FetchResult &) { done = true; }, options);
            QTest::qWait(100);
            fb.cancel(handle);
            QVERIFY(done);
            QTRY_VERIFY_WITH_TIMEOUT(cleaned->load(), 20000);
        }

        // Not enough room until the cleanup is over: waited for, then fits.
        auto freed = std::make_shared<std::atomic<bool>>(false);
        fb.setFreeSpaceForTests([freed](const QString &) {
            return freed->load() ? qint64(10) << 30 : qint64(1024);
        });
        {
            FetchOptions options;
            options.prepareAsUser = [] {};
            options.cleanupAsUser = [freed] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                freed->store(true);
            };
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), id, target.filePath("roomy"), true, 1 << 30, [&](const FetchResult &r) {
                result = r;
                done = true;
            }, options);
            QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
            QVERIFY2(result.ok, qPrintable(result.error));
            QCOMPARE(readFile(target.filePath("roomy/doc.bin")), QByteArray(1024 * 1024, 'd'));
        }
        // Without a cleanup to wait for: refused as before.
        freed->store(false);
        {
            FetchResult result;
            bool done = false;
            fb.fetch(a->id(), id, target.filePath("full"), true, 1 << 30, [&](const FetchResult &r) {
                result = r;
                done = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
            QVERIFY(!result.ok);
            QVERIFY2(result.error.startsWith(QStringLiteral("Not enough free disk space")), qPrintable(result.error));
        }
    }
};

QTEST_GUILESS_MAIN(TestTransfer)
#include "tst_transfer.moc"
