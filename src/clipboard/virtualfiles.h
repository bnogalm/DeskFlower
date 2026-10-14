#pragma once

// Windows: files that live on another device, offered as virtual files (FileGroupDescriptorW with
// names, sizes and folders, FileContents with one stream per file). Reading the list costs nothing;
// the first contents request calls `fetch`, which transfers the files and returns their local
// top-level items, and every file is then served as a stream of the local copy. Data the shell sets
// on it (drag image, drop description...) is kept and returned. Used for pasting (clipboard) and
// for dragging files from another device.

#include "clipboardbridge.h"

struct IDataObject;

// A new object with one reference. preferredEffect: DROPEFFECT_COPY or DROPEFFECT_MOVE.
IDataObject *createVirtualFiles(const QList<ClipboardBridge::ListedFile> &files, ClipboardBridge::FetchFiles fetch,
                                unsigned long preferredEffect = 1 /* DROPEFFECT_COPY */);
