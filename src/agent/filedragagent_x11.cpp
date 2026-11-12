// Dragging files between devices on Linux X11, through the XDND protocol (version 5, as GTK and
// Qt implement it), with a connection of our own to the X server:
//  - catch: a tiny XDND-aware window is put under the cursor and the pointer nudged, so the drag in
//    progress enters it like any drop target; its files are read from XdndSelection (text/uri-list,
//    with INCR for long lists) while it hovers there, and the local drag is then cancelled with an
//    Escape (XTest) instead of being dropped;
//  - hover: this device acts as an XDND drag source following the cursor (which the master moves):
//    the window under it gets XdndEnter/XdndPosition, and on the master's drop XdndDrop. Files
//    from another device are transferred first (the drop waits; progress in the agent), then the
//    target reads their local paths; the originals of this device are offered as they are (move).
//    Only a move the target confirms (XdndFinished, version 5, with XdndActionMove) of files that
//    were transferred lets the originals on the other device go; anything else is a copy.
//    It ends when the master says (endHover), or here by itself: Escape, the service silent for
//    kAliveTimeoutMs (once it said dragAlive), this device's own button released while it is the
//    master and the service says nothing, or the connection to the service lost.
// Wayland gives a background program neither: there FileDragAgent::supportsNativeDrag() is false,
// the master sends files dragged over this device with drag.drop and they go into the Downloads
// folder (see docs/DESIGN.md).

#include "filedragagent.h"

#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QLoggingCategory>
#include <QPointer>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QUrl>

#include <climits>
#include <utility>
#include <thread>

// X11 headers last: they define macros (None, Bool, KeyPress...) that clash with Qt.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

Q_LOGGING_CATEGORY(lcXdnd, "deskflower.drag.x11")

namespace {

constexpr unsigned long kNone = 0L; // Xlib's "None"
constexpr long kXdndVersion = 5;
constexpr int kFeedIntervalMs = 30;
constexpr int kStatusGraceMs = 500;      // a drop waits this long for the target's last status
constexpr int kFinishTimeoutMs = 120000; // and this long for XdndFinished once dropped

// The process's own X connection for dragging (opened on first use; null without X11).
class XLink
{
public:
    static XLink *get()
    {
        static XLink *link = [] {
            auto *l = new XLink;
            if (!l->open()) {
                delete l;
                return static_cast<XLink *>(nullptr);
            }
            return l;
        }();
        return link;
    }

    Display *dpy = nullptr;
    Window root = 0;
    bool xtest = false;
    std::function<void(XEvent &)> handler; // FileDragAgent's dispatcher

    Atom atom(const char *name) { return XInternAtom(dpy, name, False); }

    void pump()
    {
        while (dpy && XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (handler)
                handler(ev);
        }
    }

    void flush()
    {
        XFlush(dpy);
        if (XPending(dpy) > 0)
            QMetaObject::invokeMethod(m_notifier, [this] { pump(); }, Qt::QueuedConnection);
    }

    // A real server timestamp (selection ownership and conversions must not use CurrentTime).
    Time serverTime(Window w)
    {
        const Atom prop = atom("DESKFLOWER_TIME");
        XChangeProperty(dpy, w, prop, XA_STRING, 8, PropModeAppend, nullptr, 0);
        XEvent ev;
        XWindowEvent(dpy, w, PropertyChangeMask, &ev);
        return ev.xproperty.time;
    }

    void sendClient(Window to, Atom type, long l0, long l1, long l2, long l3, long l4)
    {
        XEvent ev = {};
        ev.xclient.type = ClientMessage;
        ev.xclient.display = dpy;
        ev.xclient.window = to;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = l0;
        ev.xclient.data.l[1] = l1;
        ev.xclient.data.l[2] = l2;
        ev.xclient.data.l[3] = l3;
        ev.xclient.data.l[4] = l4;
        XSendEvent(dpy, to, False, NoEventMask, &ev);
    }

    // The XDND version a window takes (its XdndAware), 0 if none.
    long xdndVersion(Window w)
    {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char *data = nullptr;
        long version = 0;
        if (XGetWindowProperty(dpy, w, atom("XdndAware"), 0, 1, False, AnyPropertyType, &type, &format, &count, &after,
                               &data)
                == Success
            && data && count == 1 && format == 32)
            version = *reinterpret_cast<long *>(data);
        if (data)
            XFree(data);
        return version;
    }

