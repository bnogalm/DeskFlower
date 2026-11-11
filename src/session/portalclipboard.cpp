#include "portalclipboard.h"

#include "portalsession.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QImage>
#include <QLoggingCategory>
#include <QMimeData>
#include <QPointer>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcPortalClip, "deskflower.portal.clipboard")

namespace {

const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kClipboard = QStringLiteral("org.freedesktop.portal.Clipboard");

constexpr int kReadTimeoutMs = 2000;
constexpr int kWriteTimeoutMs = 10000;
constexpr qint64 kMaxRead = 64 * 1024 * 1024; // as ClipboardSync's largest format

const QStringList kTextFormats = {QStringLiteral("text/plain;charset=utf-8"), QStringLiteral("text/plain"),
                                  QStringLiteral("UTF8_STRING"), QStringLiteral("STRING"), QStringLiteral("TEXT")};

// Reads a pipe to its end, giving up after kReadTimeoutMs (an application that never writes).
QByteArray readAll(int fd)
{
    QByteArray out;
    QElapsedTimer elapsed;
    elapsed.start();
    char buffer[65536];
    while (out.size() < kMaxRead) {
        const int left = int(kReadTimeoutMs - elapsed.elapsed());
        if (left <= 0)
            return {};
        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, left);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return {};
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            break; // end (or error: what arrived so far)
        out.append(buffer, int(n));
    }
    return out;
}

bool writeAll(int fd, const QByteArray &data)
{
    qint64 done = 0;
    while (done < data.size()) {
        pollfd p{fd, POLLOUT, 0};
        const int ready = ::poll(&p, 1, kWriteTimeoutMs);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return false;
        const ssize_t n = ::write(fd, data.constData() + done, size_t(data.size() - done));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

} // namespace

PortalClipboard::PortalClipboard(PortalSession *portal, QObject *parent) : ClipboardBackend(parent), m_portal(portal)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kClipboard, QStringLiteral("SelectionOwnerChanged"), this,
                SLOT(onOwnerChanged(QDBusObjectPath, QVariantMap)));
    bus.connect(kService, kPath, kClipboard, QStringLiteral("SelectionTransfer"), this,
                SLOT(onTransfer(QDBusObjectPath, QString, uint)));
    connect(m_portal, &PortalSession::started, this, [this] {
        if (m_own)
            announce(); // set before the session ran
    });
}

PortalClipboard::~PortalClipboard() = default;

QStringList PortalClipboard::offeredFormats(const QMimeData *data)
{
    QStringList out;
    if (!data)
        return out;
    const QStringList formats = data->formats();
    for (const QString &f : formats) {
        if (f == QLatin1String("application/x-qt-image")) {
            if (!out.contains(QStringLiteral("image/png")))
                out.append(QStringLiteral("image/png"));
            continue;
        }
        if (!out.contains(f))
            out.append(f);
        // Wayland applications ask for the explicit charset first.
        if (f == QLatin1String("text/plain") && !formats.contains(QStringLiteral("text/plain;charset=utf-8")))
            out.append(QStringLiteral("text/plain;charset=utf-8"));
    }
    return out;
}

QByteArray PortalClipboard::render(const QMimeData *data, const QString &mime)
{
    if (!data)
        return {};
    if (kTextFormats.contains(mime)) {
        // A lazy (sealed) object renders its own text formats; a plain one holds "text/plain".
        const QByteArray direct = data->data(mime);
        return direct.isEmpty() ? data->data(QStringLiteral("text/plain")) : direct;
    }
    if (mime == QLatin1String("image/png") && !data->formats().contains(QStringLiteral("image/png"))) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.isNull())
            image.save(&buffer, "PNG");
        return png;
    }
    return data->data(mime);
}

void PortalClipboard::setMimeData(QMimeData *data)
{
    m_own.reset(data);
    m_owned = true;
    m_snapshot.reset();
    m_snapshotValid = false;
    announce();
    emit changed();
}

void PortalClipboard::announce()
{
    if (!m_own || m_portal->state() != PortalSession::State::Started || !m_portal->clipboardEnabled())
        return;
    QVariantMap options;
    options.insert(QStringLiteral("mime_types"), offeredFormats(m_own.get()));
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kClipboard, QStringLiteral("SetSelection"));
    call << QVariant::fromValue(QDBusObjectPath(m_portal->handle())) << options;
    QDBusConnection::sessionBus().asyncCall(call);
}

