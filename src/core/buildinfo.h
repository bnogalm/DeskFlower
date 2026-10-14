#pragma once

// Facts fixed when Deskflower is built (from the top-level CMakeLists.txt).

#include <QByteArray>
#include <QString>

namespace df::build {

// PROJECT_VERSION, e.g. "0.1.0".
QString version();
// Which packages can be installed here: "<os>-<cpu>", e.g. "windows-x86_64", "linux-arm64".
QString platform();
// Ed25519 key that update packages must be signed with (DESKFLOWER_UPDATE_PUBKEY, 32 bytes).
// Empty when the build has none: updates from paired devices are then disabled.
QByteArray updatePublicKey();

} // namespace df::build
