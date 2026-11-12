// macOS screen capture with ScreenCaptureKit, built with ARC.
//  - ScreenGrabber (remote view): an SCStream of the display, frames received on a queue of their
//    own and handed to the streamer's worker thread (macOS 12.3+; older systems get nullptr and the
//    streamer grabs through QScreen on the GUI thread).
//  - captureScreenImage (thumbnails): one SCScreenshotManager image (macOS 14+).
// Both need the Screen Recording permission (the system asks the first time; InputBackend reports
// it missing). The cursor is left out: the viewer draws the remote one itself.
// NOTE: written without a macOS build environment; see docs/DESIGN.md.

#include "screengrabber.h"

#include <QLoggingCategory>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <ApplicationServices/ApplicationServices.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

Q_LOGGING_CATEGORY(lcMacCapture, "deskflower.capture.mac")

namespace {

constexpr int64_t kStartTimeoutNs = 5 * int64_t(NSEC_PER_SEC);

// The display whose bounds (global points, as QScreen and the helper's screens) are `screen`;
// the main display for an empty rectangle or one that matches nothing.
CGDirectDisplayID displayFor(const QRect &screen)
{
    if (screen.isEmpty())
        return CGMainDisplayID();
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(32, ids, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; ++i) {
            const CGRect b = CGDisplayBounds(ids[i]);
            if (QRect(int(b.origin.x), int(b.origin.y), int(b.size.width), int(b.size.height)) == screen)
                return ids[i];
        }
    }
    return CGMainDisplayID();
}

// The display's size in pixels (its bounds are in points).
void pixelSize(CGDirectDisplayID id, size_t *width, size_t *height)
{
    *width = CGDisplayPixelsWide(id);
    *height = CGDisplayPixelsHigh(id);
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
        *width = CGDisplayModeGetPixelWidth(mode);
        *height = CGDisplayModeGetPixelHeight(mode);
        CGDisplayModeRelease(mode);
    }
}

// The shareable SCDisplay of a display (blocking: called off the main thread, or with a short
// wait), nil if not allowed (Screen Recording permission) or not found.
API_AVAILABLE(macos(12.3))
SCDisplay *shareableDisplay(CGDirectDisplayID id)
{
    __block SCDisplay *found = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
        if (error)
            qCWarning(lcMacCapture) << "No shareable content:" << QString::fromNSString(error.localizedDescription);
        for (SCDisplay *display in content.displays) {
            if (display.displayID == id)
                found = display;
        }
        dispatch_semaphore_signal(done);
    }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kStartTimeoutNs)) != 0)
        return nil;
    return found;
}

// Frames from the stream's queue to the grabbing thread.
struct FrameStore {
    std::mutex mutex;
    std::condition_variable changed;
    QImage latest;
    bool fresh = false;  // not handed out yet
    bool failed = false; // the stream stopped
};

} // namespace

API_AVAILABLE(macos(12.3))
@interface DFStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithStore:(const std::shared_ptr<FrameStore> &)store;
@end

@implementation DFStreamOutput {
    std::shared_ptr<FrameStore> _store;
}

- (instancetype)initWithStore:(const std::shared_ptr<FrameStore> &)store
{
    self = [super init];
    if (self)
        _store = store;
    return self;
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer))
        return;
    // Only complete frames carry an image; an idle one means nothing changed.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
        NSNumber *status = info[SCStreamFrameInfoStatus];
        if (status && status.integerValue != SCFrameStatusComplete)
            return;
    }
    CVImageBufferRef buffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!buffer)
        return;
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const auto *base = static_cast<const uchar *>(CVPixelBufferGetBaseAddress(buffer));
    QImage image;
    if (base) {
        // 32BGRA in memory is Qt's RGB32 on a little-endian machine; copied before the unlock.
        image = QImage(base, int(CVPixelBufferGetWidth(buffer)), int(CVPixelBufferGetHeight(buffer)),
                       qsizetype(CVPixelBufferGetBytesPerRow(buffer)), QImage::Format_RGB32)
                    .copy();
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    if (image.isNull())
        return;
    {
        std::lock_guard lock(_store->mutex);
        _store->latest = std::move(image);
        _store->fresh = true;
    }
    _store->changed.notify_all();
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    qCWarning(lcMacCapture) << "Screen capture stopped:" << QString::fromNSString(error.localizedDescription);
    {
        std::lock_guard lock(_store->mutex);
        _store->failed = true;
    }
    _store->changed.notify_all();
}

@end

namespace {

class API_AVAILABLE(macos(12.3)) MacScreenGrabber final : public ScreenGrabber
{
public:
    MacScreenGrabber() : m_queue(dispatch_queue_create("deskflower.capture", DISPATCH_QUEUE_SERIAL)) {}
    ~MacScreenGrabber() override { stop(); }

