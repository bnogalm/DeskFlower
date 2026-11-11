#pragma once

#include <QString>
#include <QStringList>

namespace df {

// Log files are off unless the "logging" setting is true (settings.json only, no UI). When on,
// Qt messages from Info up go to <dir>/<fileName> (rotated at 5 MB); debug messages never go to a
// file. When off, no file is written and earlier ones (<fileName>, <fileName>.1) are deleted.
// Log messages are informative only: never typed keys, clipboard contents, file names or paths.
// alsoStderr: every message (debug included) also goes to stderr (console mode).
void installFileLogging(const QString &dir, const QString &fileName, bool alsoStderr, bool toFile);

// Always-on diagnostics for features whose failures are otherwise impossible to tell apart after
// the fact (dragging files between devices): Info and above from the categories that start with
// one of `categoryPrefixes` go to <dir>/<fileName> whatever the "logging" setting, kept small
// (rotated at kDiagnosticLogSize, one older file). The same rule as for the other logs applies:
// milestones, counts, sizes and results only, never names or paths. Can be combined with
// installFileLogging (either order); messages still reach the handler installed before.
inline constexpr qint64 kDiagnosticLogSize = 512 * 1024;
void installDiagnosticLog(const QString &dir, const QString &fileName, const QStringList &categoryPrefixes);

} // namespace df
