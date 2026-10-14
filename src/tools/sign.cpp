// deskflower-sign: release signing for updates served by paired devices (README, "Signed updates").
//
//   deskflower-sign keygen [dir]                       new key pair (default dir: ~/.deskflower-signing)
//   deskflower-sign manifest <package> <version> [platform]   writes <package>.manifest
//   deskflower-sign sign <secret-key-file> <file>      writes <file>.sig
//   deskflower-sign verify <public-key-hex|--compiled> <file> <signature-file>
//
// The secret key never belongs in the repository. "--compiled" checks against the key built into
// this binary (DESKFLOWER_UPDATE_PUBKEY), i.e. the one the matching release will accept.

#include "buildinfo.h"
#include "crypto.h"
#include "update.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <vector>
#endif

namespace update = df::update;

namespace {

const QString kSecretName = QStringLiteral("update-signing.key");
const QString kPublicName = QStringLiteral("update-signing.pub");

int usage()
{
    std::fprintf(stderr,
                 "Usage:\n"
                 "  deskflower-sign keygen [dir]                  (default: ~/.deskflower-signing)\n"
                 "  deskflower-sign manifest <package> <version> [platform]\n"
                 "  deskflower-sign sign <secret-key-file> <file>\n"
                 "  deskflower-sign verify <public-key-hex|--compiled> <file> <signature-file>\n");
    return 2;
}

int failure(const QString &message)
{
    std::fprintf(stderr, "deskflower-sign: %s\n", qPrintable(message));
    return 1;
}

QByteArray readFile(const QString &path, bool *ok)
{
    QFile f(path);
    *ok = f.open(QIODevice::ReadOnly);
    return *ok ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

// Only the current user may read or change the path (directories pass this on to new files).
bool restrictToCurrentUser(const QString &path)
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size);
    const bool haveUser = size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!haveUser)
        return false;
    EXPLICIT_ACCESS_W access = {};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
        return false;
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD rc = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, acl, nullptr);
    LocalFree(acl);
    return rc == ERROR_SUCCESS;
#else
    const bool dir = QFileInfo(path).isDir();
    return QFile::setPermissions(path, dir ? (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)
                                           : (QFile::ReadOwner | QFile::WriteOwner));
#endif
}

int keygen(const QString &dirArg)
{
    const QString dir = dirArg.isEmpty() ? QDir::home().filePath(QStringLiteral(".deskflower-signing")) : dirArg;
    const QString secretPath = QDir(dir).filePath(kSecretName);
    const QString publicPath = QDir(dir).filePath(kPublicName);
    if (QFileInfo::exists(secretPath))
        return failure(QStringLiteral("%1 already exists; not overwriting a signing key")
                           .arg(QDir::toNativeSeparators(secretPath)));
    if (!QDir().mkpath(dir) || !restrictToCurrentUser(dir))
        return failure(QStringLiteral("Cannot create a private directory %1").arg(QDir::toNativeSeparators(dir)));

    update::SigningKey key = update::generateSigningKey();
    QByteArray secretHex = key.secretKey.toHex() + '\n';
    const bool written = writeFile(secretPath, secretHex) && restrictToCurrentUser(secretPath);
    df::crypto::wipe(secretHex);
    df::crypto::wipe(key.secretKey);
    if (!written)
        return failure(QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(secretPath)));
    const QByteArray publicHex = key.publicKey.toHex();
    if (!writeFile(publicPath, publicHex + '\n'))
        return failure(QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(publicPath)));

    std::printf("Secret key: %s (keep it private and backed up; never commit it)\n"
                "Public key: %s\n\n"
                "Build releases with: -DDESKFLOWER_UPDATE_PUBKEY=%s\n",
                qPrintable(QDir::toNativeSeparators(secretPath)), qPrintable(QDir::toNativeSeparators(publicPath)),
                publicHex.constData());
    return 0;
}