    // The deepest XDND-aware window under (x, y), skipping `skip` (our own windows).
    Window targetAt(int x, int y, long *version, const QList<Window> &skip)
    {
        Window w = root;
        for (int depth = 0; depth < 32; ++depth) {
            Window child = 0;
            int tx = 0, ty = 0;
            if (!XTranslateCoordinates(dpy, root, w, x, y, &tx, &ty, &child) || !child || skip.contains(child))
                break;
            w = child;
            if (const long v = xdndVersion(w); v >= 3) {
                *version = qMin(v, kXdndVersion);
                return w;
            }
        }
        return 0;
    }

    void pressEscape()
    {
        if (!xtest)
            return;
        const KeyCode code = XKeysymToKeycode(dpy, XK_Escape);
        XTestFakeKeyEvent(dpy, code, True, CurrentTime);
        XTestFakeKeyEvent(dpy, code, False, CurrentTime);
        flush();
    }

private:
    bool open()
    {
        dpy = XOpenDisplay(nullptr);
        if (!dpy)
            return false;
        root = DefaultRootWindow(dpy);
        int event = 0, error = 0, major = 0, minor = 0;
        xtest = XTestQueryExtension(dpy, &event, &error, &major, &minor);
        m_notifier = new QSocketNotifier(ConnectionNumber(dpy), QSocketNotifier::Read);
        QObject::connect(m_notifier, &QSocketNotifier::activated, m_notifier, [this] { pump(); });
        return true;
    }

    QSocketNotifier *m_notifier = nullptr;
};

QStringList pathsFromUriList(const QByteArray &list)
{
    QStringList paths;
    for (const QByteArray &line : list.split('\n')) {
        const QByteArray l = line.trimmed();
        if (l.isEmpty() || l.startsWith('#'))
            continue;
        const QUrl url = QUrl::fromEncoded(l);
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    return paths;
}

QByteArray uriList(const QStringList &paths)
{
    QByteArray out;
    for (const QString &p : paths)
        out += QUrl::fromLocalFile(p).toEncoded() + "\r\n";
    return out;
}

} // namespace

// --- Catching the files of a drag in progress ----------------------------------------------------

struct FileDragAgent::Catcher {
    std::function<void(const QStringList &)> done;
    QStringList result;
    Window window = 0;
    Window source = 0;
    bool requested = false;
    QByteArray incr; // INCR transfer in progress
    bool inIncr = false;
    QTimer timeout;
    QTimer nudge;
    QPoint at;
    int nudges = 0;

    void handle(FileDragAgent *agent, XEvent &ev)
    {
        XLink *x = XLink::get();
        if (ev.type == ClientMessage && ev.xclient.window == window) {
            const Atom type = ev.xclient.message_type;
            if (type == x->atom("XdndEnter")) {
                source = Window(ev.xclient.data.l[0]);
            } else if (type == x->atom("XdndPosition")) {
                source = Window(ev.xclient.data.l[0]);
                // Accept (the drag shows "copy") and read the files while it hovers here.
                x->sendClient(source, x->atom("XdndStatus"), long(window), 1 | 2, 0, 0,
                              long(x->atom("XdndActionCopy")));
                if (!requested) {
                    requested = true;
                    XConvertSelection(x->dpy, x->atom("XdndSelection"), x->atom("text/uri-list"),
                                      x->atom("DESKFLOWER_CATCH"), window, Time(ev.xclient.data.l[3]));
                }
                x->flush();
            } else if (type == x->atom("XdndDrop")) {
                // Released on our window before we were done: decline, nothing is moved.
                x->sendClient(Window(ev.xclient.data.l[0]), x->atom("XdndFinished"), long(window), 0, 0, 0, 0);
                x->flush();
            }
        } else if (ev.type == SelectionNotify && ev.xselection.requestor == window) {
            if (ev.xselection.property == kNone) {
                agent->finishCatch();
                return;
            }
            readProperty(agent, false);
        } else if (ev.type == PropertyNotify && ev.xproperty.window == window && inIncr
                   && ev.xproperty.atom == x->atom("DESKFLOWER_CATCH") && ev.xproperty.state == PropertyNewValue) {
            readProperty(agent, true);
        }
    }

