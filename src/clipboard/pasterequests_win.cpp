#include "pasterequests_win.h"

#include <QCborArray>
#include <QCborValue>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QtEndian>

namespace df::pasterequests {

namespace {
// The agent's texts keep their translation context.
QString tr(const char *text) { return QCoreApplication::translate("Backend", text); }
} // namespace

void writeRequest(QLocalSocket &socket, const QString &cmd, QCborMap request, int timeoutMs)
{
    request.insert(QStringLiteral("cmd"), cmd);
    const QByteArray body = request.toCborValue().toCbor();
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    socket.write(frame + body);
    socket.waitForBytesWritten(timeoutMs);
}

bool takeMessage(QByteArray &buffer, QCborMap *msg)
{
    if (buffer.size() < 4)
        return false;
    const quint32 size = qFromBigEndian<quint32>(buffer.constData());
    if (quint32(buffer.size() - 4) < size)
        return false;
    *msg = QCborValue::fromCbor(buffer.mid(4, int(size))).toMap();
    buffer.remove(0, int(size) + 4);
    return true;
}

QList<ClipboardBridge::ListedFile> listFiles(QLocalSocket &socket, const QByteArray &kid, QString *error,
                                             int timeoutMs)
{
    QCborMap args;
    args.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
    writeRequest(socket, QStringLiteral("clipList"), args, 3000);
    QList<ClipboardBridge::ListedFile> files;
    QByteArray buffer;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (ClipboardBridge::fetchesAbandoned()) {
            *error = tr("DeskFlower is closing");
            return {};
        }
        if (!socket.waitForReadyRead(500)) {
            if (socket.state() != QLocalSocket::ConnectedState)
                break;
            continue;
        }
        buffer += socket.readAll();
        QCborMap msg;
        while (takeMessage(buffer, &msg)) {
            if (msg.value(QStringLiteral("ev")).toString() != QLatin1String("clipListed"))
                continue; // state, thumbnails...: what every new connection gets
            if (!msg.value(QStringLiteral("ok")).toBool()) {
                *error = msg.value(QStringLiteral("error")).toString();
                return {};
            }
            for (const QCborValue &v : msg.value(QStringLiteral("items")).toArray())
                files.append(ClipboardBridge::listedFromCbor(v.toArray()));
            if (!msg.value(QStringLiteral("more")).toBool())
                return files;
        }
    }
    *error = tr("No answer from the DeskFlower service");
    return {};
}

void runStreamingFetch(QLocalSocket &socket, const QByteArray &kid, qint64 listedAgoMs, PasteSession::Link &link,
                       int idleTimeoutMs, bool fresh)
{
    using State = ClipboardBridge::PasteSessionBase::State;
    QCborMap args;
    if (!kid.isEmpty())
        args.insert(QStringLiteral("kid"), QString::fromLatin1(kid.toHex()));
    args.insert(QStringLiteral("stream"), true);
    if (listedAgoMs >= 0) // an older service ignores it
        args.insert(QStringLiteral("listedAgo"), listedAgoMs);
    if (fresh) // a move paste (sent only to a service that announced it understands it)
        args.insert(QStringLiteral("fresh"), true);
    writeRequest(socket, QStringLiteral("clipFetch"), args, 3000);
    QByteArray buffer;
    QElapsedTimer quiet; // since the service last said the transfer goes on
    quiet.start();
    bool ended = false;
    while (!link.stopRequested()) {
        if (ClipboardBridge::fetchesAbandoned()) { // the agent quits: it does not wait for the transfer
            link.finished(State::Failed, tr("DeskFlower is closing"));
            break;
        }
        if (!ended && quiet.elapsed() > idleTimeoutMs) {
            link.finished(State::Failed, tr("No answer from the DeskFlower service"));
            break;
        }
        if (!socket.waitForReadyRead(100)) {
            if (socket.state() != QLocalSocket::ConnectedState) {
                // After a successful end this changes nothing (only the first report counts): the
                // files are complete, the service merely stops keeping their folder.
                link.finished(State::Failed, tr("The DeskFlower service closed the connection"));
                break;
            }
            continue;
        }
        buffer += socket.readAll();
        QCborMap msg;
        while (!ended && takeMessage(buffer, &msg)) {
            const QString ev = msg.value(QStringLiteral("ev")).toString();
            if (ev == QLatin1String("clipFetching")) {
                quiet.restart();
            } else if (ev == QLatin1String("clipStarted")) {
                quiet.restart();
                link.started(msg.value(QStringLiteral("root")).toString());
            } else if (ev == QLatin1String("clipFetched")) {
                ended = true;
                if (msg.value(QStringLiteral("ok")).toBool()) {
                    // Files already here (an earlier paste brought them) come without clipStarted:
                    // the folder of the top-level items.
                    const QCborArray files = msg.value(QStringLiteral("files")).toArray();
                    if (!files.isEmpty())
                        link.started(QFileInfo(files.at(0).toString()).absolutePath());
                    link.finished(State::Succeeded);
                } else {
                    const QString code = msg.value(QStringLiteral("code")).toString();
                    if (code == QLatin1String("noPaste"))
                        link.finished(State::Failed, QString::fromLatin1(ClipboardBridge::kNotAPaste));
                    else if (code == QLatin1String("cancelled"))
                        link.finished(State::Cancelled, msg.value(QStringLiteral("error")).toString());
                    else
                        link.finished(State::Failed, msg.value(QStringLiteral("error")).toString());
                }
            }
        }
        if (ended && buffer.size() > 0)
            buffer.clear(); // nothing more is expected
    }
    // A move paste took the files: the service removes what is left of their folder and makes
    // the next paste transfer again (before this connection's end drops the folder's pin).
    if (link.consumeRequested() && socket.state() == QLocalSocket::ConnectedState)
        writeRequest(socket, QStringLiteral("clipConsumed"), {}, 3000);
    socket.abort();
}

} // namespace df::pasterequests
