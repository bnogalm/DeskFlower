#pragma once

// Signed update packages.
//
// A release is an installer package (the MSI on Windows) plus a small JSON manifest describing it
// (product, version, platform, file name, size, SHA-256) and an Ed25519 signature of the manifest's
// exact bytes. Devices only install a package whose manifest verifies with the public key compiled
// into them (build::updatePublicKey()), whose bytes hash to the manifest's SHA-256, whose platform is
// theirs and whose version is newer than theirs. Files next to the package:
//   <package>.manifest       the manifest (JSON)
//   <package>.manifest.sig   its signature, as hexadecimal text
// scripts/package-windows.bat produces both with the deskflower-sign tool (src/tools).

#include <QByteArray>
#include <QString>
#include <optional>

class QIODevice;

namespace df::update {

constexpr int kPublicKeySize = 32;
constexpr int kSecretKeySize = 64;
constexpr int kSignatureSize = 64;
constexpr int kMaxManifestSize = 4096;
inline const QString kProduct = QStringLiteral("Deskflower");
inline const QString kManifestSuffix = QStringLiteral(".manifest");
inline const QString kSignatureSuffix = QStringLiteral(".manifest.sig");

struct SigningKey {
    QByteArray publicKey; // 32 bytes
    QByteArray secretKey; // 64 bytes (libsodium layout: seed then public key)
};

SigningKey generateSigningKey();
// Ed25519 detached signatures.
QByteArray sign(const QByteArray &secretKey, const QByteArray &data);
bool verify(const QByteArray &publicKey, const QByteArray &data, const QByteArray &signature);

// Signature files hold hexadecimal text; decoding returns empty unless it is exactly one signature.
QByteArray encodeSignature(const QByteArray &signature);
QByteArray decodeSignature(const QByteArray &text);

// Dotted numeric versions ("0.2.0", "1.10"; missing parts count as 0). Returns <0, 0 or >0, or
// nullopt when either is not purely numeric (pre-release tags are never installed automatically).
std::optional<int> compareVersions(const QString &a, const QString &b);

// Plain file name for a package: letters, digits, '.', '_' and '-', no path, at most 128 characters.
bool isSafeFileName(const QString &name);

struct Manifest {
    QString product;
    QString version;
    QString platform; // build::platform() of the devices it is for
    QString file;     // package file name
    qint64 size = 0;
    QByteArray sha256;

    QByteArray toJson() const;
    // Structure only: the signature must have been checked on these bytes first (open()).
    static std::optional<Manifest> fromJson(const QByteArray &json, QString *error);
};

// Describes a package file on disk (size and hash computed here).
std::optional<Manifest> describe(const QString &packagePath, const QString &version, const QString &platform,
                                 QString *error);
// Checks the signature of the manifest bytes with publicKey, then parses them.
std::optional<Manifest> open(const QByteArray &publicKey, const QByteArray &json, const QByteArray &signature,
                             QString *error);
// Whether a verified manifest is an update for this device: same platform, strictly newer version.
bool isApplicable(const Manifest &manifest, const QString &currentVersion, const QString &platform, QString *error);
// Reads the device from its start and checks size and SHA-256 against the manifest.
bool matchesContent(const Manifest &manifest, QIODevice *device, QString *error);

} // namespace df::update
