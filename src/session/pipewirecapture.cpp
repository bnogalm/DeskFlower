#include "pipewirecapture.h"

#include <QLoggingCategory>

#include <atomic>
#include <chrono>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

Q_LOGGING_CATEGORY(lcPipeWire, "deskflower.pipewire")

namespace {

struct StreamState {
    PipeWireCapture::Private *owner = nullptr;
    quint32 node = 0;
    QRect geometry;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    spa_video_info_raw format{};
    bool haveFormat = false;
    QImage latest; // guarded by Private::mutex
    quint64 serial = 0;
    // The newest buffer, kept (not given back) while nobody wants frames: copied only when asked
    // for (frame()). Touched with the loop's lock held.
    pw_buffer *held = nullptr;
    // Until when (steady clock, ms) frames are copied as they arrive: someone is watching.
    std::atomic<qint64> wantedUntil{0};
};

// Frames are copied as they arrive for this long after a frame() call that waits (the remote
// view's streamer asks all the time); otherwise only the newest buffer is kept, and copied when
// asked for (a thumbnail).
constexpr qint64 kWantedMs = 1000;

qint64 steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QImage::Format qtFormat(spa_video_format format)
{
    // Byte order in memory: BGRx is 0xXXRRGGBB on a little-endian 32-bit word (Qt's RGB32).
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: return QImage::Format_RGB32;
    case SPA_VIDEO_FORMAT_BGRA: return QImage::Format_ARGB32;
    case SPA_VIDEO_FORMAT_RGBx: return QImage::Format_RGBX8888;
    case SPA_VIDEO_FORMAT_RGBA: return QImage::Format_RGBA8888;
    default: return QImage::Format_Invalid;
    }
}

std::atomic<PipeWireCapture *> s_instance{nullptr};
std::unique_ptr<PipeWireCapture> s_owned;

} // namespace

struct PipeWireCapture::Private {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    std::vector<std::unique_ptr<StreamState>> streams;
    std::mutex mutex;
    std::condition_variable changed;

    static void onParamChanged(void *data, uint32_t id, const spa_pod *param)
    {
        auto *s = static_cast<StreamState *>(data);
        if (!param || id != SPA_PARAM_Format)
            return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video
            || mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
            return;
        if (spa_format_video_raw_parse(param, &s->format) < 0)
            return;
        s->haveFormat = true;
        qCInfo(lcPipeWire) << "Stream" << s->node << "format" << int(s->format.format) << s->format.size.width << "x"
                           << s->format.size.height;
        // Shared memory buffers only: they are read with the CPU. At least three, as one may be
        // kept here (StreamState::held) while the compositor fills the others.
        uint8_t buffer[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
            SPA_POD_CHOICE_RANGE_Int(4, 3, 16), SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        pw_stream_update_params(s->stream, params, 1);
    }

    // Copies `buffer` into the stream's latest frame (the loop's lock held).
    static void take(StreamState *s, pw_buffer *buffer)
    {
        const spa_data &d0 = buffer->buffer->datas[0];
        const QImage::Format format = s->haveFormat ? qtFormat(spa_video_format(s->format.format)) : QImage::Format_Invalid;
        const int width = int(s->format.size.width), height = int(s->format.size.height);
        if (!d0.data || !d0.chunk || d0.chunk->size == 0 || format == QImage::Format_Invalid || width <= 0 || height <= 0)
            return;
        const int stride = d0.chunk->stride > 0 ? d0.chunk->stride : width * 4;
        const quint64 needed = quint64(d0.chunk->offset) + quint64(stride) * quint64(height);
        if (needed > d0.maxsize)
            return;
        const QImage view(static_cast<const uchar *>(d0.data) + d0.chunk->offset, width, height, stride, format);
        QImage copy = view.copy();
        {
            std::lock_guard lock(s->owner->mutex);
            s->latest = std::move(copy);
            ++s->serial;
        }
        s->owner->changed.notify_all();
    }

    static void onProcess(void *data)
    {
        auto *s = static_cast<StreamState *>(data);
        // Only the newest buffer matters.
        pw_buffer *newest = nullptr;
        while (pw_buffer *next = pw_stream_dequeue_buffer(s->stream)) {
            if (newest)
                pw_stream_queue_buffer(s->stream, newest);
            newest = next;
        }
        if (!newest)
            return;
        if (s->held) {
            pw_stream_queue_buffer(s->stream, s->held);
            s->held = nullptr;
        }
        // Nobody watching: no copy (a monitor at 60 frames a second would cost hundreds of MB/s),
        // the buffer is kept for the next frame() instead.
        if (s->wantedUntil.load() < steadyMs()) {
            s->held = newest;
            return;
        }
        take(s, newest);
        pw_stream_queue_buffer(s->stream, newest);
    }

    static void onRemoveBuffer(void *data, pw_buffer *buffer)
    {
        auto *s = static_cast<StreamState *>(data);
        if (s->held == buffer)
            s->held = nullptr;
    }
};

namespace {
const pw_stream_events kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.param_changed = &PipeWireCapture::Private::onParamChanged;
    events.process = &PipeWireCapture::Private::onProcess;
    events.remove_buffer = &PipeWireCapture::Private::onRemoveBuffer;
    return events;
}();
} // namespace