    void readProperty(FileDragAgent *agent, bool chunk)
    {
        XLink *x = XLink::get();
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(x->dpy, window, x->atom("DESKFLOWER_CATCH"), 0, LONG_MAX / 4, True, AnyPropertyType,
                               &type, &format, &count, &after, &data)
            != Success) {
            agent->finishCatch();
            return;
        }
        const QByteArray bytes(reinterpret_cast<const char *>(data), data && format == 8 ? int(count) : 0);
        if (data)
            XFree(data);
        x->flush(); // the deletion tells an INCR owner to send the next chunk
        if (!chunk && type == x->atom("INCR")) {
            inIncr = true;
            return;
        }
        if (chunk) {
            if (!bytes.isEmpty() && incr.size() < 16 * 1024 * 1024) {
                incr += bytes;
                return;
            }
            inIncr = false;
            result = pathsFromUriList(incr);
        } else {
            result = pathsFromUriList(bytes);
        }
        // Got them: end the local drag without a drop (the files go to the other device instead).
        if (!result.isEmpty())
            x->pressEscape();
        agent->finishCatch();
    }
};

// --- Showing a drag here ---------------------------------------------------------------------------

struct FileDragAgent::Hover {
    HoverSpec spec;
    ClipboardBridge::FetchFiles fetch;
    std::function<void(const HoverResult &)> done;

    enum class Phase { Moving, WaitStatus, Fetching, Dropped } phase = Phase::Moving;
    Window source = 0;   // our XDND source window (owns XdndSelection)
    Window target = 0;   // XDND-aware window under the cursor, entered
    long version = 0;
    bool accepted = false;
    bool awaitingStatus = false;
    QPoint lastSent{INT_MIN, INT_MIN};
    Time time = 0;
    QStringList files; // local paths offered once dropped
    bool served = false;
    QTimer timer; // status grace, then the finish deadline
    quint64 id = 0;
    qint64 lastAlive = 0;    // when the service last said it is there (dragAlive)
    qint64 localRelease = 0; // this device's own button seen released (spec.local), 0 while held

    // Nested in FileDragAgent: these reach its members.
    static void handle(FileDragAgent *agent, XEvent &ev);
    static void feedOnce(FileDragAgent *agent);
    static void sendDrop(FileDragAgent *agent);
    // confirmed: the target reported moving the transferred files (see HoverResult).
    static void finish(FileDragAgent *agent, bool dropped, bool confirmed);
};

FileDragAgent::FileDragAgent(QObject *parent) : QObject(parent)
{
    m_feed.setInterval(kFeedIntervalMs);
    connect(&m_feed, &QTimer::timeout, this, &FileDragAgent::feedDrag);
    m_watch.setInterval(250);
    connect(&m_watch, &QTimer::timeout, this, &FileDragAgent::watchDrag);
    m_follow.setInterval(30);
    connect(&m_follow, &QTimer::timeout, this, [this] {
        if (m_badge)
            m_badge->move(QCursor::pos() + QPoint(18, 22));
    });
}

FileDragAgent::~FileDragAgent()
{
    if (XLink *x = XLink::get())
        x->handler = nullptr;
    if (m_hover && m_hover->source && XLink::get())
        XDestroyWindow(XLink::get()->dpy, m_hover->source);
    if (m_catcher && m_catcher->window && XLink::get())
        XDestroyWindow(XLink::get()->dpy, m_catcher->window);
}

