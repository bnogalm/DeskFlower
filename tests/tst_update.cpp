#include "testutil.h"
#include "update.h"
#include "updater.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace df;

namespace {
const QString kPlatform = QStringLiteral("testos-x86_64");

QByteArray randomBytes(int n, quint32 seed)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator rng(seed);
    for (int i = 0; i < n; ++i)
        b[i] = char(rng.bounded(256));
    return b;
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(content) == content.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A release as scripts/package-windows.bat produces it: package, manifest and signature.
QString makeRelease(const QString &dir, const update::SigningKey &key, const QString &version,
                    const QString &platform = kPlatform, int size = 300 * 1024)
{
    const QString path = QDir(dir).filePath(QStringLiteral("Deskflower-%1.msi").arg(version));
    writeFile(path, randomBytes(size, 42));
    QString error;
    const auto manifest = update::describe(path, version, platform, &error);
    if (!manifest)
        qWarning() << error;
    const QByteArray json = manifest ? manifest->toJson() : QByteArray();
    writeFile(path + update::kManifestSuffix, json);
    writeFile(path + update::kSignatureSuffix, update::encodeSignature(update::sign(key.secretKey, json)));
    return path;
}

Updater::Options options(const QString &storeDir, const update::SigningKey &key, const QString &version)
{
    Updater::Options o;
    o.storeDir = storeDir;
    o.publicKey = key.publicKey;
    o.version = version;
    o.platform = kPlatform;
    return o;
}

class MockInstaller : public PackageInstaller
{
public:
    bool isSupported() const override { return true; }
    bool launch(const QString &packagePath, std::function<void(bool, const QString &)> exit, QString *) override
    {
        onExit = std::move(exit);
        launched.append(packagePath);
        content = readFile(packagePath);
        QFile writer(packagePath);
        writableWhileLaunching = writer.open(QIODevice::ReadWrite);
        return true;
    }
    QStringList launched;
    QByteArray content;
    bool writableWhileLaunching = true;
    std::function<void(bool, const QString &)> onExit;
};

struct Result {
    bool done = false;
    bool ok = false;
    QString error;
};

void track(Updater *updater, Result *result)
{
    QObject::connect(updater, &Updater::finished, updater,
                     [result](const QByteArray &, bool ok, const QString &error, const QString &) {
                         result->done = true;
                         result->ok = ok;
                         result->error = error;
                     });
}
} // namespace

