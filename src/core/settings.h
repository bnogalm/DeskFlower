#pragma once

// Small persistent key/value settings shared by the service's components
// (<config dir>/settings.json). Keys in use:
//   shareClipboard (bool), sharedFolder (string), phoneAccess (bool), phonePort (int),
//   remoteView (bool, default true: Advanced settings), dataPort (int), discoveryPort (int), logging (bool, default false: no log files; only set by
//   hand in the file, see installFileLogging)

#include <QJsonObject>
#include <QObject>

namespace df {

class Settings : public QObject
{
    Q_OBJECT
public:
    explicit Settings(const QString &configDir, QObject *parent = nullptr);

    QJsonValue value(const QString &key, const QJsonValue &defaultValue = {}) const;
    void setValue(const QString &key, const QJsonValue &value);

signals:
    void changed(const QString &key);

private:
    QString m_path;
    QJsonObject m_values;
};

} // namespace df
