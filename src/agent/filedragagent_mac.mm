// macOS: dragging files between devices (see filedragagent.h). Built with ARC.
//  - catchFiles: the files of a drag in progress are on the drag pasteboard (no permission needed)
//    if it was written since the left button went down. The local drag then ends without a drop:
//    a window of ours that takes nothing appears under the cursor and the button is released there.
//  - hover: a transparent window under the cursor gets a (posted) mouse down and starts an
//    NSDraggingSession from it: the originals as file URLs, or files from another device as file
//    promises, written (transferred, then moved into place) when the drop target asks. The drag
//    follows the cursor through posted mouse-dragged events and ends when the master says: a
//    release (drop) or a release over our own window, which takes nothing (cancel); or here by
//    itself (cancelled): Escape, the service silent for kAliveTimeoutMs once it said dragAlive, or
//    the connection to the service lost. Promised files written into the drop target's folder are
//    a confirmed move (the originals on the other device may go); the originals of this Mac are
//    moved by the target itself.
// Posting events needs the Accessibility permission for DeskFlower (the agent) too; without it the
// user is told (notice) and nothing is caught or shown.
// NOTE: written without a macOS build environment; see the README.

#include "filedragagent.h"

#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLoggingCategory>
#include <QStandardPaths>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <ApplicationServices/ApplicationServices.h>

#include <Carbon/Carbon.h> // kVK_Escape

#include <mutex>
#include <unistd.h>
#include <utility>

Q_LOGGING_CATEGORY(lcDragAgent, "deskflower.agent.drag")

namespace {

// The session helper's marker (inputbackend_mac.cpp): its event tap ignores these events, which
// are not this computer's user.
constexpr int64_t kInjectedMarker = 0x44464C57;
constexpr CGFloat kWindowSize = 64;
constexpr int kStartTimeoutMs = 1000;    // the posted mouse down reaches our window
constexpr int kPromiseWaitMs = 15000;    // a drop target that took promises starts writing them

CGPoint cursorLocation()
{
    CGEventRef e = CGEventCreate(nullptr);
    const CGPoint p = e ? CGEventGetLocation(e) : CGPointZero;
    if (e)
        CFRelease(e);
    return p;
}

void postMouse(CGEventType type, CGPoint at)
{
    CGEventRef e = CGEventCreateMouseEvent(nullptr, type, at, kCGMouseButtonLeft);
    if (!e)
        return;
    CGEventSetIntegerValueField(e, kCGEventSourceUserData, kInjectedMarker);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

// A square around a point in Quartz global coordinates (top-left origin), as a Cocoa screen
// rectangle (bottom-left origin of the primary screen).
NSRect frameAround(CGPoint p, CGFloat size)
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    const CGFloat primaryHeight = screens.count > 0 ? screens[0].frame.size.height : 0;
    return NSMakeRect(p.x - size / 2, primaryHeight - p.y - size / 2, size, size);
}

// Local paths of the file URLs on a pasteboard.
QStringList filePaths(NSPasteboard *pasteboard)
{
    QStringList paths;
    NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                       options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL *url in urls) {
        NSURL *path = [url filePathURL]; // a file reference URL becomes a path
        if (path.path.length > 0)
            paths << QString::fromNSString(path.path);
    }
    return paths;
}

// The drag pasteboard's change count when the left button last went down: a drag pasteboard
// written since then belongs to a drag in progress (and not to an earlier one).
NSInteger s_dragCountAtPress = -1;
id s_pressMonitor = nil;
id s_localPressMonitor = nil;

// Shared by the file promises of one drag (written on their own queue).
struct PromiseState {
    QStringList names; // top-level files and folders, one promise each
    ClipboardBridge::FetchFiles fetch;
    std::function<void()> onWritten; // after each write (any thread)

    std::mutex mutex;
    bool fetchedOnce = false;
    QStringList fetched;
    QString error;
    int started = 0;
    int finished = 0;
    bool failed = false;

    bool allFinished()
    {
        std::lock_guard lock(mutex);
        return finished >= int(names.size());
    }
    bool succeeded()
    {
        std::lock_guard lock(mutex);
        return !failed && finished >= int(names.size());
    }

