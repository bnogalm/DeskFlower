#include "sessionlauncher.h"

#include "crypto.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcessEnvironment>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#endif

Q_LOGGING_CATEGORY(lcLauncher, "deskflower.launcher")

namespace df {

namespace {
const QByteArray kTokenVariable("DESKFLOWER_SESSION_TOKEN");
constexpr int kRestartDelayMs = 2000;
} // namespace

SessionLauncher::SessionLauncher(const QString &instance, const QString &logDir, bool serviceMode, QObject *parent)
    : QObject(parent),
      m_instance(instance),
      m_logDir(logDir),
      m_serviceMode(serviceMode),
      m_token(crypto::randomBytes(24).toHex())
{
    m_restart.setSingleShot(true);
    m_restart.setInterval(kRestartDelayMs);
    connect(&m_restart, &QTimer::timeout, this, &SessionLauncher::launch);

    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        qCInfo(lcLauncher) << "Session helper exited" << code << status;
        scheduleRestart();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qCWarning(lcLauncher) << "Cannot start" << program() << m_process.errorString();
            scheduleRestart();
        }
    });

    // Windows service mode: follow the active console session.
    m_sessionCheck.setInterval(2000);
    connect(&m_sessionCheck, &QTimer::timeout, this, [this] {
        const quint32 active = platform::activeConsoleSession();
        if (active != platform::kNoSession && active != m_native.sessionId) {
            qCInfo(lcLauncher) << "Active console session changed to" << active;
            stopNative();
            launch();
        }
    });
}

SessionLauncher::~SessionLauncher()
{
    m_stopping = true;
    stopNative();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(3000);
    }
}

QString SessionLauncher::program() const
{
    const QDir dir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    return dir.filePath(QStringLiteral("deskflower-session.exe"));
#else
    // macOS builds the helper as an app bundle (privacy permissions are granted per bundle); it
    // sits next to the service in the build tree, or one level up from bin/ when installed.
    const QStringList candidates = {
        dir.filePath(QStringLiteral("deskflower-session")),
        dir.filePath(QStringLiteral("deskflower-session.app/Contents/MacOS/deskflower-session")),
        dir.filePath(QStringLiteral("../deskflower-session.app/Contents/MacOS/deskflower-session")),
    };
    for (const QString &c : candidates) {
        if (QFileInfo(c).isExecutable())
            return QDir::cleanPath(c);
    }
    return candidates.first();
#endif
}

QStringList SessionLauncher::arguments() const
{
    QStringList args;
    if (!m_logDir.isEmpty()) // no log file otherwise
        args << QStringLiteral("--log-dir") << m_logDir;
    if (!m_instance.isEmpty())
        args << QStringLiteral("--instance") << m_instance;
    return args;
}

void SessionLauncher::start()
{
    launch();
    if (m_serviceMode)
        m_sessionCheck.start();
}

void SessionLauncher::scheduleRestart()
{
    if (!m_stopping && !m_restart.isActive())
        m_restart.start();
}

void SessionLauncher::launch()
{
    if (m_stopping)
        return;
#ifdef Q_OS_WIN
    if (m_serviceMode) {
        if (m_native.handle)
            return;
        QString error;
        if (!platform::launchInActiveSession(program(), arguments(), kTokenVariable, m_token, &m_native, &error)) {
            qCWarning(lcLauncher) << "Cannot start the session helper:" << error;
            scheduleRestart();
            return;
        }
        qCInfo(lcLauncher) << "Session helper started in session" << m_native.sessionId;
        m_notifier = new QWinEventNotifier(m_native.handle, this);
        connect(m_notifier, &QWinEventNotifier::activated, this, [this] {
            qCInfo(lcLauncher) << "Session helper exited";
            stopNative();
            scheduleRestart();
        });
        return;
    }
#endif
    if (m_process.state() != QProcess::NotRunning)
        return;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kTokenVariable), QString::fromLatin1(m_token));
    m_process.setProcessEnvironment(env);
    m_process.start(program(), arguments());
}

void SessionLauncher::stopNative()
{
#ifdef Q_OS_WIN
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
#endif
    platform::terminateSessionProcess(&m_native);
}

} // namespace df
