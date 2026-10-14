// Wayland input backend (GNOME 45+, KDE Plasma 6+), through xdg-desktop-portal and libei.
//  - Injection: RemoteDesktop portal via liboeffis (asks the user once per session), then a libei
//    *sender* context: absolute pointer (logical coordinates), buttons, scroll and keys.
//  - Capture: InputCapture portal. Pointer barriers are placed on the outer edges of the desktop
//    zones; when the cursor hits one the compositor activates the capture and delivers input to a
//    libei *receiver* context until we call Release (with the position to put the cursor back).
// NOTE: written without a Linux build environment; see the README.

#include "inputbackend.h"
#include "keymap.h"
#include "linuxbackends.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRect>
#include <QSocketNotifier>
#include <QTimer>

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unistd.h>

extern "C" {
#include <libei.h>
#include <liboeffis.h>
}

Q_LOGGING_CATEGORY(lcWayland, "deskflower.input.wayland")

// Named (not anonymous) namespace: moc-generated code must be able to name the class.
namespace deskflower_wayland {

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kInputCapture = QStringLiteral("org.freedesktop.portal.InputCapture");
const QString kRequest = QStringLiteral("org.freedesktop.portal.Request");

// evdev button codes.
constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112, kBtnSide = 0x113, kBtnExtra = 0x114;
// If the service does not take the capture (no device beyond that edge) within this time, give
// the pointer back to the local desktop.
constexpr int kActivationTimeoutMs = 400;

uint32_t toEvdevButton(int button)
{
    switch (button) {
    case 1: return kBtnLeft;
    case 2: return kBtnRight;
    case 3: return kBtnMiddle;
    case 4: return kBtnSide;
    case 5: return kBtnExtra;
    default: return 0;
    }
}

int fromEvdevButton(uint32_t code)
{
    switch (code) {
    case kBtnLeft: return 1;
    case kBtnRight: return 2;
    case kBtnMiddle: return 3;
    case kBtnSide: return 4;
    case kBtnExtra: return 5;
    default: return 0;
    }
}

class WaylandInputBackend : public InputBackend, protected QDBusContext
{
    Q_OBJECT
public:
    explicit WaylandInputBackend(QObject *parent) : InputBackend(parent)
    {
        m_activationTimer.setSingleShot(true);
        m_activationTimer.setInterval(kActivationTimeoutMs);
        connect(&m_activationTimer, &QTimer::timeout, this, [this] {
            if (m_capturing && !m_captureAccepted)
                release(m_activationCursor); // nothing beyond that edge
        });
    }

    ~WaylandInputBackend() override
    {
        releaseAll();
        for (ei_device *d : m_senderDevices)
            ei_device_unref(d);
        for (ei_device *d : m_receiverDevices)
            ei_device_unref(d);
        if (m_sender)
            ei_unref(m_sender);
        if (m_receiver)
            ei_unref(m_receiver);
        if (m_oeffis)
            oeffis_unref(m_oeffis);
    }

    bool start() override
    {
        startEmulation();
        startCapture();
        return true; // both parts finish asynchronously (portal dialogs)
    }

    bool isCapturing() const override { return m_capturing; }

    void setReportEdges(bool report) override
    {
        m_reportEdges = report;
        applyBarriers();
    }

    void setCapture(bool on, QPoint returnTo) override
    {
        if (on) {
            m_captureAccepted = true; // keep the compositor's capture
            m_activationTimer.stop();
        } else if (m_capturing) {
            release(QPointF(returnTo));
        }
    }

    // --- Injection --------------------------------------------------------------------------