    Result grab(QImage *image, int waitMs) override
    {
        if (!m_stream && !start())
            return Result::Failed;
        std::unique_lock lock(m_store->mutex);
        m_store->changed.wait_for(lock, std::chrono::milliseconds(waitMs),
                                  [this] { return m_store->fresh || m_store->failed; });
        if (m_store->failed) {
            lock.unlock();
            stop(); // started again at the next grab
            return Result::Failed;
        }
        if (!m_store->fresh)
            return Result::Unchanged; // (no image at all yet: the streamer retries)
        m_store->fresh = false;
        *image = m_store->latest;
        return Result::Changed;
    }

    void reset() override
    {
        std::lock_guard lock(m_store->mutex);
        m_store->fresh = !m_store->latest.isNull(); // the next grab hands the whole image again
    }

    void setScreen(const QRect &screen) override
    {
        if (screen == m_screen && m_stream)
            return;
        m_screen = screen;
        stop(); // the next grab starts on that display
    }

private:
    bool start()
    {
        m_store = std::make_shared<FrameStore>();
        const CGDirectDisplayID id = displayFor(m_screen);
        SCDisplay *display = shareableDisplay(id);
        if (!display)
            return false;
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        size_t width = 0, height = 0;
        pixelSize(id, &width, &height);
        config.width = width;
        config.height = height;
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, 30);
        config.showsCursor = NO;
        config.queueDepth = 3;
        DFStreamOutput *output = [[DFStreamOutput alloc] initWithStore:m_store];
        SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:output];
        NSError *error = nil;
        if (![stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:m_queue error:&error]) {
            qCWarning(lcMacCapture) << "Cannot receive the screen:" << QString::fromNSString(error.localizedDescription);
            return false;
        }
        __block NSError *startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [stream startCaptureWithCompletionHandler:^(NSError *e) {
            startError = e;
            dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, kStartTimeoutNs)) != 0 || startError) {
            qCWarning(lcMacCapture) << "Cannot start the screen capture:"
                                    << (startError ? QString::fromNSString(startError.localizedDescription)
                                                   : QStringLiteral("no answer"));
            [stream stopCaptureWithCompletionHandler:^(NSError *) {}];
            return false;
        }
        m_stream = stream;
        m_output = output;
        return true;
    }

    void stop()
    {
        if (!m_stream)
            return;
        dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
        [m_stream stopCaptureWithCompletionHandler:^(NSError *) {
            dispatch_semaphore_signal(stopped);
        }];
        dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 2 * int64_t(NSEC_PER_SEC)));
        m_stream = nil;
        m_output = nil;
    }

    QRect m_screen;
    dispatch_queue_t m_queue;
    SCStream *m_stream = nil;
    DFStreamOutput *m_output = nil; // the stream only keeps a weak reference to its delegate
    std::shared_ptr<FrameStore> m_store = std::make_shared<FrameStore>();
};

} // namespace

std::unique_ptr<ScreenGrabber> ScreenGrabber::create()
{
    if (@available(macOS 12.3, *))
        return std::make_unique<MacScreenGrabber>();
    return nullptr;
}

QImage captureScreenImage(const QRect &screen)
{
    if (@available(macOS 14.0, *)) {
        const CGDirectDisplayID id = displayFor(screen);
        SCDisplay *display = shareableDisplay(id);
        if (!display)
            return {};
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        size_t width = 0, height = 0;
        pixelSize(id, &width, &height);
        config.width = width;
        config.height = height;
        config.showsCursor = NO;
        __block CGImageRef shot = nullptr;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:config
                                  completionHandler:^(CGImageRef image, NSError *error) {
                                      if (image)
                                          shot = CGImageRetain(image);
                                      else if (error)
                                          qCWarning(lcMacCapture) << "No screenshot:"
                                                                  << QString::fromNSString(error.localizedDescription);
                                      dispatch_semaphore_signal(done);
                                  }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kStartTimeoutNs)) != 0 || !shot)
            return {}; // (a late image is released by nobody: rare, and small)
        const int w = int(CGImageGetWidth(shot)), h = int(CGImageGetHeight(shot));
        QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context =
            CGBitmapContextCreate(image.bits(), size_t(w), size_t(h), 8, size_t(image.bytesPerLine()), space,
                                  CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Host);
        if (context) {
            CGContextDrawImage(context, CGRectMake(0, 0, w, h), shot);
            CGContextRelease(context);
        }
        CGColorSpaceRelease(space);
        CGImageRelease(shot);
        return context ? image : QImage();
    }
    return {};
}