bool FileDragAgent::supportsNativeDrag()
{
    // XDND reaches X11 clients only: in a Wayland session (even through XWayland) native windows
    // would never see the drag.
    if (qEnvironmentVariable("XDG_SESSION_TYPE").compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0
        || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        return false;
    return !qEnvironmentVariableIsEmpty("DISPLAY") && XLink::get();
}

static void installDispatcher(FileDragAgent *agent, std::function<void(XEvent &)> dispatch)
{
    Q_UNUSED(agent)
    if (XLink *x = XLink::get())
        x->handler = std::move(dispatch);
}

void FileDragAgent::catchFiles(QPoint at, std::function<void(const QStringList &paths)> done)
{
    XLink *x = XLink::get();
    if (!x || !supportsNativeDrag()) {
        done({}); // no drag of a native Wayland window ever enters an X window
        return;
    }
    if (m_catcher)
        finishCatch(); // the previous one answers "nothing"
    installDispatcher(this, [this](XEvent &ev) {
        if (m_catcher)
            m_catcher->handle(this, ev);
        if (m_hover)
            Hover::handle(this, ev);
    });
    m_catcher = std::make_unique<Catcher>();
    m_catcher->done = std::move(done);
    m_catcher->at = at;

    XSetWindowAttributes attrs = {};
    attrs.override_redirect = True;
    attrs.event_mask = PropertyChangeMask;
    // 3x3 around the hot spot: whatever the drag source looks at, the cursor is on it.
    const Window w = XCreateWindow(x->dpy, x->root, at.x() - 1, at.y() - 1, 3, 3, 0, CopyFromParent, InputOutput,
                                   CopyFromParent, CWOverrideRedirect | CWEventMask, &attrs);
    m_catcher->window = w;
    const long version = kXdndVersion;
    XChangeProperty(x->dpy, w, x->atom("XdndAware"), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&version), 1);
    XMapRaised(x->dpy, w);
    x->flush();

    // A drag source looks for the window under the cursor when the pointer moves: nudge it (one
    // pixel towards the middle of the screen and back) until the drag enters our window.
    m_catcher->nudge.setInterval(60);
    connect(&m_catcher->nudge, &QTimer::timeout, this, [this] {
        XLink *x = XLink::get();
        if (!m_catcher || !x->xtest || m_catcher->requested)
            return;
        const QPoint at = m_catcher->at;
        const int screenW = DisplayWidth(x->dpy, DefaultScreen(x->dpy));
        const int dx = at.x() < screenW / 2 ? 1 : -1;
        const bool out = (m_catcher->nudges++ % 2) == 0;
        XTestFakeMotionEvent(x->dpy, -1, out ? at.x() + dx : at.x(), at.y(), CurrentTime);
        x->flush();
    });
    m_catcher->nudge.start();
    m_catcher->timeout.setSingleShot(true);
    connect(&m_catcher->timeout, &QTimer::timeout, this, &FileDragAgent::finishCatch);
    m_catcher->timeout.start(kCatchTimeoutMs);
}

void FileDragAgent::finishCatch()
{
    if (!m_catcher)
        return;
    std::unique_ptr<Catcher> c = std::move(m_catcher);
    if (XLink *x = XLink::get()) {
        if (c->window)
            XDestroyWindow(x->dpy, c->window);
        // Back where the master left it (the nudge may have ended one pixel off).
        if (x->xtest && c->nudges % 2 == 1)
            XTestFakeMotionEvent(x->dpy, -1, c->at.x(), c->at.y(), CurrentTime);
        x->flush();
    }
    qCInfo(lcXdnd) << "Caught" << c->result.size() << "dragged items";
    if (c->done)
        c->done(c->result);
}