    void moveTo(QPoint p) override
    {
        if (ei_device *dev = senderDevice(EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
            ei_device_pointer_motion_absolute(dev, p.x(), p.y());
            ei_device_frame(dev, ei_now(m_sender));
        }
    }

    void button(int button, bool down, QPoint at) override
    {
        moveTo(at);
        const uint32_t code = toEvdevButton(button);
        ei_device *dev = senderDevice(EI_DEVICE_CAP_BUTTON);
        if (!dev || !code)
            return;
        ei_device_button_button(dev, code, down);
        ei_device_frame(dev, ei_now(m_sender));
        if (down)
            m_injectedButtons.insert(code);
        else
            m_injectedButtons.erase(code);
    }

    void wheel(int dx, int dy) override
    {
        if (ei_device *dev = senderDevice(EI_DEVICE_CAP_SCROLL)) {
            // libei: 120 per detent, positive y scrolls down; ours: positive y scrolls up.
            ei_device_scroll_discrete(dev, dx, -dy);
            ei_device_frame(dev, ei_now(m_sender));
        }
    }

    void key(quint32 scancode, bool down) override
    {
        const int evdev = df::keymap::evdevFromSet1(scancode);
        ei_device *dev = senderDevice(EI_DEVICE_CAP_KEYBOARD);
        if (!dev || evdev < 0)
            return;
        ei_device_keyboard_key(dev, uint32_t(evdev), down);
        ei_device_frame(dev, ei_now(m_sender));
        if (down)
            m_injectedKeys.insert(uint32_t(evdev));
        else
            m_injectedKeys.erase(uint32_t(evdev));
    }

    void releaseAll() override
    {
        if (ei_device *dev = senderDevice(EI_DEVICE_CAP_KEYBOARD)) {
            for (uint32_t k : m_injectedKeys)
                ei_device_keyboard_key(dev, k, false);
            ei_device_frame(dev, ei_now(m_sender));
        }
        if (ei_device *dev = senderDevice(EI_DEVICE_CAP_BUTTON)) {
            for (uint32_t b : m_injectedButtons)
                ei_device_button_button(dev, b, false);
            ei_device_frame(dev, ei_now(m_sender));
        }
        m_injectedKeys.clear();
        m_injectedButtons.clear();
    }

private slots:
    // org.freedesktop.portal.InputCapture signals.
    void onActivated(const QDBusObjectPath &session, const QVariantMap &options)
    {
        if (session.path() != m_session)
            return;
        m_activationId = options.value(QStringLiteral("activation_id")).toUInt();
        m_activationCursor = readPoint(options.value(QStringLiteral("cursor_position")));
        const uint barrier = options.value(QStringLiteral("barrier_id")).toUInt();
        m_capturing = true;
        m_captureAccepted = false;
        m_accX = m_accY = 0;
        const int side = m_barrierSides.count(barrier) ? m_barrierSides[barrier] : -1;
        if (side < 0 || !m_reportEdges) {
            release(m_activationCursor);
            return;
        }
        m_activationTimer.start();
        emit edgeHit(side, m_activationCursor.toPoint());
    }

    void onDeactivated(const QDBusObjectPath &session, const QVariantMap &)
    {
        if (session.path() == m_session)
            m_capturing = false;
    }

    void onZonesChanged(const QDBusObjectPath &session, const QVariantMap &)
    {
        if (session.path() == m_session)
            requestZones();
    }

    void onDisabled(const QDBusObjectPath &session, const QVariantMap &)
    {
        if (session.path() == m_session) {
            m_capturing = false;
            m_enabled = false;
        }
    }

    void onRequestResponse(uint response, const QVariantMap &results)
    {
        const QString path = message().path();
        auto it = m_pending.find(path);
        if (it == m_pending.end())
            return;
        auto callback = std::move(it->second);
        m_pending.erase(it);
        QDBusConnection::sessionBus().disconnect(kPortalService, path, kRequest, QStringLiteral("Response"), this,
                                                 SLOT(onRequestResponse(uint, QVariantMap)));
        if (response != 0) {
            qCWarning(lcWayland) << "Portal request" << path << "was refused or cancelled:" << response;
            return;
        }
        callback(results);
    }

private:
    // --- Emulation (RemoteDesktop portal through liboeffis) ---------------------------------

    void startEmulation()
    {
        m_oeffis = oeffis_new(nullptr);
        if (!m_oeffis)
            return;
        auto *notifier = new QSocketNotifier(oeffis_get_fd(m_oeffis), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] { dispatchOeffis(); });
        oeffis_create_session(m_oeffis, OEFFIS_DEVICE_KEYBOARD | OEFFIS_DEVICE_POINTER);
    }

    void dispatchOeffis()
    {
        oeffis_dispatch(m_oeffis);
        for (enum oeffis_event_type e = oeffis_get_event(m_oeffis); e != OEFFIS_EVENT_NONE;
             e = oeffis_get_event(m_oeffis)) {
            switch (e) {
            case OEFFIS_EVENT_CONNECTED_TO_EIS:
                setUpSender(oeffis_get_eis_fd(m_oeffis));
                break;
            case OEFFIS_EVENT_CLOSED:
            case OEFFIS_EVENT_DISCONNECTED:
                qCWarning(lcWayland) << "Remote desktop session ended:" << oeffis_get_error_message(m_oeffis);
                break;
            default:
                break;
            }
        }
    }

