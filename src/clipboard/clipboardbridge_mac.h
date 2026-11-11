#pragma once

// macOS part of ClipboardBridge (clipboardbridge_mac.mm), on NSPasteboard directly:
//  - changeCount: the general pasteboard's change counter. Qt only looks at it when the
//    application is activated, which the background session helper never is, so ClipboardBridge
//    polls it to notice what the user copies;
//  - setSealed: sealed content as lazily provided pasteboard items: the text, HTML and PNG formats
//    are decrypted (unseal) and the copied files and folders transferred (fetch) only when an
//    application reads them, i.e. the user pastes. One item per top-level file or folder (`names`,
//    their order), each with its file URL, which is what Finder pastes; the formats are on the first
//    item. Without names but with `pendingFiles` (no full list: a guest's copy, or a long one) a
//    single item offers every path at once (the legacy NSFilenamesPboardType) and the first as its
//    file URL. Returns the change count once written (its own change, not to be reported back);
//  - rendering: whether an item is being rendered now (it may wait for the service in a nested
//    event loop): the pasteboard is not replaced meanwhile.

#include "clipboardbridge.h"

#include <QStringList>

namespace df::macclipboard {

qint64 changeCount();
bool rendering();
qint64 setSealed(const QList<ClipboardBridge::SealedFormat> &formats, const QStringList &names, int pendingFiles,
                 ClipboardBridge::UnsealFormat unseal, ClipboardBridge::FetchFiles fetch);

} // namespace df::macclipboard