QString FileDragAgent::folderAt(QPoint) const
{
    // No file manager says which folder is under a point: the Downloads folder is the fallback
    // target of a drop without a drag shown here (the service tells the user where they went).
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

bool FileDragAgent::recycle(const QStringList &paths, QString *error)
{
    bool ok = true;
    for (const QString &p : paths) {
        if (QFile::exists(p) && !QFile::moveToTrash(p)) {
            *error = tr("%1 could not be moved to the trash").arg(p);
            ok = false;
        }
    }
    return ok;
}

void FileDragAgent::setBadge(const QString &text)
{
    if (text.isEmpty()) {
        m_follow.stop();
        m_badge.reset();
        return;
    }
    if (!m_badge) {
        m_badge = std::make_unique<QLabel>();
        m_badge->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        m_badge->setAttribute(Qt::WA_ShowWithoutActivating);
        m_badge->setStyleSheet(QStringLiteral(
            "QLabel { background: #263238; color: white; border: 1px solid #4db6ac; border-radius: 6px; padding: 6px 10px; }"));
    }
    m_badge->setText(QStringLiteral("📄 ") + text);
    m_badge->adjustSize();
    m_badge->move(QCursor::pos() + QPoint(18, 22));
    m_badge->show();
    m_follow.start();
}

void FileDragAgent::hover(const HoverSpec &spec, ClipboardBridge::FetchFiles fetch,
                          std::function<void(const HoverResult &)> done)
{
    XLink *x = XLink::get();
    if (!x || !supportsNativeDrag()) {
        HoverResult result;
        result.unavailable = true;
        done(result);
        return;
    }
    if (m_hover)
        Hover::finish(this, false, false); // one drag at a time: the current one is cancelled
    installDispatcher(this, [this](XEvent &ev) {
        if (m_catcher)
            m_catcher->handle(this, ev);
        if (m_hover)
            Hover::handle(this, ev);
    });
    static quint64 s_ids = 0;
    m_hover = std::make_shared<Hover>();
    m_hover->id = ++s_ids;
    m_hover->spec = spec;
    m_hover->fetch = std::move(fetch);
    m_hover->done = std::move(done);
    m_hover->lastAlive = QDateTime::currentMSecsSinceEpoch();

    XSetWindowAttributes attrs = {};
    attrs.event_mask = PropertyChangeMask;
    m_hover->source = XCreateWindow(x->dpy, x->root, -10, -10, 1, 1, 0, 0, InputOnly, CopyFromParent, CWEventMask, &attrs);
    m_hover->time = x->serverTime(m_hover->source);
    XSetSelectionOwner(x->dpy, x->atom("XdndSelection"), m_hover->source, m_hover->time);
    const Atom types[] = {x->atom("text/uri-list"), x->atom("x-special/gnome-copied-files")};
    XChangeProperty(x->dpy, m_hover->source, x->atom("XdndTypeList"), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(types), 2);
    x->flush();
    m_inDrag = true;
    feedDrag();
    m_feed.start();
    m_watch.start();
}

void FileDragAgent::alive()
{
    // A service that says it is there will keep saying it: from now on its silence counts.
    if (m_hover) {
        m_hover->lastAlive = QDateTime::currentMSecsSinceEpoch();
        m_hover->spec.heartbeat = true;
    }
}

void FileDragAgent::serviceLost()
{
    cancelHere("the service is gone");
}

void FileDragAgent::cancelHere(const char *why)
{
    // Only while the cursor still drags: once dropped, the target finishes on its own.
    if (!m_hover || m_hover->phase != Hover::Phase::Moving)
        return;
    qCInfo(lcXdnd) << "Drag here cancelled:" << why;
    Hover::finish(this, false, false);
}

// Escape hatches, so that a drag never stays on screen: Escape, the service silent for too long,
// or (this device being the master) its own button released with no word from the service.
void FileDragAgent::watchDrag()
{
    XLink *x = XLink::get();
    if (!m_hover || !x || m_hover->phase != Hover::Phase::Moving)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    char keys[32] = {};
    XQueryKeymap(x->dpy, keys);
    const KeyCode escape = XKeysymToKeycode(x->dpy, XK_Escape);
    if (escape && (keys[escape / 8] & (1 << (escape % 8)))) {
        cancelHere("Escape");
        return;
    }
    if (m_hover->spec.heartbeat && now - m_hover->lastAlive > kAliveTimeoutMs) {
        cancelHere("no word from the service");
        return;
    }
    if (m_hover->spec.local) {
        Window rootReturn = 0, child = 0;
        int rx = 0, ry = 0, wx = 0, wy = 0;
        unsigned int mask = 0;
        if (XQueryPointer(x->dpy, x->root, &rootReturn, &child, &rx, &ry, &wx, &wy, &mask)) {
            if (mask & Button1Mask)
                m_hover->localRelease = 0;
            else if (!m_hover->localRelease)
                m_hover->localRelease = now;
            else if (now - m_hover->localRelease > kLocalReleaseGraceMs)
                cancelHere("the button was released here and the service did not end the drag");
        }
    }
}

// Follows the cursor (moved by the master): enters, leaves and updates XDND targets.
void FileDragAgent::feedDrag()
{
    XLink *x = XLink::get();
    if (!m_hover || !x || m_hover->phase != Hover::Phase::Moving)
        return;
    Window rootReturn = 0, child = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(x->dpy, x->root, &rootReturn, &child, &rx, &ry, &wx, &wy, &mask))
        return;
    QList<Window> skip{m_hover->source};
    if (m_badge)
        skip.append(Window(m_badge->winId()));
    long version = 0;
    const Window target = x->targetAt(rx, ry, &version, skip);
    Hover &h = *m_hover;
    if (target != h.target) {
        if (h.target)
            x->sendClient(h.target, x->atom("XdndLeave"), long(h.source), 0, 0, 0, 0);
        h.target = target;
        h.version = version;
        h.accepted = false;
        h.awaitingStatus = false;
        h.lastSent = {INT_MIN, INT_MIN};
        if (target) {
            x->sendClient(target, x->atom("XdndEnter"), long(h.source), long(version) << 24,
                          long(x->atom("text/uri-list")), long(x->atom("x-special/gnome-copied-files")), 0);
        }
    }
    // One position at a time: the next once the target answered (or moved on).
    if (h.target && QPoint(rx, ry) != h.lastSent && !h.awaitingStatus) {
        // A move in both cases: the originals here, or the transferred copies of files from another
        // device (only a move the target confirms lets the originals there go).
        const Atom action = x->atom("XdndActionMove");
        x->sendClient(h.target, x->atom("XdndPosition"), long(h.source), 0, long((rx << 16) | (ry & 0xFFFF)),
                      long(h.time), long(action));
        h.lastSent = QPoint(rx, ry);
        h.awaitingStatus = true;
    }
    x->flush();
}

