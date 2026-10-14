#pragma once

// Updates from paired devices (see update.h for the signed package format).
//
// Serving: a device keeps the signed package of the version it runs in <store>/package (put there
// by "deskflower-service --import-update", which the MSI runs with the package it was installed
// from). It announces the package's version in its hello and sends it, unprompted, to paired peers
// that ask, one at a time and at most once per peer every few seconds.
//
// Receiving: only when the local user asks (update()). Over a SecureStream, every frame starting
// with a kind byte (1 = CBOR control, 2 = data):
//   client: update {}
//   server: package {m: manifest bytes, s: signature}, data frames, end {}   or   error {r}
// The client checks the signature, platform, version and size before accepting any data, streams
// the package into <store>/incoming (SYSTEM and Administrators only on Windows), then reopens it
// read-only so nobody else can change it, hashes those exact bytes and hands the still-locked file
// to the installer. The lock is held, and no other update starts, until this process exits (the
// installer stops the service) or the installer fails. Links (junctions, symbolic links) inside the
// store are never followed.

#include "update.h"

#include <QCborMap>
#include <QFile>
#include <QObject>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace df {

class Node;
class SecureStream;
struct PeerView;

// Starts the platform installer. A test double replaces it in the tests.
class PackageInstaller
{
public:
    virtual ~PackageInstaller() = default;
    // Whether this platform can install packages at all.
    virtual bool isSupported() const = 0;
    // Starts installing the verified package and returns at once. The installer may stop this process;
    // if it ends first, onExit says whether it succeeded (with the reason if not).
    virtual bool launch(const QString &packagePath, std::function<void(bool ok, const QString &error)> onExit,
                        QString *error) = 0;
};

class Updater : public QObject
{
    Q_OBJECT
public:
    static constexpr int kChunk = 60 * 1024;
    static constexpr qint64 kHighWater = 1024 * 1024;
    static constexpr int kIdleTimeoutMs = 30000;

    struct Options {
        QString storeDir;           // <config dir>/updates
        QByteArray publicKey;       // build::updatePublicKey(); empty: updates disabled
        QString version;            // build::version()
        QString platform;           // build::platform()
        qint64 maxPackageBytes = qint64(512) * 1024 * 1024;
        int minRequestIntervalMs = 10000; // serving side, per peer
        static Options defaults(const QString &storeDir);
    };

    enum class State { Idle, Downloading, Verifying, Installing };

    Updater(Node *node, PackageInstaller *installer, Options options, QObject *parent = nullptr);
    ~Updater() override;

    // Copies a package (with <package>.manifest and .manifest.sig next to it) into the store after
    // checking its signature, platform and content. Its version must be options.version: the store
    // holds the package of what is installed. Used by "deskflower-service --import-update".
    static bool importPackage(const Options &options, const QString &packagePath, QString *error);

    bool isConfigured() const { return m_options.publicKey.size() == update::kPublicKeySize; }
    bool isSupported() const { return m_installer && m_installer->isSupported(); }
    // The package this device serves, if any.
    const std::optional<update::Manifest> &servedPackage() const { return m_served; }
    // Version a paired peer offers that would update this device (empty: nothing to offer).
    QString offeredUpdate(const PeerView &peer) const;

    // Downloads, verifies and installs the peer's package. False (with the reason) if it cannot start;
    // otherwise finished() follows.
    bool update(const QByteArray &peerId, QString *error);
    State state() const { return m_state; }
    QByteArray peer() const { return m_peer; }
    QString targetVersion() const { return m_manifest ? m_manifest->version : QString(); }
    qint64 received() const { return m_received; }
    qint64 total() const { return m_manifest ? m_manifest->size : 0; }

signals:
    void stateChanged(); // also on download progress
    // The package is verified and the installer starts right after this.
    void aboutToInstall();
    // ok: the installer was started with the verified package at `path`. If the installer then fails,
    // finished(peer, false, ...) follows; until then the state stays Installing.
    void finished(const QByteArray &peerId, bool ok, const QString &error, const QString &path);

private:
    struct Upload;
    void loadServedPackage();
    void onStreamAccepted(SecureStream *stream);
    void serve(SecureStream *stream);
    void pump(quint64 handle);
    void onFrame(const QByteArray &frame);
    void onPackage(const QCborMap &msg);
    void onComplete();
    void onInstallerExit(bool ok, const QString &error);
    void fail(const QString &error);
    void setState(State state);
    void resetDownload();

    Node *m_node;
    PackageInstaller *m_installer;
    Options m_options;

    std::optional<update::Manifest> m_served;
    QByteArray m_servedJson;
    QByteArray m_servedSignature;
    QString m_servedPath;
    std::map<quint64, Upload *> m_uploads;
    std::map<QByteArray, qint64> m_lastRequest;
    quint64 m_nextUpload = 1;

    State m_state = State::Idle;
    QByteArray m_peer;
    SecureStream *m_stream = nullptr;
    std::optional<update::Manifest> m_manifest;
    QString m_path;
    QFile m_file;
    qint64 m_received = 0;
    QTimer m_idle;
    std::unique_ptr<QFile> m_locked; // the verified package, kept read-only until we exit
};

} // namespace df
