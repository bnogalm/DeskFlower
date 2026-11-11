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

#ifdef Q_OS_MACOS
#include <QFile>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#endif

Q_LOGGING_CATEGORY(lcLauncher, "deskflower.launcher")

namespace df {

namespace {
const QByteArray kTokenVariable("DESKFLOWER_SESSION_TOKEN");
constexpr int kRestartDelayMs = 2000;
// A helper that ran less than this before exiting doubles the next delay, up to the maximum.
constexpr qint64 kStableRunMs = 15000;
constexpr int kMaxRestartDelayMs = 60000;

#ifdef Q_OS_MACOS
// Starts the helper responsible for itself: its own bundle, not the service, is what the privacy
// permissions are asked for and granted to. The responsibility call is private (looked up at run
// time; without it the helper still runs, attributed to the service). Only stdin, stdout and
// stderr are inherited. The pid, or 0 with *error.
pid_t spawnResponsible(const QString &program, const QStringList &args, const QProcessEnvironment &env,
                       QString *error)
{
    QList<QByteArray> argBytes{QFile::encodeName(program)};
    for (const QString &a : args)
        argBytes.append(a.toLocal8Bit());
    QList<QByteArray> envBytes;
    for (const QString &entry : env.toStringList())
        envBytes.append(entry.toLocal8Bit());
    std::vector<char *> argv;
    for (QByteArray &a : argBytes)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<char *> envp;
    for (QByteArray &e : envBytes)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, short(POSIX_SPAWN_CLOEXEC_DEFAULT));
    using Disclaim = int (*)(posix_spawnattr_t *, int);
    if (const auto disclaim = reinterpret_cast<Disclaim>(dlsym(RTLD_DEFAULT, "responsibility_spawnattrs_setdisclaim")))
        disclaim(&attr, 1);
    else
        qCWarning(lcLauncher) << "The helper cannot be made responsible for itself on this macOS";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd : {0, 1, 2})
        posix_spawn_file_actions_addinherit_np(&actions, fd);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        *error = QString::fromLocal8Bit(std::strerror(rc));
        return 0;
    }
    return pid;
}
#endif
} // namespace

SessionLauncher::SessionLauncher(const QString &instance, const QString &logDir, const QString &crashDir,
                                 bool serviceMode, QObject *parent)
    : QObject(parent),
      m_instance(instance),
      m_logDir(logDir),
      m_crashDir(crashDir),
      m_serviceMode(serviceMode),
      m_token(crypto::randomBytes(24).toHex()),
      m_restartDelayMs(kRestartDelayMs)
{
    m_restart.setSingleShot(true);
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

#ifdef Q_OS_MACOS
    m_reap.setInterval(500);
    connect(&m_reap, &QTimer::timeout, this, [this] {
        int status = 0;
        if (m_pid <= 0 || ::waitpid(pid_t(m_pid), &status, WNOHANG) != pid_t(m_pid))
            return;
        qCInfo(lcLauncher) << "Session helper exited" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        m_pid = 0;
        m_reap.stop();
        scheduleRestart();
    });
#endif

    // Windows service mode: follow the active console session.
    m_exitGrace.setSingleShot(true);
    m_exitGrace.setInterval(kExitGraceMs);
    connect(&m_exitGrace, &QTimer::timeout, this, [this] {
        if (m_native.handle) {
            qCWarning(lcLauncher) << "The session helper did not leave in time: terminating it";
            stopNative();
        }
        launch();
    });
    m_sessionCheck.setInterval(2000);
    connect(&m_sessionCheck, &QTimer::timeout, this, [this] {
        const quint32 active = platform::activeConsoleSession();
        if (active == platform::kNoSession || active == m_native.sessionId || m_exitGrace.isActive())
            return;
        qCInfo(lcLauncher) << "Active console session changed to" << active;
        if (!m_native.handle) {
            launch();
            return;
        }
        // The old helper only ends when its pipe to the service closes (it then puts the system
        // cursors back and releases injected keys in the session it leaves), so the service is
        // told to close it; a helper still there after the grace is terminated. Waiting here
        // would stall the whole service (network, IPC, phones) for the grace time.
        emit helperStopping();
        m_exitGrace.start();
    });
}