    // Writes promise `index` at `url`: everything is transferred at the first request (blocking:
    // the drop target waits), then the item is moved there. nil or the error.
    NSError *write(int index, NSURL *url)
    {
        QString path;
        QString why;
        {
            std::lock_guard lock(mutex);
            ++started;
            if (!fetchedOnce) {
                fetchedOnce = true;
                fetched = fetch ? fetch(&error) : QStringList();
            }
            const QString name = names.value(index);
            for (const QString &f : std::as_const(fetched)) {
                if (QFileInfo(f).fileName() == name)
                    path = f;
            }
            if (path.isEmpty())
                path = fetched.value(index);
            why = error.isEmpty() ? QStringLiteral("not transferred") : error;
        }
        NSError *result = nil;
        if (path.isEmpty()) {
            result = [NSError errorWithDomain:NSCocoaErrorDomain
                                         code:NSFileWriteUnknownError
                                     userInfo:@{NSLocalizedDescriptionKey : why.toNSString()}];
        } else {
            NSFileManager *files = [NSFileManager defaultManager];
            NSURL *from = [NSURL fileURLWithPath:path.toNSString()];
            NSError *moveError = nil;
            if (![files moveItemAtURL:from toURL:url error:&moveError]) {
                NSError *copyError = nil;
                if (![files copyItemAtURL:from toURL:url error:&copyError])
                    result = copyError ? copyError : moveError;
            }
        }
        {
            std::lock_guard lock(mutex);
            ++finished;
            if (result)
                failed = true;
        }
        if (result)
            qCWarning(lcDragAgent) << "A dropped item could not be written:"
                                   << QString::fromNSString(result.localizedDescription);
        if (onWritten)
            onWritten();
        return result;
    }
};

} // namespace

// The transparent window's view: the source of the drag shown here, and a drop target that takes
// nothing (the catcher, and the end of a cancelled drag).
@interface DFDragView : NSView <NSDraggingSource>
@property(nonatomic, copy) void (^onMouseDown)(NSEvent *event);
@property(nonatomic, copy) void (^onEnded)(NSDragOperation operation);
@property(nonatomic) NSDragOperation operations;
@end

@implementation DFDragView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        NSMutableArray<NSPasteboardType> *types =
            [NSMutableArray arrayWithObjects:NSPasteboardTypeFileURL, NSPasteboardTypeURL, NSPasteboardTypeString, nil];
        [types addObjectsFromArray:[NSFilePromiseReceiver readableDraggedTypes]];
        [self registerForDraggedTypes:types];
    }
    return self;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    (void)event;
    return YES;
}

- (void)mouseDown:(NSEvent *)event
{
    if (self.onMouseDown)
        self.onMouseDown(event);
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    (void)session;
    return context == NSDraggingContextOutsideApplication ? self.operations : NSDragOperationNone;
}

- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation
{
    (void)session;
    (void)screenPoint;
    if (self.onEnded)
        self.onEnded(operation);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationNone;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NO;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NO;
}

@end

// Names and writes the promised files (see PromiseState).
@interface DFPromiseDelegate : NSObject <NSFilePromiseProviderDelegate>
- (instancetype)initWithState:(const std::shared_ptr<PromiseState> &)state;
@end

@implementation DFPromiseDelegate {
    std::shared_ptr<PromiseState> _state;
    NSOperationQueue *_queue;
}

- (instancetype)initWithState:(const std::shared_ptr<PromiseState> &)state
{
    self = [super init];
    if (self) {
        _state = state;
        // Off the main thread: the transfer blocks, and the agent must go on talking to the service.
        _queue = [[NSOperationQueue alloc] init];
        _queue.maxConcurrentOperationCount = 1;
        _queue.qualityOfService = NSQualityOfServiceUserInitiated;
    }
    return self;
}

- (NSString *)filePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider fileNameForType:(NSString *)fileType
{
    (void)fileType;
    const int index = [filePromiseProvider.userInfo intValue];
    return _state->names.value(index).toNSString();
}