    void setUpSender(int fd)
    {
        m_sender = ei_new_sender(nullptr);
        ei_configure_name(m_sender, "DeskFlower");
        if (ei_setup_backend_fd(m_sender, fd) != 0) {
            qCWarning(lcWayland) << "Cannot connect to the compositor's EIS (emulation)";
            return;
        }
        auto *notifier = new QSocketNotifier(ei_get_fd(m_sender), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] { dispatchEi(m_sender, true); });
        qCInfo(lcWayland) << "Input emulation ready";
    }

    ei_device *senderDevice(enum ei_device_capability cap) const
    {
        for (ei_device *d : m_senderDevices) {
            if (m_resumed.count(d) && ei_device_has_capability(d, cap))
                return d;
        }
        return nullptr;
    }

    // --- Capture (InputCapture portal + libei receiver) -------------------------------------

    void startCapture()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(kPortalService, kPortalPath, kInputCapture, QStringLiteral("Activated"), this,
                    SLOT(onActivated(QDBusObjectPath, QVariantMap)));
        bus.connect(kPortalService, kPortalPath, kInputCapture, QStringLiteral("Deactivated"), this,
                    SLOT(onDeactivated(QDBusObjectPath, QVariantMap)));
        bus.connect(kPortalService, kPortalPath, kInputCapture, QStringLiteral("ZonesChanged"), this,
                    SLOT(onZonesChanged(QDBusObjectPath, QVariantMap)));
        bus.connect(kPortalService, kPortalPath, kInputCapture, QStringLiteral("Disabled"), this,
                    SLOT(onDisabled(QDBusObjectPath, QVariantMap)));

