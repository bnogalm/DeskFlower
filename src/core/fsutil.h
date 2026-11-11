#pragma once

// File system helpers for code that runs with more rights than the user whose folders it writes
// to (the Windows service runs as LocalSystem): a link the user plants must not redirect it.

#include <QString>

#include <functional>
#include <memory>

namespace df::fs {

// Whether an existing component of the absolute `path` is a symbolic link or a Windows junction
// (mount points included; a .lnk shortcut is an ordinary file, not a link). Windows only, where the
// service runs as LocalSystem; false elsewhere. Checked on the destinations the service writes to
// (received files and folders, drop folders); reads are done as the user and need no check.
bool passesThroughLink(const QString &path);

// Whether `path` is `folder` or inside it (both absolute; case-insensitive on Windows).
bool isInside(const QString &path, const QString &folder);

// Deletes `path` and everything below it without following links: a link is removed, not what it
// points to. Returns false if something could not be deleted. A `stop` that returns true (asked
// before each entry) ends the walk at once, leaving the rest in place: false then too.
bool removeTree(const QString &path, const std::function<bool()> &stop = {});

// A received file being written (FileTransfer's writers): created (or truncated), written in order,
// closed. Never resized ahead: its size on disk is what was written, which a reader that follows
// it (a paste served while it arrives) takes as progress. Windows: CreateFileW with a long path,
// shared for reading and writing (such a reader opens it while it grows), without following a
// reparse point at its own name, and refused when that name is a link. Elsewhere: QFile.
class WriteFile
{
public:
    WriteFile();
    ~WriteFile(); // close()
    WriteFile(const WriteFile &) = delete;
    WriteFile &operator=(const WriteFile &) = delete;

    bool open(const QString &path); // closes the one open first
    bool write(const char *data, qint64 size);
    bool close(); // true when nothing was open
    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// A destination folder and the folders made below it, held open while files are written there
// (FileTransfer's received files), so that no link can redirect them and no link check is needed
// per file.
//
// Windows (the service writes as LocalSystem impersonating the user): open() checks every existing
// folder of the root's path once, from the drive down, and holds each open without
// FILE_SHARE_DELETE, so that none of them can be renamed, deleted or replaced by a junction while
// the tree is open; the missing tail of the path is made later, by ensureDir, the same way.
// ensureDir creates a folder (and its missing parents: an existing one is fine), opens it without
// following a reparse point and refuses it unless it is a directory that is not a link (a
// symbolic link or a junction, as passesThroughLink); then it holds it like the others. A folder
// already held costs one lookup. Paths are long paths (\\?\). The folders cannot be renamed or
// deleted by anybody while the tree is open: close() it before removing them (removeTree) and
// before the files are handed on.
// Elsewhere (the service runs as the user, passesThroughLink is a no-op): ensureDir is mkpath with
// the same cache, and nothing is held.
// Thread-safe. Call open() before anything else; ensureDir as the user who may write there.
class SafeTree
{
public:
    SafeTree();
    ~SafeTree(); // close()
    SafeTree(const SafeTree &) = delete;
    SafeTree &operator=(const SafeTree &) = delete;

    // False, with the error, when an existing folder of the path is a link (or cannot be opened).
    bool open(const QString &root, QString *error);
    // `rel`: '/' separated, relative to the root ("" is the root itself), already sanitised.
    bool ensureDir(const QString &rel, QString *error);
    void close();
    QString root() const;

    // For tests: folders held open now, and how many folders ensureDir had to look at on disk
    // (not found in its cache).
    int heldForTests() const;
    int diskLookupsForTests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace df::fs
