#pragma once

#include "platform/platform.h"

#include <QObject>
#include <QProcess>
#include <QTimer>

class QWinEventNotifier;

namespace df {

// Keeps deskflower-session running in the interactive session and restarts it when it exits
// or when the active console session changes (fast user switching, logon).
//  - Console mode / Linux / macOS: a child QProcess of the service.
//  - Windows service mode: LocalSystem inside the active console session.
class SessionLauncher : public QObject
{
    Q_OBJECT
public:
    SessionLauncher(const QString &instance, const QString &logDir, bool serviceMode, QObject *parent = nullptr);
    ~SessionLauncher() override;

    // Secret the helper must present in its IPC hello.
    QByteArray token() const { return m_token; }
    void start();

private:
    void launch();
    void scheduleRestart();
    void stopNative();
    QString program() const;
    QStringList arguments() const;

    QString m_instance;
    QString m_logDir;
    bool m_serviceMode;
    QByteArray m_token;
    bool m_stopping = false;
    QTimer m_restart;

    QProcess m_process;

    platform::SessionProcess m_native;
    QWinEventNotifier *m_notifier = nullptr;
    QTimer m_sessionCheck;
};

} // namespace df
