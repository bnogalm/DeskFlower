#pragma once

#include <QString>

namespace df {

// Sends Qt log messages to <dir>/<fileName> (rotated at 5 MB) and, when requested, to stderr.
void installFileLogging(const QString &dir, const QString &fileName, bool alsoStderr);

} // namespace df