PipeWireCapture *PipeWireCapture::instance()
{
    return s_instance.load();
}

void PipeWireCapture::setInstance(std::unique_ptr<PipeWireCapture> capture)
{
    s_instance.store(capture.get());
    s_owned = std::move(capture); // the previous one (if any) goes once nobody can find it
}

std::unique_ptr<PipeWireCapture> PipeWireCapture::connect(int fd, const QList<Source> &sources)
{
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    Q_UNUSED(initialized)

    std::unique_ptr<PipeWireCapture> capture(new PipeWireCapture);
    capture->d = std::make_unique<Private>();
    Private *d = capture->d.get();
    d->loop = pw_thread_loop_new("deskflower-pipewire", nullptr);
    if (!d->loop)
        return nullptr;
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (!d->context || pw_thread_loop_start(d->loop) < 0)
        return nullptr;

    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect_fd(d->context, fd, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        qCWarning(lcPipeWire) << "Cannot connect to the PipeWire remote of the screen cast";
        return nullptr;
    }
    for (const Source &src : sources) {
        auto s = std::make_unique<StreamState>();
        s->owner = d;
        s->node = src.node;
        s->geometry = src.geometry;
        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                 PW_KEY_MEDIA_ROLE, "Screen", nullptr);
        s->stream = pw_stream_new(d->core, "deskflower-screen", props);
        if (!s->stream)
            continue;
        pw_stream_add_listener(s->stream, &s->listener, &kStreamEvents, s.get());
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_rectangle defSize = SPA_RECTANGLE(1920, 1080), minSize = SPA_RECTANGLE(1, 1),
                      maxSize = SPA_RECTANGLE(16384, 16384);
        spa_fraction defRate = SPA_FRACTION(30, 1), minRate = SPA_FRACTION(0, 1), maxRate = SPA_FRACTION(240, 1);
        const spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                   SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate)));
        if (pw_stream_connect(s->stream, PW_DIRECTION_INPUT, src.node,
                              pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1)
            < 0) {
            qCWarning(lcPipeWire) << "Cannot connect to the screen stream" << src.node;
            pw_stream_destroy(s->stream);
            continue;
        }
        d->streams.push_back(std::move(s));
    }
    pw_thread_loop_unlock(d->loop);
    if (d->streams.empty())
        return nullptr;
    return capture;
}

PipeWireCapture::~PipeWireCapture()
{
    if (!d || !d->loop)
        return;
    pw_thread_loop_stop(d->loop);
    for (auto &s : d->streams) {
        if (s->stream)
            pw_stream_destroy(s->stream);
    }
    d->streams.clear();
    if (d->core)
        pw_core_disconnect(d->core);
    if (d->context)
        pw_context_destroy(d->context);
    pw_thread_loop_destroy(d->loop);
}

QList<QRect> PipeWireCapture::screens() const
{
    QList<QRect> out;
    for (const auto &s : d->streams)
        out.append(s->geometry);
    return out;
}

bool PipeWireCapture::frame(const QRect &screen, QImage *image, quint64 *serial, int waitMs)
{
    StreamState *chosen = nullptr;
    for (const auto &s : d->streams) {
        if (screen.isEmpty() || s->geometry == screen) {
            chosen = s.get();
            break;
        }
    }
    if (!chosen && d->streams.size() == 1)
        chosen = d->streams.front().get(); // a portal that gave no geometry: its only monitor
    if (!chosen)
        return false;
    // A caller that waits (the remote view's streamer) has the frames copied as they come for a
    // moment; any caller gets the one kept meanwhile, copied now.
    if (waitMs > 0)
        chosen->wantedUntil.store(steadyMs() + kWantedMs);
    pw_thread_loop_lock(d->loop);
    if (chosen->held) {
        Private::take(chosen, chosen->held);
        pw_stream_queue_buffer(chosen->stream, chosen->held);
        chosen->held = nullptr;
    }
    pw_thread_loop_unlock(d->loop);
    std::unique_lock lock(d->mutex);
    d->changed.wait_for(lock, std::chrono::milliseconds(qMax(0, waitMs)),
                        [&] { return chosen->serial > *serial && !chosen->latest.isNull(); });
    if (chosen->serial <= *serial || chosen->latest.isNull())
        return false;
    *image = chosen->latest;
    *serial = chosen->serial;
    return true;
}