int manifest(const QString &package, const QString &version, const QString &platform)
{
    QString error;
    const auto m = update::describe(package, version, platform.isEmpty() ? df::build::platform() : platform, &error);
    if (!m)
        return failure(error);
    const QString out = package + update::kManifestSuffix;
    if (!writeFile(out, m->toJson()))
        return failure(QStringLiteral("Cannot write %1").arg(out));
    std::printf("%s: %s %s %s, %lld bytes\n", qPrintable(QDir::toNativeSeparators(out)), qPrintable(m->file),
                qPrintable(m->version), qPrintable(m->platform), static_cast<long long>(m->size));
    return 0;
}

int sign(const QString &secretKeyFile, const QString &file)
{
    bool ok = false;
    QByteArray secretHex = readFile(secretKeyFile, &ok).trimmed();
    QByteArray secret = QByteArray::fromHex(secretHex);
    df::crypto::wipe(secretHex);
    if (!ok || secret.size() != update::kSecretKeySize)
        return failure(QStringLiteral("%1 is not a signing key (deskflower-sign keygen)").arg(secretKeyFile));
    const QByteArray data = readFile(file, &ok);
    if (!ok)
        return failure(QStringLiteral("Cannot read %1").arg(file));
    const QByteArray signature = update::sign(secret, data);
    const QByteArray publicKey = secret.right(update::kPublicKeySize); // libsodium appends it
    df::crypto::wipe(secret);
    if (!update::verify(publicKey, data, signature))
        return failure(QStringLiteral("Signing failed"));
    if (!writeFile(file + QStringLiteral(".sig"), update::encodeSignature(signature)))
        return failure(QStringLiteral("Cannot write %1.sig").arg(file));
    std::printf("Signed %s with key %s\n", qPrintable(QDir::toNativeSeparators(file)), publicKey.toHex().constData());
    return 0;
}

int verify(const QString &publicKeyArg, const QString &file, const QString &signatureFile)
{
    const QByteArray publicKey = publicKeyArg == QLatin1String("--compiled")
                                     ? df::build::updatePublicKey()
                                     : QByteArray::fromHex(publicKeyArg.toLatin1());
    if (publicKey.size() != update::kPublicKeySize)
        return failure(publicKeyArg == QLatin1String("--compiled")
                           ? QStringLiteral("This build has no DESKFLOWER_UPDATE_PUBKEY")
                           : QStringLiteral("The public key must be 64 hexadecimal characters"));
    bool ok = false, sigOk = false;
    const QByteArray data = readFile(file, &ok);
    const QByteArray signature = update::decodeSignature(readFile(signatureFile, &sigOk));
    if (!ok || !sigOk)
        return failure(QStringLiteral("Cannot read %1 or %2").arg(file, signatureFile));
    if (!update::verify(publicKey, data, signature))
        return failure(QStringLiteral("BAD signature for %1 (key %2)").arg(file, QString::fromLatin1(publicKey.toHex())));
    std::printf("Signature OK: %s\n", qPrintable(QDir::toNativeSeparators(file)));

    // A manifest: the package next to it must match too.
    if (!file.endsWith(update::kManifestSuffix))
        return 0;
    QString error;
    const auto m = update::Manifest::fromJson(data, &error);
    const QString package = file.chopped(update::kManifestSuffix.size());
    QFile content(package);
    if (!m || !content.open(QIODevice::ReadOnly) || !update::matchesContent(*m, &content, &error))
        return failure(error.isEmpty() ? QStringLiteral("Cannot read %1").arg(package) : error);
    std::printf("Package OK: %s %s %s\n", qPrintable(m->file), qPrintable(m->version), qPrintable(m->platform));
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (!df::crypto::init())
        return failure(QStringLiteral("libsodium init failed"));
    const QStringList args = app.arguments().mid(1);
    const QString cmd = args.value(0);
    if (cmd == QLatin1String("keygen") && args.size() <= 2)
        return keygen(args.value(1));
    if (cmd == QLatin1String("manifest") && (args.size() == 3 || args.size() == 4))
        return manifest(args.at(1), args.at(2), args.value(3));
    if (cmd == QLatin1String("sign") && args.size() == 3)
        return sign(args.at(1), args.at(2));
    if (cmd == QLatin1String("verify") && args.size() == 4)
        return verify(args.at(1), args.at(2), args.at(3));
    return usage();
}
