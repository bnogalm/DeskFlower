#pragma once

#include <QString>

namespace df::crash {

// Always-on record of what ended a process badly, independent of the "logging" setting (log
// files are off by default, so a crash would otherwise leave nothing behind):
//   - <dir>/problems.log: one line when the process starts and one when it exits normally
//     (markCleanExit), so that a start without an exit shows an end nothing could report, and a
//     line for each qFatal, std::terminate (with the exception's what()), abort, unhandled
//     exception (Windows: code, module and offset) or fatal signal (Unix). Kept small: past
//     kMaxLogBytes it is moved to problems.log.1 at the next start. Like the log files it never
//     holds typed keys, clipboard contents or file names: qFatal texts come from the code.
//   - Windows: a minidump of the crash, <dir>/<name>-<yyyyMMdd-HHmmss>.dmp (thread stacks and
//     loaded modules only, no heap), the newest kMaxDumps kept. Elsewhere the OS's core dumps
//     are the tool.
// Call once, early, after installFileLogging (its message handler is chained) and with a
// directory only the process's user can read: the service's logs folder (SYSTEM and
// Administrators), the agent's under the user's local application data, the session helper's in
// <service logs>/session.
void install(const QString &dir, const QString &name);
// The process is about to exit normally (after the event loop returned).
void markCleanExit(int code);

QString problemsLogPath(const QString &dir);

constexpr qint64 kMaxLogBytes = 256 * 1024;
constexpr int kMaxDumps = 3;

} // namespace df::crash
