#pragma once

// Windows: the agent's requests to the service for a streaming paste (see Backend::fileStreaming),
// on a connection the caller opened (the service's render server, or its main server). Blocking,
// without an event loop: they run on worker threads and on a paste session's thread.
//
// - listFiles: clipList {kid}; the names and sizes arrive in clipListed chunks ({ok, items, more}).
// - runStreamingFetch: clipFetch {kid, stream, listedAgo, fresh}; clipStarted gives the folder the files
//   arrive in, clipFetching says the transfer goes on, clipFetched {ok, code, files} ends it. A move
//   paste (fresh) whose files were moved away sends clipConsumed before it closes the connection.

#include "clipboardbridge.h"
#include "pastesession_win.h"

#include <QByteArray>
#include <QCborMap>
#include <QList>
#include <QString>

class QLocalSocket;

namespace df::pasterequests {

// How long a reader of the list of copied files (Explorer's descriptor) is kept waiting at most.
constexpr int kListTimeoutMs = 60 * 1000;

// Sends one framed request (4-byte big-endian size, then the CBOR map with "cmd").
void writeRequest(QLocalSocket &socket, const QString &cmd, QCborMap request, int timeoutMs);

// The next whole message in `buffer` (removed from it), if there is one.
bool takeMessage(QByteArray &buffer, QCborMap *msg);

// The names and sizes of the clipboard object `kid`'s files, gathered from the clipListed chunks
// (no contents move, no paste needed). Empty + error on refusal, timeout, a closed connection, or
// once ClipboardBridge::fetchesAbandoned().
QList<ClipboardBridge::ListedFile> listFiles(QLocalSocket &socket, const QByteArray &kid, QString *error,
                                             int timeoutMs = kListTimeoutMs);

// A streaming paste's request, run by its PasteSession (see pastesession_win.h). The connection
// stays open after a successful end (the service keeps the folder for the streams) until the
// session stops the request; closing it before the end drops the request, which cancels a transfer
// nobody else waits for. Without a word from the service for idleTimeoutMs while the transfer
// runs, it fails. Returns with the socket aborted. listedAgoMs (>= 0): how long ago the paste's
// reader read the list of the files (sent as listedAgo, see ClipboardSync::fetchFiles). fresh: a
// move paste (ClipboardBridge::FileStreaming::move): always a new transfer, and clipConsumed sent
// before the connection closes once the session was told consumed().
void runStreamingFetch(QLocalSocket &socket, const QByteArray &kid, qint64 listedAgoMs, PasteSession::Link &link,
                       int idleTimeoutMs, bool fresh = false);

} // namespace df::pasterequests