- (void)filePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider
          writePromiseToURL:(NSURL *)url
          completionHandler:(void (^)(NSError *errorOrNil))completionHandler
{
    const int index = [filePromiseProvider.userInfo intValue];
    const std::shared_ptr<PromiseState> state = _state;
    completionHandler(state->write(index, url));
}

- (NSOperationQueue *)operationQueueForFilePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider
{
    (void)filePromiseProvider;
    return _queue;
}

@end

namespace {

NSPanel *makeWindow(CGPoint at)
{
    NSPanel *window = [[NSPanel alloc] initWithContentRect:frameAround(at, kWindowSize)
                                                 styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
    window.releasedWhenClosed = NO;
    window.level = NSScreenSaverWindowLevel;
    window.opaque = NO;
    // Not fully transparent: a window with nothing drawn would let the mouse through.
    window.backgroundColor = [NSColor colorWithCalibratedWhite:0 alpha:0.01];
    window.hasShadow = NO;
    window.ignoresMouseEvents = NO;
    window.hidesOnDeactivate = NO;
    window.becomesKeyOnlyIfNeeded = YES;
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary
                                | NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
    window.contentView = [[DFDragView alloc] initWithFrame:NSMakeRect(0, 0, kWindowSize, kWindowSize)];
    [window orderFrontRegardless];
    return window;
}

void closeWindow(NSPanel *window)
{
    if (!window)
        return;
    [window orderOut:nil];
    [window close];
}

quint64 s_nextHoverId = 1;

} // namespace

struct FileDragAgent::Hover {
    enum class State { Dragging, Drop, Cancel };
    quint64 id = 0;
    HoverSpec spec;
    ClipboardBridge::FetchFiles fetch;
    std::function<void(const HoverResult &)> done;
    std::function<void(const HoverResult &)> finish; // ends it (called through a copy: it deletes this)
    State requested = State::Dragging;      // asked by the master
    bool started = false;                   // the drag session runs
    bool ended = false;                     // and has ended (dropped: waiting for the promises)
    NSPanel *window = nil;
    DFPromiseDelegate *promises = nil;      // the providers only keep a weak reference
    std::shared_ptr<PromiseState> promiseState;
    CGPoint lastFed = CGPointMake(-1e9, -1e9);
    qint64 lastAlive = 0; // when the service last said it is there (dragAlive)
};

namespace {

FileDragAgent::HoverResult hoverResult(bool dropped, bool confirmed, bool unavailable = false)
{
    FileDragAgent::HoverResult r;
    r.dropped = dropped;
    r.confirmed = confirmed;
    r.unavailable = unavailable;
    return r;
}

} // namespace

struct FileDragAgent::Catcher {
    NSPanel *window = nil;
    QStringList paths;
    std::function<void(const QStringList &)> done;
};

namespace {

// The Accessibility permission, without which posted events are dropped: asked once (the system
// prompt), and each refusal explained.
bool mayPostEvents(FileDragAgent *agent)
{
    if (AXIsProcessTrusted())
        return true;
    static bool asked = false;
    if (!asked) {
        asked = true;
        NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    }
    emit agent->notice(FileDragAgent::tr("To drag files to and from this Mac, allow DeskFlower in System Settings › "
                                         "Privacy & Security › Accessibility"));
    return false;
}

} // namespace

FileDragAgent::FileDragAgent(QObject *parent) : QObject(parent)
{
    m_feed.setInterval(15);
    connect(&m_feed, &QTimer::timeout, this, &FileDragAgent::feedDrag);
    m_watch.setInterval(250);
    connect(&m_watch, &QTimer::timeout, this, &FileDragAgent::watchDrag);

    m_follow.setInterval(30);
    connect(&m_follow, &QTimer::timeout, this, [this] {
        if (m_badge)
            m_badge->move(QCursor::pos() + QPoint(18, 22));
    });

    // Mouse presses elsewhere (no permission needed for mouse events) and in our own windows.
    s_dragCountAtPress = [NSPasteboard pasteboardWithName:NSPasteboardNameDrag].changeCount;
    if (!s_pressMonitor) {
        s_pressMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                                                handler:^(NSEvent *event) {
                                                                    (void)event;
                                                                    s_dragCountAtPress =
                                                                        [NSPasteboard pasteboardWithName:NSPasteboardNameDrag]
                                                                            .changeCount;
                                                                }];
        s_localPressMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                                                   handler:^NSEvent *(NSEvent *event) {
                                                                       s_dragCountAtPress =
                                                                           [NSPasteboard pasteboardWithName:NSPasteboardNameDrag]
                                                                               .changeCount;
                                                                       return event;
                                                                   }];
    }
}

