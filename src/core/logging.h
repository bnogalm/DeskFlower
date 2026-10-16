#pragma once

#include <QString>

namespace df {

// Log files are off unless the "logging" setting is true (settings.json only, no UI). When on,
// Qt messages from Info up go to <dir>/<fileName> (rotated at 5 MB); debug messages never go to a
// file. When off, no file is written and earlier ones (<fileName>, <fileName>.1) are deleted.
// Log messages are informative only: never typed keys, clipboard contents, file names or paths.
// alsoStderr: every message (debug included) also goes to stderr (console mode).
void installFileLogging(const QString &dir, const QString &fileName, bool alsoStderr, bool toFile);

} // namespace df