        const QString sessionToken = token();
        QVariantMap options;
        options.insert(QStringLiteral("session_handle_token"), sessionToken);
        options.insert(QStringLiteral("capabilities"), uint(1 | 2)); // keyboard | pointer
        portalRequest(QStringLiteral("CreateSession"), {QString(), options}, options, [this](const QVariantMap &r) {
            m_session = r.value(QStringLiteral("session_handle")).value<QDBusObjectPath>().path();
            if (m_session.isEmpty())
                m_session = r.value(QStringLiteral("session_handle")).toString();
            qCInfo(lcWayland) << "Input capture session" << m_session;
            connectReceiver();
            requestZones();
        });
    }

    void connectReceiver()
    {
        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kInputCapture,
                                                           QStringLiteral("ConnectToEIS"));
        call << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariantMap();
        const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(call);
        if (!reply.isValid()) {
            qCWarning(lcWayland) << "ConnectToEIS failed:" << reply.error().message();
            return;
        }
        m_receiver = ei_new_receiver(nullptr);
        ei_configure_name(m_receiver, "DeskFlower capture");
        if (ei_setup_backend_fd(m_receiver, dup(reply.value().fileDescriptor())) != 0) {
            qCWarning(lcWayland) << "Cannot connect to the compositor's EIS (capture)";
            return;
        }
        auto *notifier = new QSocketNotifier(ei_get_fd(m_receiver), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] { dispatchEi(m_receiver, false); });
    }

    void requestZones()
    {
        QVariantMap options;
        portalRequest(QStringLiteral("GetZones"), {QVariant::fromValue(QDBusObjectPath(m_session)), options}, options,
                      [this](const QVariantMap &r) {
                          m_zones.clear();
                          const QDBusArgument arg = r.value(QStringLiteral("zones")).value<QDBusArgument>();
                          arg.beginArray();
                          while (!arg.atEnd()) {
                              uint width = 0, height = 0;
                              int x = 0, y = 0;
                              arg.beginStructure();
                              arg >> width >> height >> x >> y;
                              arg.endStructure();
                              m_zones.append(QRect(x, y, int(width), int(height)));
                          }
                          arg.endArray();
                          m_zoneSet = r.value(QStringLiteral("zone_set")).toUInt();
                          applyBarriers();
                      });
    }

    bool insideAnyZone(int x, int y) const
    {
        for (const QRect &z : m_zones) {
            if (z.contains(x, y))
                return true;
        }
        return false;
    }

    // One barrier per zone edge that has no other zone beyond it.
    void applyBarriers()
    {
        if (m_session.isEmpty() || m_zones.isEmpty())
            return;
        if (!m_reportEdges) {
            if (m_enabled)
                callSimple(QStringLiteral("Disable"));
            m_enabled = false;
            return;
        }
        QDBusArgument barriers;
        barriers.beginArray(QMetaType::fromType<QVariantMap>());
        m_barrierSides.clear();
        uint id = 1;
        auto add = [&](int side, int x1, int y1, int x2, int y2) {
            QDBusArgument position;
            position.beginStructure();
            position << x1 << y1 << x2 << y2;
            position.endStructure();
            QVariantMap barrier;
            barrier.insert(QStringLiteral("barrier_id"), id);
            barrier.insert(QStringLiteral("position"), QVariant::fromValue(position));
            barriers << barrier;
            m_barrierSides[id++] = side;
        };
        for (const QRect &z : m_zones) {
            const int left = z.x(), top = z.y(), right = z.x() + z.width(), bottom = z.y() + z.height();
            if (!insideAnyZone(left - 1, z.center().y()))
                add(0, left, top, left, bottom - 1);
            if (!insideAnyZone(right, z.center().y()))
                add(1, right, top, right, bottom - 1);
            if (!insideAnyZone(z.center().x(), top - 1))
                add(2, left, top, right - 1, top);
            if (!insideAnyZone(z.center().x(), bottom))
                add(3, left, bottom, right - 1, bottom);
        }
        barriers.endArray();

        QVariantMap options;
        portalRequest(QStringLiteral("SetPointerBarriers"),
                      {QVariant::fromValue(QDBusObjectPath(m_session)), options, QVariant::fromValue(barriers), m_zoneSet},
                      options, [this](const QVariantMap &) {
                          callSimple(QStringLiteral("Enable"));
                          m_enabled = true;
                      });
    }

    void release(QPointF cursor)
    {
        if (m_session.isEmpty())
            return;
        QDBusArgument position;
        position.beginStructure();
        position << cursor.x() << cursor.y();
        position.endStructure();
        QVariantMap options;
        options.insert(QStringLiteral("activation_id"), m_activationId);
        options.insert(QStringLiteral("cursor_position"), QVariant::fromValue(position));
        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kInputCapture,
                                                           QStringLiteral("Release"));
        call << QVariant::fromValue(QDBusObjectPath(m_session)) << options;
        QDBusConnection::sessionBus().asyncCall(call);
        m_capturing = false;
        m_activationTimer.stop();
    }

    void callSimple(const QString &method)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kInputCapture, method);
        call << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariantMap();
        QDBusConnection::sessionBus().asyncCall(call);
    }

    static QPointF readPoint(const QVariant &value)
    {
        double x = 0, y = 0;
        const QDBusArgument arg = value.value<QDBusArgument>();
        arg.beginStructure();
        arg >> x >> y;
        arg.endStructure();
        return QPointF(x, y);
    }

    // --- libei event loop (both contexts) ---------------------------------------------------

    void dispatchEi(ei *context, bool sender)
    {
        if (!context)
            return;
        ei_dispatch(context);
        while (ei_event *e = ei_get_event(context)) {
            switch (ei_event_get_type(e)) {
            case EI_EVENT_SEAT_ADDED:
                if (sender)
                    ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                              EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD,
                                              nullptr);
                else
                    ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON,
                                              EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD, nullptr);
                break;
            case EI_EVENT_DEVICE_ADDED:
                (sender ? m_senderDevices : m_receiverDevices).insert(ei_device_ref(ei_event_get_device(e)));
                break;
            case EI_EVENT_DEVICE_REMOVED: {
                ei_device *d = ei_event_get_device(e);
                auto &devices = sender ? m_senderDevices : m_receiverDevices;
                if (devices.erase(d))
                    ei_device_unref(d);
                m_resumed.erase(d);
                break;
            }
            case EI_EVENT_DEVICE_RESUMED:
                m_resumed.insert(ei_event_get_device(e));
                if (sender)
                    ei_device_start_emulating(ei_event_get_device(e), ++m_sequence);
                break;
            case EI_EVENT_DEVICE_PAUSED:
                m_resumed.erase(ei_event_get_device(e));
                break;
            case EI_EVENT_DISCONNECT:
                qCWarning(lcWayland) << (sender ? "Emulation" : "Capture") << "disconnected by the compositor";
                break;
            default:
                if (!sender)
                    handleCaptured(e);
                break;
            }
            ei_event_unref(e);
        }
    }

    void handleCaptured(ei_event *e)
    {
        if (!m_capturing)
            return;
        switch (ei_event_get_type(e)) {
        case EI_EVENT_POINTER_MOTION: {
            m_accX += ei_event_pointer_get_dx(e);
            m_accY += ei_event_pointer_get_dy(e);
            const int dx = int(m_accX), dy = int(m_accY);
            m_accX -= dx;
            m_accY -= dy;
            if (dx || dy)
                emit moved(dx, dy);
            break;
        }
        case EI_EVENT_BUTTON_BUTTON:
            if (const int b = fromEvdevButton(ei_event_button_get_button(e)))
                emit buttonEvent(b, ei_event_button_get_is_press(e));
            break;
        case EI_EVENT_SCROLL_DISCRETE:
            emit wheelEvent(ei_event_scroll_get_discrete_dx(e), -ei_event_scroll_get_discrete_dy(e));
            break;
        case EI_EVENT_SCROLL_DELTA: {
            // Smooth scrolling (touchpads), in logical pixels: about 50 px per wheel notch.
            const int dx = int(std::lround(ei_event_scroll_get_dx(e) * 2.4));
            const int dy = int(std::lround(-ei_event_scroll_get_dy(e) * 2.4));
            if (dx || dy)
                emit wheelEvent(dx, dy);
            break;
        }
        case EI_EVENT_KEYBOARD_KEY:
            if (const quint32 set1 = df::keymap::set1FromEvdev(int(ei_event_keyboard_get_key(e))))
                emit keyEvent(set1, ei_event_keyboard_get_key_is_press(e));
            break;
        default:
            break;
        }
    }

    // --- Portal request plumbing ------------------------------------------------------------

    static QString token() { return QStringLiteral("deskflower%1").arg(QRandomGenerator::global()->generate()); }

    // Calls an InputCapture method returning a Request handle; `callback` gets the results of the
    // Request's Response signal. The handle path is predictable, so we subscribe before calling.
    void portalRequest(const QString &method, const QVariantList &args, QVariantMap &options,
                       std::function<void(const QVariantMap &)> callback)
    {
        const QString handleToken = token();
        options.insert(QStringLiteral("handle_token"), handleToken);
        QVariantList finalArgs = args;
        for (QVariant &a : finalArgs) {
            if (a.metaType() == QMetaType::fromType<QVariantMap>())
                a = options; // the options argument now carries the handle token
        }
        QDBusConnection bus = QDBusConnection::sessionBus();
        QString sender = bus.baseService().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handleToken);
        m_pending[path] = std::move(callback);
        bus.connect(kPortalService, path, kRequest, QStringLiteral("Response"), this,
                    SLOT(onRequestResponse(uint, QVariantMap)));

        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kInputCapture, method);
        call.setArguments(finalArgs);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [method](QDBusPendingCallWatcher *w) {
            if (w->isError())
                qCWarning(lcWayland) << "InputCapture." << method << "failed:" << w->error().message();
            w->deleteLater();
        });
    }

    // Emulation.
    oeffis *m_oeffis = nullptr;
    ei *m_sender = nullptr;
    std::set<ei_device *> m_senderDevices;
    uint32_t m_sequence = 0;
    std::set<uint32_t> m_injectedKeys;
    std::set<uint32_t> m_injectedButtons;

    // Capture.
    ei *m_receiver = nullptr;
    std::set<ei_device *> m_receiverDevices;
    std::set<ei_device *> m_resumed;
    QString m_session;
    QList<QRect> m_zones;
    uint m_zoneSet = 0;
    std::map<uint, int> m_barrierSides;
    bool m_reportEdges = false;
    bool m_enabled = false;
    bool m_capturing = false;
    bool m_captureAccepted = false;
    uint m_activationId = 0;
    QPointF m_activationCursor;
    QTimer m_activationTimer;
    double m_accX = 0;
    double m_accY = 0;
    std::map<QString, std::function<void(const QVariantMap &)>> m_pending;
};

} // namespace deskflower_wayland

InputBackend *createWaylandInputBackend(QObject *parent)
{
    return new deskflower_wayland::WaylandInputBackend(parent);
}

#include "inputbackend_wayland.moc"