FileDragAgent::~FileDragAgent()
{
    if (m_catcher)
        finishCatch();
    if (m_hover)
        closeWindow(m_hover->window);
    if (s_pressMonitor) {
        [NSEvent removeMonitor:s_pressMonitor];
        s_pressMonitor = nil;
    }
    if (s_localPressMonitor) {
        [NSEvent removeMonitor:s_localPressMonitor];
        s_localPressMonitor = nil;
    }
}

void FileDragAgent::catchFiles(QPoint at, std::function<void(const QStringList &)> done)
{
    if (m_catcher)
        finishCatch(); // an older one answers "nothing"
    m_catcher = std::make_unique<Catcher>();
    m_catcher->done = std::move(done);

    NSPasteboard *drag = [NSPasteboard pasteboardWithName:NSPasteboardNameDrag];
    const bool held = ([NSEvent pressedMouseButtons] & 1) != 0;
    if (!held || drag.changeCount == s_dragCountAtPress) {
        finishCatch(); // no drag in progress (the button held for something else)
        return;
    }
    const QStringList paths = filePaths(drag);
    if (paths.isEmpty() || !mayPostEvents(this)) {
        finishCatch(); // not files (text, a window...), or the drag cannot be ended here
        return;
    }
    m_catcher->paths = paths;

    // End the drag here without a drop: over our window, which takes nothing, the button released.
    Catcher *catcher = m_catcher.get();
    const CGPoint p = CGPointMake(at.x(), at.y());
    catcher->window = makeWindow(p);
    postMouse(kCGEventLeftMouseDragged, CGPointMake(p.x + 1, p.y)); // the drag finds the window
    postMouse(kCGEventLeftMouseDragged, p);
    QTimer::singleShot(60, this, [this, catcher, p] {
        if (m_catcher.get() != catcher)
            return;
        postMouse(kCGEventLeftMouseUp, p);
        QTimer::singleShot(120, this, [this, catcher] {
            if (m_catcher.get() == catcher)
                finishCatch();
        });
    });
}

void FileDragAgent::finishCatch()
{
    std::unique_ptr<Catcher> catcher = std::move(m_catcher);
    if (!catcher)
        return;
    closeWindow(catcher->window);
    if (!catcher->paths.isEmpty())
        qCInfo(lcDragAgent) << "Caught a drag of" << catcher->paths.size() << "items";
    if (catcher->done)
        catcher->done(catcher->paths);
}

bool FileDragAgent::supportsNativeDrag()
{
    return true; // without the Accessibility permission a hover answers "unavailable" (and says why)
}

void FileDragAgent::hover(const HoverSpec &spec, ClipboardBridge::FetchFiles fetch,
                          std::function<void(const HoverResult &)> done)
{
    auto next = std::make_shared<Hover>();
    next->id = s_nextHoverId++;
    next->spec = spec;
    next->fetch = std::move(fetch);
    next->done = std::move(done);
    next->lastAlive = QDateTime::currentMSecsSinceEpoch();
    if (m_inDrag) {
        // One drag at a time: the current one is cancelled, this one starts once it has ended.
        if (m_nextHover && m_nextHover->done)
            std::exchange(m_nextHover->done, nullptr)(HoverResult{});
        m_nextHover = std::move(next);
        endHover(false);
        return;
    }
    if (m_hover && m_hover->done) // replaced before it started: it never ran
        std::exchange(m_hover->done, nullptr)(HoverResult{});
    m_hover = std::move(next);
    QTimer::singleShot(0, this, &FileDragAgent::runHover);
}