void FileDragAgent::Hover::handle(FileDragAgent *agent, XEvent &ev)
{
    XLink *x = XLink::get();
    Hover &h = *agent->m_hover;
    if (ev.type == ClientMessage && ev.xclient.window == h.source) {
        const Atom type = ev.xclient.message_type;
        if (type == x->atom("XdndStatus") && Window(ev.xclient.data.l[0]) == h.target) {
            h.accepted = ev.xclient.data.l[1] & 1;
            h.awaitingStatus = false;
            if (h.phase == Hover::Phase::WaitStatus)
                agent->runHover();
        } else if (type == x->atom("XdndFinished") && h.phase == Hover::Phase::Dropped) {
            // Version 5 says whether the target took them and what it did; older ones only that it
            // is done (taken if it read the list, but never a confirmed move).
            const bool accepted = h.version < 5 || (ev.xclient.data.l[1] & 1);
            const bool moved = h.version >= 5 && accepted && Atom(ev.xclient.data.l[2]) == x->atom("XdndActionMove");
            finish(agent, accepted || h.served, moved && h.served && !h.spec.own);
        }
    } else if (ev.type == SelectionRequest && ev.xselectionrequest.owner == h.source
               && ev.xselectionrequest.selection == x->atom("XdndSelection")) {
        const XSelectionRequestEvent &req = ev.xselectionrequest;
        XEvent reply = {};
        reply.xselection.type = SelectionNotify;
        reply.xselection.display = x->dpy;
        reply.xselection.requestor = req.requestor;
        reply.xselection.selection = req.selection;
        reply.xselection.target = req.target;
        reply.xselection.time = req.time;
        reply.xselection.property = kNone;
        const Atom property = req.property ? req.property : req.target;
        if (h.phase == Hover::Phase::Dropped && !h.files.isEmpty()) {
            if (req.target == x->atom("TARGETS")) {
                const Atom targets[] = {x->atom("TARGETS"), x->atom("text/uri-list"),
                                        x->atom("x-special/gnome-copied-files")};
                XChangeProperty(x->dpy, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(targets), 3);
                reply.xselection.property = property;
            } else if (req.target == x->atom("text/uri-list")
                       || req.target == x->atom("x-special/gnome-copied-files")) {
                const QByteArray data = req.target == x->atom("text/uri-list")
                                            ? uriList(h.files)
                                            : ClipboardBridge::gnomeCopiedFiles(h.files);
                // One request: lists of up to 2,000 entries stay well within the request size.
                const long maxBytes = (XExtendedMaxRequestSize(x->dpy) ? XExtendedMaxRequestSize(x->dpy)
                                                                         : XMaxRequestSize(x->dpy)) * 4 - 100;
                if (data.size() < maxBytes) {
                    XChangeProperty(x->dpy, req.requestor, property, req.target, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char *>(data.constData()), int(data.size()));
                    reply.xselection.property = property;
                    h.served = true;
                } else {
                    qCWarning(lcXdnd) << "Dropped file list too long for one X request:" << data.size();
                }
            }
        }
        XSendEvent(x->dpy, req.requestor, False, NoEventMask, &reply);
        x->flush();
    }
}