class TestUpdate : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }

    void signVerifyRoundTrip()
    {
        const update::SigningKey key = update::generateSigningKey();
        QCOMPARE(key.publicKey.size(), update::kPublicKeySize);
        QCOMPARE(key.secretKey.size(), update::kSecretKeySize);
        const QByteArray data("manifest bytes");
        const QByteArray sig = update::sign(key.secretKey, data);
        QVERIFY(update::verify(key.publicKey, data, sig));
        QCOMPARE(update::decodeSignature(update::encodeSignature(sig)), sig);
        QVERIFY(!update::verify(key.publicKey, data + "x", sig));
        QVERIFY(update::decodeSignature("not hex").isEmpty());
        QVERIFY(update::decodeSignature(sig.left(10).toHex()).isEmpty());

        QTemporaryDir dir;
        const QString package = makeRelease(dir.path(), key, "0.2.0");
        QString error;
        const auto m = update::open(key.publicKey, readFile(package + update::kManifestSuffix),
                                    update::decodeSignature(readFile(package + update::kSignatureSuffix)), &error);
        QVERIFY2(m, qPrintable(error));
        QCOMPARE(m->version, QString("0.2.0"));
        QCOMPARE(m->platform, kPlatform);
        QCOMPARE(m->file, QString("Deskflower-0.2.0.msi"));
        QCOMPARE(m->size, qint64(300 * 1024));
        QFile f(package);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY2(update::matchesContent(*m, &f, &error), qPrintable(error));
    }

    void tamperedPackageRejected()
    {
        const update::SigningKey key = update::generateSigningKey();
        QTemporaryDir dir, store;
        const QString package = makeRelease(dir.path(), key, "0.2.0");
        QByteArray bytes = readFile(package);
        bytes[1000] = char(bytes[1000] ^ 1);
        QVERIFY(writeFile(package, bytes));

        QString error;
        const auto m = update::open(key.publicKey, readFile(package + update::kManifestSuffix),
                                    update::decodeSignature(readFile(package + update::kSignatureSuffix)), &error);
        QVERIFY(m);
        QFile f(package);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!update::matchesContent(*m, &f, &error));
        QVERIFY(error.contains("SHA-256"));
        QVERIFY(!Updater::importPackage(options(store.path(), key, "0.2.0"), package, &error));

        QVERIFY(writeFile(package, bytes + "trailing"));
        f.close();
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!update::matchesContent(*m, &f, &error));
    }

    void tamperedManifestRejected()
    {
        const update::SigningKey key = update::generateSigningKey();
        QTemporaryDir dir;
        const QString package = makeRelease(dir.path(), key, "0.2.0");
        const QByteArray json = readFile(package + update::kManifestSuffix);
        const QByteArray sig = update::decodeSignature(readFile(package + update::kSignatureSuffix));
        QString error;
        QVERIFY(update::open(key.publicKey, json, sig, &error));

        QByteArray forged = json;
        forged.replace("0.2.0", "9.9.9");
        QVERIFY(!update::open(key.publicKey, forged, sig, &error));
        QVERIFY(error.contains("signature"));
        QVERIFY(!update::open(key.publicKey, json + " ", sig, &error));
        QByteArray badSig = sig;
        badSig[0] = char(badSig[0] ^ 1);
        QVERIFY(!update::open(key.publicKey, json, badSig, &error));

        // Signed but malformed: still refused.
        QVERIFY(!update::open(key.publicKey, "{}", update::sign(key.secretKey, "{}"), &error));
        QByteArray traversal = json;
        traversal.replace("Deskflower-0.2.0.msi", "../evil.msi");
        QVERIFY(!update::open(key.publicKey, traversal, update::sign(key.secretKey, traversal), &error));
    }

    void wrongKeyRejected()
    {
        const update::SigningKey key = update::generateSigningKey();
        const update::SigningKey other = update::generateSigningKey();
        QTemporaryDir dir, store;
        const QString package = makeRelease(dir.path(), key, "0.2.0");
        QString error;
        QVERIFY(!update::open(other.publicKey, readFile(package + update::kManifestSuffix),
                              update::decodeSignature(readFile(package + update::kSignatureSuffix)), &error));
        QVERIFY(!Updater::importPackage(options(store.path(), other, "0.2.0"), package, &error));
        // No key compiled in: nothing is accepted.
        QVERIFY(!update::open({}, readFile(package + update::kManifestSuffix),
                              update::decodeSignature(readFile(package + update::kSignatureSuffix)), &error));
        QVERIFY(error.contains("not configured"));
    }

    void versionsAndPlatforms()
    {
        QCOMPARE(update::compareVersions("0.2.0", "0.1.9"), std::optional<int>(1));
        QCOMPARE(update::compareVersions("0.10.0", "0.9.0"), std::optional<int>(1));
        QCOMPARE(update::compareVersions("1.0", "1.0.0"), std::optional<int>(0));
        QCOMPARE(update::compareVersions("1.0.0", "1.0.1"), std::optional<int>(-1));
        QVERIFY(!update::compareVersions("1.0.0-beta", "1.0.0"));
        QVERIFY(!update::compareVersions("", "1.0.0"));
        QVERIFY(!update::compareVersions("1..0", "1.0.0"));

        update::Manifest m;
        m.version = "0.2.0";
        m.platform = kPlatform;
        QString error;
        QVERIFY(update::isApplicable(m, "0.1.0", kPlatform, &error));
        QVERIFY(!update::isApplicable(m, "0.2.0", kPlatform, &error)); // same version
        QVERIFY(!update::isApplicable(m, "0.3.0", kPlatform, &error)); // downgrade
        QVERIFY(error.contains("not newer"));
        QVERIFY(!update::isApplicable(m, "0.1.0", "otheros-arm64", &error)); // wrong platform
        QVERIFY(error.contains("otheros-arm64"));
    }

    void importKeepsOnlyThisInstallation()
    {
        const update::SigningKey key = update::generateSigningKey();
        QTemporaryDir dir, store;
        const QString package = makeRelease(dir.path(), key, "0.2.0");
        const QString foreign = makeRelease(dir.path(), key, "0.3.0", "otheros-arm64");
        QString error;
        QVERIFY(!Updater::importPackage(options(store.path(), key, "0.1.0"), package, &error)); // other version
        QVERIFY(!Updater::importPackage(options(store.path(), key, "0.3.0"), foreign, &error)); // other platform
        QVERIFY(!Updater::importPackage(options(store.path(), {}, "0.2.0"), package, &error));  // not configured
        QVERIFY2(Updater::importPackage(options(store.path(), key, "0.2.0"), package, &error), qPrintable(error));
        const QString kept = QDir(store.path()).filePath("package/Deskflower-0.2.0.msi");
        QCOMPARE(readFile(kept), readFile(package));
        QVERIFY(QFile::exists(kept + update::kManifestSuffix));
        QVERIFY(QFile::exists(kept + update::kSignatureSuffix));
        QVERIFY2(Updater::importPackage(options(store.path(), key, "0.2.0"), kept, &error), qPrintable(error));
    }

    void transferBetweenPairedNodes()
    {
        const update::SigningKey key = update::generateSigningKey();
        QTemporaryDir dirA, dirB, release, storeA, storeB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));

        const QString package = makeRelease(release.path(), key, "0.2.0", kPlatform, 2 * 1024 * 1024 + 77);
        QString error;
        QVERIFY2(Updater::importPackage(options(storeA.path(), key, "0.2.0"), package, &error), qPrintable(error));
        MockInstaller installerA, installerB;
        Updater server(a.get(), &installerA, options(storeA.path(), key, "0.2.0"));
        Updater client(b.get(), &installerB, options(storeB.path(), key, "0.1.0"));
        QVERIFY(server.servedPackage());

        // The offer travels in the hello.
        auto offered = [&] {
            for (const PeerView &p : b->peers()) {
                if (p.id == a->id())
                    return client.offeredUpdate(p);
            }
            return QString();
        };
        QTRY_COMPARE(offered(), QString("0.2.0"));

        Result result;
        track(&client, &result);
        QVERIFY2(client.update(a->id(), &error), qPrintable(error));
        QCOMPARE(client.state(), Updater::State::Downloading);
        QVERIFY(!client.update(a->id(), &error)); // one at a time
        QTRY_VERIFY_WITH_TIMEOUT(result.done, 30000);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(installerB.launched.size(), 1);
        QCOMPARE(installerB.content, readFile(package));
        QCOMPARE(QFileInfo(installerB.launched.first()).fileName(), QString("Deskflower-0.2.0.msi"));
        QVERIFY(installerB.launched.first().startsWith(storeB.path()));
        // Kept next to it, so that the installer can keep it for this device's peers.
        QVERIFY(QFile::exists(installerB.launched.first() + update::kSignatureSuffix));