void FileDragAgent::alive()
{
    // A service that says it is there will keep saying it: from now on its silence counts.
    for (const auto &h : {m_hover, m_nextHover}) {
        if (h) {
            h->lastAlive = QDateTime::currentMSecsSinceEpoch();
            h->spec.heartbeat = true;
        }
    }
}

void FileDragAgent::serviceLost()
{
    if (m_nextHover && m_nextHover->done)
        std::exchange(m_nextHover->done, nullptr)(HoverResult{});
    m_nextHover.reset();
    cancelHere("the service is gone");
}

void FileDragAgent::cancelHere(const char *why)
{
    if (!m_hover || m_hover->requested != Hover::State::Dragging)
        return;
    qCInfo(lcDragAgent) << "Drag here cancelled:" << why;
    endHover(false);
}

// Escape hatches, so that a drag never stays on screen: Escape, or the service silent for too long.
void FileDragAgent::watchDrag()
{
    if (!m_hover || !m_hover->started || m_hover->ended || m_hover->requested != Hover::State::Dragging)
        return;
    if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, kVK_Escape))
        cancelHere("Escape");
    else if (m_hover->spec.heartbeat && QDateTime::currentMSecsSinceEpoch() - m_hover->lastAlive > kAliveTimeoutMs)
        cancelHere("no word from the service");
}

