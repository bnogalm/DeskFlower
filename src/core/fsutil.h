#pragma once

// File system helpers for code that runs with more rights than the user whose folders it writes
// to (the Windows service runs as LocalSystem): a link the user plants must not redirect it.

#include <QString>

namespace df::fs {

// Whether an existing component of the absolute `path` is a symbolic link or a Windows junction
// (mount points included). Windows only, where the service runs as LocalSystem; false elsewhere.
bool passesThroughLink(const QString &path);

// Whether `path` is `folder` or inside it (both absolute; case-insensitive on Windows).
bool isInside(const QString &path, const QString &folder);

// Deletes `path` and everything below it without following links: a link is removed, not what it
// points to. Returns false if something could not be deleted.
bool removeTree(const QString &path);

} // namespace df::fs