#ifdef Q_OS_WIN
        QVERIFY(!installerB.writableWhileLaunching); // locked between the last check and the installer
#endif
        QVERIFY(installerA.launched.isEmpty());

        // Installing until the installer ends: no second update meanwhile, the package stays put.
        QCOMPARE(client.state(), Updater::State::Installing);
        QVERIFY(!client.update(a->id(), &error));
        QVERIFY(QFile::exists(installerB.launched.first()));
        installerB.onExit(true, QString()); // done, the restart is pending
        QCOMPARE(client.state(), Updater::State::Installing);

        // A failed installation releases and deletes the package, and allows another try.
        Result again;
        track(&client, &again);
        installerB.onExit(false, QStringLiteral("error 1603"));
        QVERIFY(again.done);
        QVERIFY(!again.ok);
        QCOMPARE(again.error, QString("error 1603"));
        QCOMPARE(client.state(), Updater::State::Idle);
        QVERIFY(!QFile::exists(installerB.launched.first()));
    }

    void unpairedPeerRefused()
    {
        const update::SigningKey key = update::generateSigningKey();
        QTemporaryDir dirA, dirB, dirC, release, storeA, storeB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        auto c = test::makeNode(dirC.path());
        QVERIFY(test::pairNodes(*a, *b));
        const QString package = makeRelease(release.path(), key, "0.2.0");
        QString error;
        QVERIFY(Updater::importPackage(options(storeA.path(), key, "0.2.0"), package, &error));
        MockInstaller installerA, installerB;
        Updater server(a.get(), &installerA, options(storeA.path(), key, "0.2.0"));
        Updater client(b.get(), &installerB, options(storeB.path(), key, "0.1.0"));

        QVERIFY(!client.update(c->id(), &error)); // never paired
        QVERIFY(error.contains("not paired"));

        b->unpair(a->id());
        QTRY_VERIFY(!a->isTrusted(b->id()));
        QVERIFY(!client.update(a->id(), &error));
        QVERIFY(error.contains("not paired"));
        QCOMPARE(client.state(), Updater::State::Idle);
        QVERIFY(installerB.launched.isEmpty());
    }

    void sizeLimitAndTamperingOnTheWire()
    {
        const update::SigningKey key = update::generateSigningKey();
        QTemporaryDir dirA, dirB, release, storeA, storeB;
        auto a = test::makeNode(dirA.path());
        auto b = test::makeNode(dirB.path());
        QVERIFY(test::pairNodes(*a, *b));
        const QString package = makeRelease(release.path(), key, "0.2.0");
        QString error;
        QVERIFY(Updater::importPackage(options(storeA.path(), key, "0.2.0"), package, &error));
        MockInstaller installerA, installerB;
        Updater::Options serverOptions = options(storeA.path(), key, "0.2.0");
        serverOptions.minRequestIntervalMs = 0;
        Updater server(a.get(), &installerA, serverOptions);
        Updater::Options clientOptions = options(storeB.path(), key, "0.1.0");
        clientOptions.maxPackageBytes = 100 * 1024;
        auto client = std::make_unique<Updater>(b.get(), &installerB, clientOptions);
        Result result;
        track(client.get(), &result);

        // Too large: refused from the manifest, before any data.
        QVERIFY2(client->update(a->id(), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(result.done, 15000);
        QVERIFY(!result.ok);
        QVERIFY2(result.error.contains("larger than allowed"), qPrintable(result.error));
        QVERIFY(installerB.launched.isEmpty());
        QVERIFY(QDir(QDir(storeB.path()).filePath("incoming")).isEmpty() || !QDir(storeB.path()).exists("incoming"));

        // The served file changes after it was checked: the receiver's hash check catches it.
        const QString stored = QDir(storeA.path()).filePath("package/Deskflower-0.2.0.msi");
        QByteArray bytes = readFile(stored);
        bytes[5] = char(bytes[5] ^ 0x80);
        QVERIFY(writeFile(stored, bytes));
        clientOptions.maxPackageBytes = 1024 * 1024;
        client = std::make_unique<Updater>(b.get(), &installerB, clientOptions);
        result = {};
        track(client.get(), &result);
        QVERIFY2(client->update(a->id(), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(result.done, 15000);
        QVERIFY(!result.ok);
        QVERIFY2(result.error.contains("SHA-256"), qPrintable(result.error));
        QVERIFY(installerB.launched.isEmpty());

        // Without a compiled-in key nothing is even requested.
        clientOptions.publicKey.clear();
        Updater unconfigured(b.get(), &installerB, clientOptions);
        QVERIFY(!unconfigured.update(a->id(), &error));
        QVERIFY(error.contains("not configured"));
    }
};

QTEST_GUILESS_MAIN(TestUpdate)
#include "tst_update.moc"
