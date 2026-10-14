#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace df {

Settings::Settings(const QString &configDir, QObject *parent)
    : QObject(parent), m_path(QDir(configDir).filePath(QStringLiteral("settings.json")))
{
    QFile f(m_path);
    if (f.open(QIODevice::ReadOnly))
        m_values = QJsonDocument::fromJson(f.readAll()).object();
}

QJsonValue Settings::value(const QString &key, const QJsonValue &defaultValue) const
{
    return m_values.contains(key) ? m_values.value(key) : defaultValue;
}

void Settings::setValue(const QString &key, const QJsonValue &value)
{
    if (m_values.value(key) == value && m_values.contains(key))
        return;
    m_values.insert(key, value);
    QSaveFile f(m_path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_values).toJson());
        f.commit();
    }
    emit changed(key);
}

} // namespace df