void FileDragAgent::endHover(bool drop)
{
    if (!m_hover)
        return;
    if (!drop) {
        Hover::finish(this, false, false);
        return;
    }
    m_feed.stop();
    Hover::feedOnce(this);
    m_hover->phase = Hover::Phase::WaitStatus;
    m_hover->timer.setSingleShot(true);
    m_hover->timer.disconnect();
    connect(&m_hover->timer, &QTimer::timeout, this, &FileDragAgent::runHover);
    m_hover->timer.start(kStatusGraceMs);
    if (!m_hover->awaitingStatus)
        runHover();
}

void FileDragAgent::Hover::feedOnce(FileDragAgent *agent)
{
    // What the target already answered counts (its status may be waiting to be read), then the
    // last position the master gave goes to it, unless an answer is still awaited.
    XLink::get()->pump();
    if (agent->m_hover)
        agent->feedDrag();
}

// The master dropped: once the target's status is known, transfer (files from another device)
// and drop.
void FileDragAgent::runHover()
{
    if (!m_hover || m_hover->phase != Hover::Phase::WaitStatus)
        return;
    m_hover->timer.stop();
    Hover &h = *m_hover;
    if (!h.target || !h.accepted) {
        Hover::finish(this, false, false); // nothing under the cursor takes files
        return;
    }
    if (h.spec.own) {
        h.files = h.spec.paths;
        Hover::sendDrop(this);
        return;
    }
    h.phase = Hover::Phase::Fetching;
    // The transfer blocks (the service answers once every file arrived): on a thread of its own.
    const QPointer<FileDragAgent> self(this);
    const quint64 id = h.id;
    std::thread([self, id, fetch = h.fetch] {
        QString error;
        const QStringList files = fetch ? fetch(&error) : QStringList();
        QMetaObject::invokeMethod(qApp, [self, id, files, error] {
            if (!self || !self->m_hover || self->m_hover->id != id)
                return;
            if (files.isEmpty()) {
                qCWarning(lcXdnd) << "Dragged files not received:" << error;
                Hover::finish(self, true, false);
                return;
            }
            self->m_hover->files = files;
            Hover::sendDrop(self);
        });
    }).detach();
}

void FileDragAgent::Hover::sendDrop(FileDragAgent *agent)
{
    XLink *x = XLink::get();
    Hover &h = *agent->m_hover;
    h.phase = Phase::Dropped;
    x->sendClient(h.target, x->atom("XdndDrop"), long(h.source), 0, long(h.time), 0, 0);
    x->flush();
    h.timer.disconnect();
    QObject::connect(&h.timer, &QTimer::timeout, agent, [agent] {
        if (agent->m_hover) // no XdndFinished: read means taken, but not a confirmed move
            finish(agent, agent->m_hover->served, false);
    });
    h.timer.start(kFinishTimeoutMs);
}

void FileDragAgent::Hover::finish(FileDragAgent *agent, bool dropped, bool confirmed)
{
    if (!agent->m_hover)
        return;
    std::shared_ptr<Hover> h = std::move(agent->m_hover);
    agent->m_feed.stop();
    agent->m_watch.stop();
    agent->m_inDrag = false;
    if (XLink *x = XLink::get()) {
        if (h->target && h->phase != Phase::Dropped)
            x->sendClient(h->target, x->atom("XdndLeave"), long(h->source), 0, 0, 0, 0);
        if (XGetSelectionOwner(x->dpy, x->atom("XdndSelection")) == h->source)
            XSetSelectionOwner(x->dpy, x->atom("XdndSelection"), kNone, h->time);
        XDestroyWindow(x->dpy, h->source);
        x->flush();
    }
    qCInfo(lcXdnd) << "Drag here ended: dropped" << dropped << "confirmed" << confirmed;
    HoverResult result;
    result.dropped = dropped;
    result.confirmed = confirmed;
    if (auto done = std::exchange(h->done, nullptr))
        done(result);
}