void PortalClipboard::onOwnerChanged(const QDBusObjectPath &session, const QVariantMap &options)
{
    if (session.path() != m_portal->handle())
        return;
    if (options.value(QStringLiteral("session_is_owner")).toBool())
        return; // our own SetSelection
    m_owned = false;
    m_own.reset();
    m_available = options.value(QStringLiteral("mime_types")).toStringList();
    m_snapshot.reset();
    m_snapshotValid = false;
    emit changed();
}

void PortalClipboard::onTransfer(const QDBusObjectPath &session, const QString &mime, uint serial)
{
    if (session.path() != m_portal->handle())
        return;
    const QDBusObjectPath handle(m_portal->handle());
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kClipboard, QStringLiteral("SelectionWrite"));
    call << QVariant::fromValue(handle) << serial;
    const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(call);
    if (!reply.isValid()) {
        qCWarning(lcPortalClip) << "SelectionWrite failed:" << reply.error().message();
        return;
    }
    // Rendered here (a sealed format asks the service now: the paste); written on a thread, so
    // that a slow reader never holds this one. The render may run the event loop: the content is
    // held here in case another application copies (or this one sets new content) meanwhile.
    const std::shared_ptr<QMimeData> own = m_owned ? m_own : nullptr;
    const QByteArray data = render(own.get(), mime);
    const int fd = dup(reply.value().fileDescriptor());
    const QPointer<PortalClipboard> self(this);
    std::thread([self, fd, data, serial, handle] {
        const bool ok = !data.isEmpty() && writeAll(fd, data);
        ::close(fd);
        QMetaObject::invokeMethod(qApp, [self, ok, serial, handle] {
            if (!self)
                return;
            QDBusMessage done =
                QDBusMessage::createMethodCall(kService, kPath, kClipboard, QStringLiteral("SelectionWriteDone"));
            done << QVariant::fromValue(handle) << serial << ok;
            QDBusConnection::sessionBus().asyncCall(done);
        });
    }).detach();
}

QByteArray PortalClipboard::read(const QString &mime) const
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kClipboard, QStringLiteral("SelectionRead"));
    call << QVariant::fromValue(QDBusObjectPath(m_portal->handle())) << mime;
    const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(call);
    if (!reply.isValid()) {
        qCWarning(lcPortalClip) << "SelectionRead" << mime << "failed:" << reply.error().message();
        return {};
    }
    const int fd = dup(reply.value().fileDescriptor());
    const QByteArray data = readAll(fd);
    ::close(fd);
    return data;
}

const QMimeData *PortalClipboard::mimeData()
{
    if (m_owned)
        return m_own.get();
    if (m_snapshotValid)
        return m_snapshot.get();
    m_snapshotValid = true;
    m_snapshot.reset();
    if (m_portal->state() != PortalSession::State::Started || m_available.isEmpty())
        return nullptr;
    // Only what ClipboardBridge shares: text, HTML, a PNG image, copied files.
    auto *snapshot = new QMimeData;
    for (const QString &f : kTextFormats) {
        if (!m_available.contains(f))
            continue;
        const QByteArray text = read(f);
        if (!text.isEmpty()) {
            snapshot->setText(QString::fromUtf8(text));
            break;
        }
    }
    if (m_available.contains(QStringLiteral("text/html"))) {
        const QByteArray html = read(QStringLiteral("text/html"));
        if (!html.isEmpty())
            snapshot->setHtml(QString::fromUtf8(html));
    }
    if (m_available.contains(QStringLiteral("image/png"))) {
        QImage image;
        if (image.loadFromData(read(QStringLiteral("image/png")), "PNG"))
            snapshot->setImageData(image);
    }
    if (m_available.contains(QStringLiteral("text/uri-list"))) {
        const QByteArray list = read(QStringLiteral("text/uri-list"));
        if (!list.isEmpty())
            snapshot->setData(QStringLiteral("text/uri-list"), list);
    }
    m_snapshot.reset(snapshot);
    return snapshot;
}