void FileDragAgent::runHover()
{
    if (!m_hover || m_inDrag)
        return;
    Hover *h = m_hover.get();
    const quint64 id = h->id;
    h->finish = [this, id](const HoverResult &result) {
        if (!m_hover || m_hover->id != id)
            return;
        std::shared_ptr<Hover> ended = std::move(m_hover);
        m_feed.stop();
        m_watch.stop();
        m_inDrag = false;
        closeWindow(ended->window);
        ended->window = nil;
        qCInfo(lcDragAgent) << "Drag here ended:" << (result.dropped ? "dropped" : "not dropped")
                            << "confirmed" << result.confirmed << "unavailable" << result.unavailable;
        const auto done = std::exchange(ended->done, nullptr);
        ended.reset();
        if (done)
            done(result);
        if (m_nextHover) {
            m_hover = std::move(m_nextHover);
            QTimer::singleShot(0, this, &FileDragAgent::runHover);
        }
    };
    const auto finish = [this, id](const HoverResult &result) {
        if (m_hover && m_hover->id == id)
            std::function<void(const HoverResult &)>(m_hover->finish)(result);
    };

    // Top-level items of files from another device, one promise each.
    QStringList names;
    QList<bool> dirs;
    for (const ClipboardBridge::ListedFile &f : std::as_const(h->spec.files)) {
        const QString top = f.path.section(QLatin1Char('/'), 0, 0);
        if (top.isEmpty() || names.contains(top))
            continue;
        names.append(top);
        dirs.append(f.dir || f.path.contains(QLatin1Char('/')));
    }
    const bool usable = h->spec.own ? !h->spec.paths.isEmpty() : !names.isEmpty();
    if (h->requested != Hover::State::Dragging || !usable) {
        m_inDrag = true; // finish() clears it
        finish(hoverResult(false, false));
        return;
    }
    if (!mayPostEvents(this)) {
        // No drag can be shown without the permission: a release saves the files into a folder.
        m_inDrag = true;
        finish(hoverResult(false, false, true));
        return;
    }
    if (!h->spec.own) {
        h->promiseState = std::make_shared<PromiseState>();
        h->promiseState->names = names;
        h->promiseState->fetch = h->fetch;
        h->promiseState->onWritten = [this, id] {
            QMetaObject::invokeMethod(this, [this, id] {
                Hover *h = m_hover.get();
                if (!h || h->id != id || !h->ended || !h->promiseState->allFinished())
                    return;
                // Written into the target's folder: it kept them, a confirmed move.
                const bool written = h->promiseState->succeeded();
                std::function<void(const HoverResult &)>(h->finish)(hoverResult(true, written));
            }, Qt::QueuedConnection);
        };
        h->promises = [[DFPromiseDelegate alloc] initWithState:h->promiseState];
    }

    m_inDrag = true;
    const CGPoint at = cursorLocation();
    h->window = makeWindow(at);
    DFDragView *view = (DFDragView *)h->window.contentView;
    view.operations = h->spec.own ? (NSDragOperationCopy | NSDragOperationMove | NSDragOperationGeneric)
                                  : NSDragOperationCopy;

    const auto begin = [this, id, names, dirs](NSEvent *event) {
        Hover *h = m_hover.get();
        if (!h || h->id != id || h->started)
            return;
        if (h->requested != Hover::State::Dragging) { // ended before it began
            postMouse(kCGEventLeftMouseUp, cursorLocation());
            std::function<void(const HoverResult &)>(h->finish)(hoverResult(false, false));
            return;
        }
        NSMutableArray<NSDraggingItem *> *items = [NSMutableArray array];
        const NSRect frame = NSMakeRect(0, 0, kWindowSize, kWindowSize);
        if (h->spec.own) {
            for (const QString &path : std::as_const(h->spec.paths)) {
                NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
                NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
                [item setDraggingFrame:frame contents:[[NSWorkspace sharedWorkspace] iconForFile:path.toNSString()]];
                [items addObject:item];
            }
        } else {
            for (int i = 0; i < names.size(); ++i) {
                const bool dir = dirs.value(i);
                NSFilePromiseProvider *provider =
                    [[NSFilePromiseProvider alloc] initWithFileType:(dir ? @"public.folder" : @"public.data")
                                                           delegate:h->promises];
                provider.userInfo = @(i);
                NSImage *icon = nil;
                if (dir) {
                    icon = [NSImage imageNamed:NSImageNameFolder];
                } else {
                    UTType *type = [UTType typeWithFilenameExtension:QFileInfo(names.at(i)).suffix().toNSString()];
                    icon = [[NSWorkspace sharedWorkspace] iconForContentType:(type ? type : UTTypeData)];
                }
                NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:provider];
                [item setDraggingFrame:frame contents:icon];
                [items addObject:item];
            }
        }
        DFDragView *view = (DFDragView *)h->window.contentView;
        NSDraggingSession *session = [view beginDraggingSessionWithItems:items event:event source:view];
        session.animatesToStartingPositionsOnCancelOrFail = YES;
        session.draggingFormation = NSDraggingFormationPile;
        h->started = true;
        // Out of the way: the drop target is whatever is under the cursor.
        h->window.ignoresMouseEvents = YES;
        [h->window setFrameOrigin:NSMakePoint(-20000, -20000)];
        h->lastFed = cursorLocation();
        m_feed.start();
        m_watch.start();
        if (h->requested != Hover::State::Dragging)
            endHover(h->requested == Hover::State::Drop); // asked meanwhile
    };
    view.onMouseDown = ^(NSEvent *event) {
        begin(event);
    };
    const auto ended = [this, id](NSDragOperation operation) {
        Hover *h = m_hover.get();
        if (!h || h->id != id || h->ended)
            return;
        h->ended = true;
        m_feed.stop();
        const bool dropped = operation != NSDragOperationNone;
        if (!dropped || h->spec.own || h->promiseState->allFinished()) {
            const bool written = !h->spec.own && dropped && h->promiseState->succeeded();
            std::function<void(const HoverResult &)>(h->finish)(hoverResult(dropped, written));
            return;
        }
        // Dropped: the target writes the promised files now (the transfer runs meanwhile).
        QTimer::singleShot(kPromiseWaitMs, this, [this, id] {
            Hover *h = m_hover.get();
            if (!h || h->id != id)
                return;
            bool started = false;
            {
                std::lock_guard lock(h->promiseState->mutex);
                started = h->promiseState->started > 0;
            }
            if (!started) // a target that took the promises but never asked for them
                std::function<void(const HoverResult &)>(h->finish)(hoverResult(true, false));
        });
    };
    view.onEnded = ^(NSDragOperation operation) {
        ended(operation);
    };

    // The posted press lands on our window (on top, under the cursor) and starts the drag.
    QTimer::singleShot(30, this, [this, id] {
        if (m_hover && m_hover->id == id && !m_hover->started)
            postMouse(kCGEventLeftMouseDown, cursorLocation());
    });
    QTimer::singleShot(kStartTimeoutMs, this, [this, id] {
        Hover *h = m_hover.get();
        if (!h || h->id != id || h->started)
            return;
        qCWarning(lcDragAgent) << "The drag could not start here";
        postMouse(kCGEventLeftMouseUp, cursorLocation());
        std::function<void(const HoverResult &)>(h->finish)(hoverResult(false, false));
    });
}