SessionLauncher::~SessionLauncher()
{
    // The service is stopping and its pipe is closed by now: the helper ends by itself once it has
    // released the capture and put the system cursors back (blank while the pointer is on another
    // device). Killed right away, it would leave the cursor invisible.
    m_stopping = true;
    stopNative(kExitGraceMs);
    if (m_process.state() != QProcess::NotRunning && !m_process.waitForFinished(kExitGraceMs)) {
        m_process.kill();
        m_process.waitForFinished(3000);
    }
#ifdef Q_OS_MACOS
    if (m_pid > 0) {
        const pid_t pid = pid_t(m_pid);
        int status = 0;
        for (int waited = 0; waited < kExitGraceMs; waited += 50) {
            if (::waitpid(pid, &status, WNOHANG) == pid)
                return;
            ::usleep(50 * 1000);
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
#endif
}

QString SessionLauncher::program() const
{
    if (!m_program.isEmpty())
        return m_program;
    const QDir dir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    return dir.filePath(QStringLiteral("deskflower-session.exe"));
#else
    // macOS builds the helper as an app bundle (privacy permissions are granted per bundle): in
    // DeskFlower.app/Contents/Helpers when installed (the service is in Contents/MacOS), or in the
    // helper's own build folder next to the service's.
    const QStringList candidates = {
        dir.filePath(QStringLiteral("deskflower-session")),
        dir.filePath(QStringLiteral("../Helpers/deskflower-session.app/Contents/MacOS/deskflower-session")),
        dir.filePath(QStringLiteral("deskflower-session.app/Contents/MacOS/deskflower-session")),
        dir.filePath(QStringLiteral("../session/deskflower-session.app/Contents/MacOS/deskflower-session")),
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
    if (!m_crashDir.isEmpty())
        args << QStringLiteral("--crash-dir") << m_crashDir;
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
    if (m_stopping || m_restart.isActive())
        return;
    // Back off while it keeps failing at once; a helper that ran a while starts over quickly.
    if (m_running.isValid() && m_running.elapsed() >= kStableRunMs)
        m_restartDelayMs = kRestartDelayMs;
    const int delay = m_restartDelayMs;
    m_restartDelayMs = qMin(m_restartDelayMs * 2, kMaxRestartDelayMs);
    m_running.invalidate();
    qCInfo(lcLauncher) << "Restarting the session helper in" << delay << "ms";
    m_restart.start(delay);
}

QProcessEnvironment SessionLauncher::helperEnvironment(const QHash<QString, QString> &sessionEnv)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = sessionEnv.constBegin(); it != sessionEnv.constEnd(); ++it)
        env.insert(it.key(), it.value());
    return env;
}

void SessionLauncher::setSessionEnvironment(const QHash<QString, QString> &env)
{
    if (env == m_sessionEnv)
        return;
    m_sessionEnv = env;
    m_restartDelayMs = kRestartDelayMs; // something changed: worth trying again soon
    if (m_process.state() == QProcess::NotRunning) {
        if (m_restart.isActive()) {
            m_restart.stop();
            launch();
        }
        return;
    }
    // What the helper really got (the service's own variables with the agent's laid over them) is
    // what counts: a helper started before any agent connected, with the same display, stays.
    if (helperEnvironment(env) == m_helperEnv)
        return;
    // Started without (or with another) display: it cannot work there. A clean exit (the service
    // still reads its pipe), then a start with the agent's session.
    qCInfo(lcLauncher) << "The graphical session changed: restarting the session helper";
    m_running.invalidate();
    m_process.terminate();
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
        m_running.start();
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
    m_helperEnv = helperEnvironment(m_sessionEnv);
    QProcessEnvironment env = m_helperEnv;
    env.insert(QString::fromLatin1(kTokenVariable), QString::fromLatin1(m_token));
#ifdef Q_OS_MACOS
    if (m_pid > 0)
        return;
    QString error;
    const pid_t pid = spawnResponsible(program(), arguments(), env, &error);
    if (pid <= 0) {
        qCWarning(lcLauncher) << "Cannot start" << program() << error;
        scheduleRestart();
        return;
    }
    m_pid = pid;
    m_running.start();
    m_reap.start();
    return;
#endif
    m_process.setProcessEnvironment(env);
    m_running.start();
    m_process.start(program(), arguments());
}

void SessionLauncher::stopNative(int graceMs)
{
#ifdef Q_OS_WIN
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
#endif
    platform::terminateSessionProcess(&m_native, graceMs);
}

} // namespace df
