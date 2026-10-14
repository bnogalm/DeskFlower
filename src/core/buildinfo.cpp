#include "buildinfo.h"

#include "buildconfig.h"

#include <QSysInfo>

namespace df::build {

QString version() { return QStringLiteral(DESKFLOWER_VERSION); }

QString platform()
{
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    const QString os = QStringLiteral("macos");
#else
    const QString os = QStringLiteral("linux");
#endif
    return os + QLatin1Char('-') + QSysInfo::buildCpuArchitecture();
}

QByteArray updatePublicKey() { return QByteArray::fromHex(DESKFLOWER_UPDATE_PUBKEY); }

} // namespace df::build