void FileDragAgent::endHover(bool drop)
{
    Hover *h = m_hover.get();
    if (!h)
        return;
    h->requested = drop ? Hover::State::Drop : Hover::State::Cancel;
    if (!h->started || h->ended)
        return; // begin() or the start timeout takes it from there
    const CGPoint p = cursorLocation();
    if (!drop) {
        // Over our own window, which takes nothing: the drag fails (and slides back).
        [h->window setFrame:frameAround(p, kWindowSize) display:NO];
        h->window.ignoresMouseEvents = NO;
        postMouse(kCGEventLeftMouseDragged, CGPointMake(p.x + 1, p.y));
        postMouse(kCGEventLeftMouseDragged, p);
    }
    QTimer::singleShot(drop ? 0 : 50, this, [p] { postMouse(kCGEventLeftMouseUp, p); });
}

// The drag session follows mouse-dragged events: the cursor, moved by the master through the
// session helper (as plain moves), is fed to it as such.
void FileDragAgent::feedDrag()
{
    Hover *h = m_hover.get();
    if (!h || !h->started || h->ended)
        return;
    const CGPoint p = cursorLocation();
    if (p.x == h->lastFed.x && p.y == h->lastFed.y)
        return;
    h->lastFed = p;
    postMouse(kCGEventLeftMouseDragged, p);
}

QString FileDragAgent::folderAt(QPoint at) const
{
    // The desktop when no window is under the point; a Finder window only if it is Finder's front
    // one (the one AppleScript can name); anything else is no folder.
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (!list)
        return {};
    NSArray<NSDictionary *> *windows = CFBridgingRelease(list);
    const pid_t self = getpid();
    bool finderFront = true; // no Finder window above it so far
    for (NSDictionary *w in windows) {
        if ([w[(__bridge NSString *)kCGWindowLayer] intValue] != 0)
            continue;
        const pid_t pid = [w[(__bridge NSString *)kCGWindowOwnerPID] intValue];
        if (pid == self)
            continue; // our badge or catcher
        NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        const bool finder = [app.bundleIdentifier isEqualToString:@"com.apple.finder"];
        CGRect bounds;
        NSDictionary *boundsInfo = w[(__bridge NSString *)kCGWindowBounds];
        if (!boundsInfo || !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)boundsInfo, &bounds)
            || !CGRectContainsPoint(bounds, CGPointMake(at.x(), at.y()))) {
            if (finder)
                finderFront = false;
            continue;
        }
        if (!finder || !finderFront)
            return {};
        NSAppleScript *script = [[NSAppleScript alloc]
            initWithSource:@"tell application \"Finder\" to get POSIX path of (target of front window as alias)"];
        NSDictionary *error = nil;
        NSAppleEventDescriptor *result = [script executeAndReturnError:&error];
        return result.stringValue ? QString::fromNSString(result.stringValue) : QString();
    }
    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

bool FileDragAgent::recycle(const QStringList &paths, QString *error)
{
    for (const QString &p : paths) {
        NSError *trashError = nil;
        if (![[NSFileManager defaultManager] trashItemAtURL:[NSURL fileURLWithPath:p.toNSString()]
                                           resultingItemURL:nil
                                                      error:&trashError]) {
            *error = trashError ? QString::fromNSString(trashError.localizedDescription) : tr("unknown error");
            return false;
        }
    }
    return true;
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
