#include "internal.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#if WL2_HAVE_QUICKJS

namespace wl2_gstreamer {

// --- Media membus bridges ---------------------------------------------------

#if WL2_GSTREAMER_HAVE_APP

// Find a named appsink/appsrc element in a pipeline. Returns a new ref (owned by
// the caller) or null with a pending error.
GstElement* find_app_element(JSContext* ctx, PipelineBox* box, const char* operation,
    const std::string& name, bool wantSink) {
    GstElement* element = gst_bin_get_by_name(GST_BIN(box->pipeline), name.c_str());
    if (!element) {
        throw_gst_error(ctx, "gstreamer_element_not_found", operation,
            "No element named '" + name + "' in the pipeline");
        return nullptr;
    }
    const bool isSink = GST_IS_APP_SINK(element);
    const bool isSrc = GST_IS_APP_SRC(element);
    if ((wantSink && !isSink) || (!wantSink && !isSrc)) {
        gst_object_unref(element);
        throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "Element '" + name + "' is not an " + (wantSink ? "appsink" : "appsrc"));
        return nullptr;
    }
    return element;
}

Bridge* find_bridge(PipelineBox* box, const std::string& kind, const std::string& name) {
    for (auto& bridge : box->bridges) {
        if (bridge->kind == kind && (name.empty() || bridge->name == name)) {
            return bridge.get();
        }
    }
    return nullptr;
}

bool has_bridge(PipelineBox* box, const std::string& name) {
    for (auto& bridge : box->bridges) {
        if (bridge->name == name) {
            return true;
        }
    }
    return false;
}

void copy_strided(char* dst, size_t dstStride, const uint8_t* src, size_t srcStride,
    size_t rows, size_t rowBytes) {
    for (size_t y = 0; y < rows; ++y) {
        std::memcpy(dst + y * dstStride, src + y * srcStride, rowBytes);
    }
}

void store_caps_once(std::mutex& mutex, std::string& target, GstCaps* caps) {
    if (!caps) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (target.empty()) {
        gchar* text = gst_caps_to_string(caps);
        if (text) {
            target = text;
            g_free(text);
        }
    }
}

std::string caps_to_string(GstCaps* caps) {
    if (!caps) {
        return {};
    }
    gchar* text = gst_caps_to_string(caps);
    std::string out = text ? text : "";
    if (text) {
        g_free(text);
    }
    return out;
}

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += ' ';
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

std::string packet_metadata_json(const std::string& caps, int64_t pts, int64_t dts, int64_t duration,
    unsigned flags, bool discontinuity) {
    std::ostringstream out;
    out << "{\"schema\":1,\"timeBase\":\"1/1000000000\",\"caps\":\"" << json_escape(caps)
        << "\",\"pts\":" << pts << ",\"dts\":" << dts << ",\"duration\":" << duration
        << ",\"flags\":" << flags << ",\"discontinuity\":" << (discontinuity ? "true" : "false") << "}";
    return out.str();
}

wl2::PacketKind packet_kind_from_caps(GstCaps* caps) {
    if (!caps || gst_caps_get_size(caps) == 0) {
        return wl2::PacketKind::Data;
    }
    const char* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    if (!name) {
        return wl2::PacketKind::Data;
    }
    std::string media = name;
    if (media.rfind("video/", 0) == 0) return wl2::PacketKind::Video;
    if (media.rfind("audio/", 0) == 0) return wl2::PacketKind::Audio;
    return wl2::PacketKind::Data;
}

wl2::PacketKind packet_kind_from_name(const std::string& name) {
    if (name == "video") return wl2::PacketKind::Video;
    if (name == "audio") return wl2::PacketKind::Audio;
    return wl2::PacketKind::Data;
}

std::string js_bytes(JSContext* ctx, JSValueConst value) {
    if (JS_IsString(value)) {
        size_t len = 0;
        const char* text = JS_ToCStringLen(ctx, &len, value);
        if (!text) {
            return {};
        }
        std::string out(text, len);
        JS_FreeCString(ctx, text);
        return out;
    }

    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, value);
    if (bytes) {
        return std::string(reinterpret_cast<const char*>(bytes), byteLength);
    }
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);

    size_t byteOffset = 0;
    size_t viewLength = 0;
    size_t bytesPerElement = 0;
    JSValue arrayBuffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &viewLength, &bytesPerElement);
    if (JS_IsException(arrayBuffer)) {
        ex = JS_GetException(ctx);
        JS_FreeValue(ctx, ex);
        return {};
    }
    bytes = JS_GetArrayBuffer(ctx, &byteLength, arrayBuffer);
    std::string out;
    if (bytes && byteOffset + viewLength <= byteLength) {
        out.assign(reinterpret_cast<const char*>(bytes) + byteOffset, viewLength);
    }
    JS_FreeValue(ctx, arrayBuffer);
    return out;
}

// ---- Video sink: appsink -> VideoBuffer ----

struct VideoSinkBridge : Bridge {
    GstAppSink* sink = nullptr;
    wl2::VideoBuffer buffer;
    int64_t width = 0;
    int64_t height = 0;
    wl2::VideoPixelFormat format = wl2::VideoPixelFormat::Rgba32;
    std::atomic<int64_t> frames{0};
    std::atomic<int64_t> dropped{0};
    std::mutex capsMutex;
    std::string negotiatedCaps;
    // Most recent sample, retained for snapshot(). Guarded by sampleMutex because
    // it is written on the streaming thread and read on the JS thread.
    std::mutex sampleMutex;
    GstSample* lastSample = nullptr;

    ~VideoSinkBridge() override {
        if (sink) {
            gst_object_unref(sink);
            sink = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(sampleMutex);
            if (lastSample) {
                gst_sample_unref(lastSample);
                lastSample = nullptr;
            }
        }
        buffer.close();
    }

    GstSample* acquireLastSample() override {
        std::lock_guard<std::mutex> lock(sampleMutex);
        return lastSample ? gst_sample_ref(lastSample) : nullptr;
    }

#if WL2_HAVE_QUICKJS
    JSValue stats(JSContext* ctx) override {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind.c_str()));
        JS_SetPropertyStr(ctx, obj, "videoBufferName", JS_NewString(ctx, bufferName.c_str()));
        JS_SetPropertyStr(ctx, obj, "frames", JS_NewInt64(ctx, frames.load()));
        JS_SetPropertyStr(ctx, obj, "dropped", JS_NewInt64(ctx, dropped.load()));
        JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, buffer.sequence()));
        {
            std::lock_guard<std::mutex> lock(capsMutex);
            JS_SetPropertyStr(ctx, obj, "negotiatedCaps", JS_NewString(ctx, negotiatedCaps.c_str()));
        }
        return obj;
    }
#endif
};

GstFlowReturn video_sink_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* bridge = static_cast<VideoSinkBridge*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }
    GstCaps* caps = gst_sample_get_caps(sample);
    store_caps_once(bridge->capsMutex, bridge->negotiatedCaps, caps);

    // Reject frames whose negotiated caps do not match the ring exactly, rather
    // than copying misaligned data (see the plan's "avoid hidden conversions").
    bool ok = false;
    if (caps && gst_caps_get_size(caps) > 0) {
        GstStructure* s = gst_caps_get_structure(caps, 0);
        gint w = 0;
        gint h = 0;
        const char* fmt = gst_structure_get_string(s, "format");
        gst_structure_get_int(s, "width", &w);
        gst_structure_get_int(s, "height", &h);
        auto pixel = fmt ? gst_format_to_pixel(fmt) : std::nullopt;
        ok = (w == bridge->width && h == bridge->height && pixel && *pixel == bridge->format);
    }
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (ok && buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        int64_t slot = bridge->buffer.pointer(0);
        auto view = bridge->buffer.frame(slot);
        if (view && view.value().data && bridge->height > 0) {
            const size_t rows = static_cast<size_t>(bridge->height);
            const size_t dstStride = static_cast<size_t>(view.value().scanWidth);
            const size_t srcStride = map.size / rows;
            const size_t rowBytes = std::min(dstStride, srcStride);
            copy_strided(view.value().data, dstStride, map.data, srcStride, rows, rowBytes);
            bridge->buffer.next(1);
            bridge->frames.fetch_add(1);
        } else {
            bridge->dropped.fetch_add(1);
        }
        gst_buffer_unmap(buf, &map);
    } else {
        bridge->dropped.fetch_add(1);
    }
    // Retain the latest sample for snapshot(); replace any previously held one.
    {
        std::lock_guard<std::mutex> lock(bridge->sampleMutex);
        if (bridge->lastSample) {
            gst_sample_unref(bridge->lastSample);
        }
        bridge->lastSample = gst_sample_ref(sample);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ---- Audio sink: appsink -> AudioBuffer ----

struct AudioSinkBridge : Bridge {
    GstAppSink* sink = nullptr;
    wl2::AudioBuffer buffer;
    int64_t channels = 0;
    int64_t sampleRate = 0;
    wl2::AudioSampleFormat format = wl2::AudioSampleFormat::S16Le;
    std::atomic<int64_t> buffersWritten{0};
    std::atomic<int64_t> samples{0};
    std::atomic<int64_t> dropped{0};
    std::mutex capsMutex;
    std::string negotiatedCaps;

    ~AudioSinkBridge() override {
        if (sink) {
            gst_object_unref(sink);
            sink = nullptr;
        }
        buffer.close();
    }

#if WL2_HAVE_QUICKJS
    JSValue stats(JSContext* ctx) override {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind.c_str()));
        JS_SetPropertyStr(ctx, obj, "audioBufferName", JS_NewString(ctx, bufferName.c_str()));
        JS_SetPropertyStr(ctx, obj, "buffers", JS_NewInt64(ctx, buffersWritten.load()));
        JS_SetPropertyStr(ctx, obj, "samples", JS_NewInt64(ctx, samples.load()));
        JS_SetPropertyStr(ctx, obj, "dropped", JS_NewInt64(ctx, dropped.load()));
        JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, buffer.sequence()));
        {
            std::lock_guard<std::mutex> lock(capsMutex);
            JS_SetPropertyStr(ctx, obj, "negotiatedCaps", JS_NewString(ctx, negotiatedCaps.c_str()));
        }
        return obj;
    }
#endif
};

GstFlowReturn audio_sink_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* bridge = static_cast<AudioSinkBridge*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }
    GstCaps* caps = gst_sample_get_caps(sample);
    store_caps_once(bridge->capsMutex, bridge->negotiatedCaps, caps);

    bool ok = false;
    if (caps && gst_caps_get_size(caps) > 0) {
        GstStructure* s = gst_caps_get_structure(caps, 0);
        gint rate = 0;
        gint ch = 0;
        const char* fmt = gst_structure_get_string(s, "format");
        gst_structure_get_int(s, "rate", &rate);
        gst_structure_get_int(s, "channels", &ch);
        auto sf = fmt ? gst_format_to_sample(fmt) : std::nullopt;
        ok = (rate == bridge->sampleRate && ch == bridge->channels && sf && *sf == bridge->format);
    }
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (ok && buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        int64_t slot = bridge->buffer.pointer(0);
        auto view = bridge->buffer.buffer(slot);
        const size_t cap = static_cast<size_t>(bridge->buffer.bufferSize());
        if (view && view.value().data && cap > 0) {
            const size_t n = std::min(cap, map.size);
            std::memcpy(view.value().data, map.data, n);
            if (n < cap) {
                std::memset(view.value().data + n, 0, cap - n);
            }
            bridge->buffer.next(1);
            bridge->buffersWritten.fetch_add(1);
            const int64_t bytesPerFrame = bridge->channels * bridge->buffer.bytesPerSample();
            if (bytesPerFrame > 0) {
                bridge->samples.fetch_add(static_cast<int64_t>(n) / bytesPerFrame);
            }
        } else {
            bridge->dropped.fetch_add(1);
        }
        gst_buffer_unmap(buf, &map);
    } else {
        bridge->dropped.fetch_add(1);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ---- Video/Audio source bridges: membus ring -> appsrc ----

struct VideoSourceBridge : Bridge {
    GstAppSrc* src = nullptr;
    wl2::VideoBuffer buffer;
    int64_t fps = 30;
    std::atomic<int64_t> pushed{0};
    int64_t frameIndex = 0; // JS-thread only: drives default timestamps.

    ~VideoSourceBridge() override {
        if (src) {
            gst_object_unref(src);
            src = nullptr;
        }
        buffer.close();
    }

#if WL2_HAVE_QUICKJS
    JSValue stats(JSContext* ctx) override {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind.c_str()));
        JS_SetPropertyStr(ctx, obj, "videoBufferName", JS_NewString(ctx, bufferName.c_str()));
        JS_SetPropertyStr(ctx, obj, "pushed", JS_NewInt64(ctx, pushed.load()));
        return obj;
    }
#endif
};

struct AudioSourceBridge : Bridge {
    GstAppSrc* src = nullptr;
    wl2::AudioBuffer buffer;
    std::atomic<int64_t> pushed{0};
    int64_t bufferIndex = 0;

    ~AudioSourceBridge() override {
        if (src) {
            gst_object_unref(src);
            src = nullptr;
        }
        buffer.close();
    }

#if WL2_HAVE_QUICKJS
    JSValue stats(JSContext* ctx) override {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind.c_str()));
        JS_SetPropertyStr(ctx, obj, "audioBufferName", JS_NewString(ctx, bufferName.c_str()));
        JS_SetPropertyStr(ctx, obj, "pushed", JS_NewInt64(ctx, pushed.load()));
        return obj;
    }
#endif
};

#if WL2_HAVE_QUICKJS

// Authorize a shared-memory name through the runtime policy.
bool authorize_shared_memory(JSContext* ctx, const char* operation, const std::string& name) {
    wl2::Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation, "Runtime is unavailable");
        return false;
    }
    if (auto ok = runtime->authorizeSharedMemory(name); !ok) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation,
            "Shared-memory access denied for '" + name + "'");
        return false;
    }
    return true;
}

// Core video-sink attach shared by attachVideoSink and testPattern. On error it
// leaves a pending exception and returns JS_EXCEPTION.
JSValue attach_video_sink_core(JSContext* ctx, PipelineBox* box, const char* operation,
    const std::string& elementName, const std::string& bufferName, bool create,
    int64_t width, int64_t height, int64_t fps, int64_t buffers, wl2::VideoPixelFormat pixel) {
    if (!authorize_shared_memory(ctx, operation, bufferName)) {
        return JS_EXCEPTION;
    }
    if (has_bridge(box, elementName)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "Element '" + elementName + "' is already attached");
    }
    if (create && (width <= 0 || height <= 0)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "video sink with create requires positive width and height");
    }
    if (create && (fps <= 0 || buffers <= 0)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "video sink with create requires positive fps and buffers");
    }
    wl2::Result<wl2::VideoBuffer> opened = create
        ? wl2::VideoBuffer::create(bufferName, width, height, pixel, fps, buffers)
        : wl2::VideoBuffer::openExisting(bufferName);
    if (!create && opened) {
        width = opened.value().width();
        height = opened.value().height();
        fps = opened.value().fps();
        buffers = opened.value().buffers();
        pixel = opened.value().format().value_or(pixel);
        opened.value().close();
        opened = wl2::VideoBuffer::attach(bufferName, width, height, pixel, fps, buffers);
    }
    if (!opened) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", operation, opened.error().message());
    }

    GstElement* element = find_app_element(ctx, box, operation, elementName, true);
    if (!element) {
        return JS_EXCEPTION;
    }

    auto bridge = std::make_unique<VideoSinkBridge>();
    bridge->name = elementName;
    bridge->kind = "videoSink";
    bridge->bufferName = bufferName;
    bridge->sink = GST_APP_SINK(element);
    bridge->buffer = std::move(opened.value());
    bridge->width = bridge->buffer.width();
    bridge->height = bridge->buffer.height();
    bridge->format = bridge->buffer.format().value_or(pixel);

    g_object_set(G_OBJECT(bridge->sink), "sync", FALSE, "max-buffers", static_cast<guint>(4), "drop", FALSE, nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = video_sink_new_sample;
    gst_app_sink_set_callbacks(bridge->sink, &callbacks, bridge.get(), nullptr);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, result, "videoBufferName", JS_NewString(ctx, bufferName.c_str()));
    JS_SetPropertyStr(ctx, result, "width", JS_NewInt64(ctx, bridge->width));
    JS_SetPropertyStr(ctx, result, "height", JS_NewInt64(ctx, bridge->height));
    JS_SetPropertyStr(ctx, result, "format", JS_NewString(ctx, pixel_to_gst_format(bridge->format)));
    JS_SetPropertyStr(ctx, result, "fps", JS_NewInt64(ctx, bridge->buffer.fps()));
    JS_SetPropertyStr(ctx, result, "ringBuffers", JS_NewInt64(ctx, bridge->buffer.buffers()));
    box->bridges.push_back(std::move(bridge));
    return result;
}

JSValue pipeline_attach_video_sink(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "attachVideoSink");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachVideoSink",
            "attachVideoSink(options) requires an options object");
    }
    std::string elementName = "wl2_video_sink";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string bufferName;
    if (!option_string(ctx, argv[0], "videoBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachVideoSink",
            "attachVideoSink requires a videoBufferName");
    }
    const bool create = option_bool(ctx, argv[0], "create", true);
    int64_t width = 0;
    int64_t height = 0;
    int64_t fps = 30;
    int64_t buffers = 4;
    option_int(ctx, argv[0], "width", width);
    option_int(ctx, argv[0], "height", height);
    option_int(ctx, argv[0], "fps", fps);
    option_int(ctx, argv[0], "buffers", buffers);
    std::string formatName = "RGBA";
    option_string(ctx, argv[0], "format", formatName);
    auto pixel = gst_format_to_pixel(formatName);
    if (!pixel) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachVideoSink",
            "Unsupported video format: " + formatName);
    }
    return attach_video_sink_core(ctx, box, "attachVideoSink", elementName, bufferName, create,
        width, height, fps, buffers, *pixel);
}

JSValue pipeline_attach_audio_sink(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "attachAudioSink");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSink",
            "attachAudioSink(options) requires an options object");
    }
    std::string elementName = "wl2_audio_sink";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string bufferName;
    if (!option_string(ctx, argv[0], "audioBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSink",
            "attachAudioSink requires an audioBufferName");
    }
    if (!authorize_shared_memory(ctx, "attachAudioSink", bufferName)) {
        return JS_EXCEPTION;
    }
    if (has_bridge(box, elementName)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSink",
            "Element '" + elementName + "' is already attached");
    }

    const bool create = option_bool(ctx, argv[0], "create", true);
    int64_t sampleRate = 48000;
    int64_t channels = 2;
    int64_t fps = 50;
    int64_t buffers = 16;
    option_int(ctx, argv[0], "sampleRate", sampleRate);
    option_int(ctx, argv[0], "channels", channels);
    option_int(ctx, argv[0], "fps", fps);
    option_int(ctx, argv[0], "buffers", buffers);
    std::string formatName = "S16LE";
    option_string(ctx, argv[0], "format", formatName);
    auto sf = gst_format_to_sample(formatName);
    if (!sf) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSink",
            "Unsupported audio format: " + formatName);
    }
    if (create && (sampleRate <= 0 || channels <= 0 || fps <= 0 || buffers <= 0)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSink",
            "audio sink with create requires positive sampleRate, channels, fps, and buffers");
    }

    wl2::Result<wl2::AudioBuffer> opened = create
        ? wl2::AudioBuffer::create(bufferName, channels, *sf, sampleRate, fps, buffers)
        : wl2::AudioBuffer::openExisting(bufferName);
    if (!create && opened) {
        channels = opened.value().channels();
        sampleRate = opened.value().sampleRate();
        fps = opened.value().fps();
        buffers = opened.value().buffers();
        sf = opened.value().format().value_or(*sf);
        opened.value().close();
        opened = wl2::AudioBuffer::attach(bufferName, channels, *sf, sampleRate, fps, buffers);
    }
    if (!opened) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "attachAudioSink", opened.error().message());
    }

    GstElement* element = find_app_element(ctx, box, "attachAudioSink", elementName, true);
    if (!element) {
        return JS_EXCEPTION;
    }

    auto bridge = std::make_unique<AudioSinkBridge>();
    bridge->name = elementName;
    bridge->kind = "audioSink";
    bridge->bufferName = bufferName;
    bridge->sink = GST_APP_SINK(element);
    bridge->buffer = std::move(opened.value());
    bridge->channels = bridge->buffer.channels();
    bridge->sampleRate = bridge->buffer.sampleRate();
    bridge->format = *sf;

    g_object_set(G_OBJECT(bridge->sink), "sync", FALSE, "max-buffers", static_cast<guint>(8), "drop", FALSE, nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = audio_sink_new_sample;
    gst_app_sink_set_callbacks(bridge->sink, &callbacks, bridge.get(), nullptr);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, result, "audioBufferName", JS_NewString(ctx, bufferName.c_str()));
    JS_SetPropertyStr(ctx, result, "sampleRate", JS_NewInt64(ctx, bridge->sampleRate));
    JS_SetPropertyStr(ctx, result, "channels", JS_NewInt64(ctx, bridge->channels));
    JS_SetPropertyStr(ctx, result, "format", JS_NewString(ctx, sample_to_gst_format(bridge->format)));
    box->bridges.push_back(std::move(bridge));
    return result;
}

// Build appsrc caps for a video ring and apply them to the element.
void apply_video_source_caps(GstAppSrc* src, wl2::VideoBuffer& buffer) {
    const char* fmt = buffer.format() ? pixel_to_gst_format(*buffer.format()) : "RGBA";
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, fmt,
        "width", G_TYPE_INT, static_cast<gint>(buffer.width()),
        "height", G_TYPE_INT, static_cast<gint>(buffer.height()),
        "framerate", GST_TYPE_FRACTION, static_cast<gint>(buffer.fps()), 1,
        nullptr);
    gst_app_src_set_caps(src, caps);
    gst_caps_unref(caps);
    g_object_set(G_OBJECT(src), "format", GST_FORMAT_TIME, nullptr);
}

JSValue pipeline_attach_video_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "attachVideoSource");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachVideoSource",
            "attachVideoSource(options) requires an options object");
    }
    std::string elementName = "wl2_video_src";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string bufferName;
    if (!option_string(ctx, argv[0], "videoBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachVideoSource",
            "attachVideoSource requires a videoBufferName");
    }
    if (!authorize_shared_memory(ctx, "attachVideoSource", bufferName)) {
        return JS_EXCEPTION;
    }
    if (has_bridge(box, elementName)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachVideoSource",
            "Element '" + elementName + "' is already attached");
    }
    auto opened = wl2::VideoBuffer::openExisting(bufferName);
    if (!opened) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "attachVideoSource", opened.error().message());
    }

    GstElement* element = find_app_element(ctx, box, "attachVideoSource", elementName, false);
    if (!element) {
        return JS_EXCEPTION;
    }

    auto bridge = std::make_unique<VideoSourceBridge>();
    bridge->name = elementName;
    bridge->kind = "videoSource";
    bridge->bufferName = bufferName;
    bridge->src = GST_APP_SRC(element);
    bridge->buffer = std::move(opened.value());
    bridge->fps = bridge->buffer.fps() > 0 ? bridge->buffer.fps() : 30;
    apply_video_source_caps(bridge->src, bridge->buffer);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, result, "videoBufferName", JS_NewString(ctx, bufferName.c_str()));
    JS_SetPropertyStr(ctx, result, "width", JS_NewInt64(ctx, bridge->buffer.width()));
    JS_SetPropertyStr(ctx, result, "height", JS_NewInt64(ctx, bridge->buffer.height()));
    box->bridges.push_back(std::move(bridge));
    return result;
}

JSValue pipeline_attach_audio_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "attachAudioSource");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSource",
            "attachAudioSource(options) requires an options object");
    }
    std::string elementName = "wl2_audio_src";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string bufferName;
    if (!option_string(ctx, argv[0], "audioBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSource",
            "attachAudioSource requires an audioBufferName");
    }
    if (!authorize_shared_memory(ctx, "attachAudioSource", bufferName)) {
        return JS_EXCEPTION;
    }
    if (has_bridge(box, elementName)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachAudioSource",
            "Element '" + elementName + "' is already attached");
    }
    auto opened = wl2::AudioBuffer::openExisting(bufferName);
    if (!opened) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "attachAudioSource", opened.error().message());
    }

    GstElement* element = find_app_element(ctx, box, "attachAudioSource", elementName, false);
    if (!element) {
        return JS_EXCEPTION;
    }

    auto bridge = std::make_unique<AudioSourceBridge>();
    bridge->name = elementName;
    bridge->kind = "audioSource";
    bridge->bufferName = bufferName;
    bridge->src = GST_APP_SRC(element);
    bridge->buffer = std::move(opened.value());
    const char* fmt = bridge->buffer.format() ? sample_to_gst_format(*bridge->buffer.format()) : "S16LE";
    GstCaps* caps = gst_caps_new_simple("audio/x-raw",
        "format", G_TYPE_STRING, fmt,
        "layout", G_TYPE_STRING, "interleaved",
        "rate", G_TYPE_INT, static_cast<gint>(bridge->buffer.sampleRate()),
        "channels", G_TYPE_INT, static_cast<gint>(bridge->buffer.channels()),
        nullptr);
    gst_app_src_set_caps(bridge->src, caps);
    gst_caps_unref(caps);
    g_object_set(G_OBJECT(bridge->src), "format", GST_FORMAT_TIME, nullptr);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, result, "audioBufferName", JS_NewString(ctx, bufferName.c_str()));
    box->bridges.push_back(std::move(bridge));
    return result;
}

JSValue pipeline_push_video_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "pushVideoFrame");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string elementName;
    if (argc > 0) {
        option_string(ctx, argv[0], "elementName", elementName);
    }
    auto* bridge = static_cast<VideoSourceBridge*>(find_bridge(box, "videoSource", elementName));
    if (!bridge) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "pushVideoFrame",
            "No attached video source" + (elementName.empty() ? std::string() : " named '" + elementName + "'"));
    }
    int64_t slot = 0;
    bool latest = false;
    int64_t retries = 3;
    if (argc > 0) {
        option_int(ctx, argv[0], "slot", slot);
        latest = option_bool(ctx, argv[0], "latest", false);
        option_int(ctx, argv[0], "retries", retries);
        retries = std::max<int64_t>(1, retries);
        if (latest) {
            slot = bridge->buffer.pointer(-1);
        }
    }
    GstBuffer* gbuf = nullptr;
    int64_t sequence = -1;
    for (int64_t attempt = 0; attempt < (latest ? retries : 1); ++attempt) {
        if (latest) {
            slot = bridge->buffer.pointer(-1);
            sequence = bridge->buffer.frameSequence(slot);
            if (slot < 0 || sequence < 0) {
                return throw_gst_error(ctx, "gstreamer_membus_failed", "pushVideoFrame",
                    "No committed video frame is available");
            }
        }
        auto view = bridge->buffer.frame(slot);
        if (!view || !view.value().data) {
            return throw_gst_error(ctx, "gstreamer_membus_failed", "pushVideoFrame",
                "Could not read video frame slot " + std::to_string(slot));
        }
        gbuf = gst_buffer_new_allocate(nullptr, view.value().size, nullptr);
        if (!gbuf) {
            return throw_gst_error(ctx, "gstreamer_membus_failed", "pushVideoFrame", "Failed to allocate GstBuffer");
        }
        gst_buffer_fill(gbuf, 0, view.value().data, view.value().size);
        if (!latest || bridge->buffer.frameSequence(slot) == sequence) {
            break;
        }
        gst_buffer_unref(gbuf);
        gbuf = nullptr;
    }
    if (!gbuf) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "pushVideoFrame",
            "Latest video frame changed while it was being copied");
    }

    const int64_t duration = bridge->fps > 0 ? GST_SECOND / bridge->fps : 0;
    int64_t pts = bridge->frameIndex * duration;
    int64_t overridePts = 0;
    if (argc > 0 && option_int(ctx, argv[0], "pts", overridePts)) {
        pts = overridePts;
    }
    int64_t overrideDuration = duration;
    if (argc > 0) {
        option_int(ctx, argv[0], "duration", overrideDuration);
    }
    if (pts < 0 || overrideDuration < 0) {
        gst_buffer_unref(gbuf);
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "pushVideoFrame",
            "pts and duration must be non-negative");
    }
    GST_BUFFER_PTS(gbuf) = static_cast<GstClockTime>(pts);
    GST_BUFFER_DTS(gbuf) = static_cast<GstClockTime>(pts);
    GST_BUFFER_DURATION(gbuf) = static_cast<GstClockTime>(overrideDuration);

    GstFlowReturn flow = gst_app_src_push_buffer(bridge->src, gbuf); // consumes gbuf
    bridge->frameIndex++;
    if (flow != GST_FLOW_OK) {
        return throw_gst_error(ctx, "gstreamer_push_failed", "pushVideoFrame",
            std::string("appsrc rejected the buffer: ") + gst_flow_get_name(flow));
    }
    bridge->pushed.fetch_add(1);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "slot", JS_NewInt64(ctx, slot));
    if (latest) {
        JS_SetPropertyStr(ctx, result, "sequence", JS_NewInt64(ctx, sequence));
    }
    JS_SetPropertyStr(ctx, result, "pts", JS_NewInt64(ctx, pts));
    return result;
}

JSValue pipeline_push_audio_samples(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "pushAudioSamples");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string elementName;
    if (argc > 0) {
        option_string(ctx, argv[0], "elementName", elementName);
    }
    auto* bridge = static_cast<AudioSourceBridge*>(find_bridge(box, "audioSource", elementName));
    if (!bridge) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "pushAudioSamples",
            "No attached audio source" + (elementName.empty() ? std::string() : " named '" + elementName + "'"));
    }
    int64_t slot = 0;
    bool latest = false;
    int64_t retries = 3;
    if (argc > 0) {
        option_int(ctx, argv[0], "slot", slot);
        latest = option_bool(ctx, argv[0], "latest", false);
        option_int(ctx, argv[0], "retries", retries);
        retries = std::max<int64_t>(1, retries);
        if (latest) {
            slot = bridge->buffer.pointer(-1);
        }
    }
    const size_t size = static_cast<size_t>(bridge->buffer.bufferSize());
    GstBuffer* gbuf = nullptr;
    int64_t sequence = -1;
    for (int64_t attempt = 0; attempt < (latest ? retries : 1); ++attempt) {
        if (latest) {
            slot = bridge->buffer.pointer(-1);
            sequence = bridge->buffer.frameSequence(slot);
            if (slot < 0 || sequence < 0) {
                return throw_gst_error(ctx, "gstreamer_membus_failed", "pushAudioSamples",
                    "No committed audio buffer is available");
            }
        }
        auto view = bridge->buffer.buffer(slot);
        if (!view || !view.value().data) {
            return throw_gst_error(ctx, "gstreamer_membus_failed", "pushAudioSamples",
                "Could not read audio slot " + std::to_string(slot));
        }
        gbuf = gst_buffer_new_allocate(nullptr, size, nullptr);
        if (!gbuf) {
            return throw_gst_error(ctx, "gstreamer_membus_failed", "pushAudioSamples", "Failed to allocate GstBuffer");
        }
        gst_buffer_fill(gbuf, 0, view.value().data, size);
        if (!latest || bridge->buffer.frameSequence(slot) == sequence) {
            break;
        }
        gst_buffer_unref(gbuf);
        gbuf = nullptr;
    }
    if (!gbuf) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "pushAudioSamples",
            "Latest audio buffer changed while it was being copied");
    }

    const int64_t bytesPerFrame = bridge->buffer.channels() * bridge->buffer.bytesPerSample();
    const int64_t sampleCount = bytesPerFrame > 0 ? static_cast<int64_t>(size) / bytesPerFrame : 0;
    const int64_t rate = bridge->buffer.sampleRate();
    const int64_t duration = rate > 0 ? sampleCount * GST_SECOND / rate : 0;
    int64_t pts = bridge->bufferIndex * duration;
    int64_t overridePts = 0;
    if (argc > 0 && option_int(ctx, argv[0], "pts", overridePts)) {
        pts = overridePts;
    }
    int64_t overrideDuration = duration;
    if (argc > 0) {
        option_int(ctx, argv[0], "duration", overrideDuration);
    }
    if (pts < 0 || overrideDuration < 0) {
        gst_buffer_unref(gbuf);
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "pushAudioSamples",
            "pts and duration must be non-negative");
    }
    GST_BUFFER_PTS(gbuf) = static_cast<GstClockTime>(pts);
    GST_BUFFER_DTS(gbuf) = static_cast<GstClockTime>(pts);
    GST_BUFFER_DURATION(gbuf) = static_cast<GstClockTime>(overrideDuration);

    GstFlowReturn flow = gst_app_src_push_buffer(bridge->src, gbuf);
    bridge->bufferIndex++;
    if (flow != GST_FLOW_OK) {
        return throw_gst_error(ctx, "gstreamer_push_failed", "pushAudioSamples",
            std::string("appsrc rejected the buffer: ") + gst_flow_get_name(flow));
    }
    bridge->pushed.fetch_add(1);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "slot", JS_NewInt64(ctx, slot));
    if (latest) {
        JS_SetPropertyStr(ctx, result, "sequence", JS_NewInt64(ctx, sequence));
    }
    JS_SetPropertyStr(ctx, result, "samples", JS_NewInt64(ctx, sampleCount));
    return result;
}

// ---- Packet sink/source bridges: PacketBuffer <-> app ----

struct PacketSinkBridge : Bridge {
    GstAppSink* sink = nullptr;
    wl2::PacketBuffer buffer;
    int64_t track = 0;
    std::atomic<int64_t> packets{0};
    std::atomic<int64_t> dropped{0};
    std::atomic<int64_t> bytes{0};
    std::mutex capsMutex;
    std::string negotiatedCaps;

    ~PacketSinkBridge() override {
        if (sink) {
            gst_object_unref(sink);
            sink = nullptr;
        }
        buffer.close();
    }

#if WL2_HAVE_QUICKJS
    JSValue stats(JSContext* ctx) override {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind.c_str()));
        JS_SetPropertyStr(ctx, obj, "packetBufferName", JS_NewString(ctx, bufferName.c_str()));
        JS_SetPropertyStr(ctx, obj, "packets", JS_NewInt64(ctx, packets.load()));
        JS_SetPropertyStr(ctx, obj, "dropped", JS_NewInt64(ctx, dropped.load()));
        JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, bytes.load()));
        JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, buffer.sequence()));
        {
            std::lock_guard<std::mutex> lock(capsMutex);
            JS_SetPropertyStr(ctx, obj, "negotiatedCaps", JS_NewString(ctx, negotiatedCaps.c_str()));
        }
        return obj;
    }
#endif
};

struct PacketSourceBridge : Bridge {
    GstAppSrc* src = nullptr;
    wl2::PacketBuffer buffer;
    int64_t track = 0;
    int64_t lastSequence = -1;
    std::atomic<int64_t> pushed{0};
    std::atomic<int64_t> bytes{0};

    ~PacketSourceBridge() override {
        if (src) {
            gst_object_unref(src);
            src = nullptr;
        }
        buffer.close();
    }

#if WL2_HAVE_QUICKJS
    JSValue stats(JSContext* ctx) override {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, kind.c_str()));
        JS_SetPropertyStr(ctx, obj, "packetBufferName", JS_NewString(ctx, bufferName.c_str()));
        JS_SetPropertyStr(ctx, obj, "pushed", JS_NewInt64(ctx, pushed.load()));
        JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, bytes.load()));
        JS_SetPropertyStr(ctx, obj, "lastSequence", JS_NewInt64(ctx, lastSequence));
        return obj;
    }
#endif
};

GstFlowReturn packet_sink_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* bridge = static_cast<PacketSinkBridge*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }
    GstCaps* caps = gst_sample_get_caps(sample);
    store_caps_once(bridge->capsMutex, bridge->negotiatedCaps, caps);
    std::string capsText = caps_to_string(caps);
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        const int64_t pts = GST_BUFFER_PTS_IS_VALID(buf) ? static_cast<int64_t>(GST_BUFFER_PTS(buf)) : 0;
        const int64_t dts = GST_BUFFER_DTS_IS_VALID(buf) ? static_cast<int64_t>(GST_BUFFER_DTS(buf)) : pts;
        const int64_t duration = GST_BUFFER_DURATION_IS_VALID(buf) ? static_cast<int64_t>(GST_BUFFER_DURATION(buf)) : 0;
        const bool discont = GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DISCONT);
        std::string metadata = packet_metadata_json(capsText, pts, dts, duration,
            static_cast<unsigned>(GST_MINI_OBJECT_FLAGS(buf)), discont);
        auto wrote = bridge->buffer.write(
            std::string_view(reinterpret_cast<const char*>(map.data), map.size),
            packet_kind_from_caps(caps), bridge->track, pts, metadata);
        if (wrote) {
            bridge->packets.fetch_add(1);
            bridge->bytes.fetch_add(static_cast<int64_t>(map.size));
        } else {
            bridge->dropped.fetch_add(1);
        }
        gst_buffer_unmap(buf, &map);
    } else {
        bridge->dropped.fetch_add(1);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

JSValue pipeline_attach_packet_sink(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "attachPacketSink");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (!wl2::libmembusHasV21Surface()) {
        return throw_gst_error(ctx, "gstreamer_unsupported", "attachPacketSink",
            "PacketBuffer bridges require the libmembus 2.1 surface");
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSink",
            "attachPacketSink(options) requires an options object");
    }
    std::string elementName = "wl2_packet_sink";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string bufferName;
    if (!option_string(ctx, argv[0], "packetBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSink",
            "attachPacketSink requires a packetBufferName");
    }
    if (!authorize_shared_memory(ctx, "attachPacketSink", bufferName)) {
        return JS_EXCEPTION;
    }
    if (has_bridge(box, elementName)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSink",
            "Element '" + elementName + "' is already attached");
    }

    const bool create = option_bool(ctx, argv[0], "create", true);
    int64_t buffers = 32;
    int64_t arenaSize = 1 << 20;
    int64_t maxRecord = 256 << 10;
    int64_t align = 0;
    int64_t track = 0;
    option_int(ctx, argv[0], "buffers", buffers);
    option_int(ctx, argv[0], "arenaSize", arenaSize);
    option_int(ctx, argv[0], "maxRecord", maxRecord);
    option_int(ctx, argv[0], "align", align);
    option_int(ctx, argv[0], "track", track);
    std::string metadata;
    option_string(ctx, argv[0], "metadata", metadata);
    std::string caps;
    if (option_string(ctx, argv[0], "caps", caps) && metadata.empty()) {
        metadata = caps;
    }
    if (create && (buffers <= 0 || arenaSize <= 0 || maxRecord <= 0)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSink",
            "packet sink with create requires positive buffers, arenaSize, and maxRecord");
    }

    if (!create) {
        return throw_gst_error(ctx, "gstreamer_unsupported", "attachPacketSink",
            "PacketBuffer sink attach requires create:true; libmembus does not expose a writable attach surface");
    }
    auto opened = wl2::PacketBuffer::create(bufferName, buffers, arenaSize, maxRecord, align, 0, metadata);
    if (!opened) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "attachPacketSink", opened.error().message());
    }

    GstElement* element = find_app_element(ctx, box, "attachPacketSink", elementName, true);
    if (!element) {
        return JS_EXCEPTION;
    }

    auto bridge = std::make_unique<PacketSinkBridge>();
    bridge->name = elementName;
    bridge->kind = "packetSink";
    bridge->bufferName = bufferName;
    bridge->sink = GST_APP_SINK(element);
    bridge->buffer = std::move(opened.value());
    bridge->track = track;
    g_object_set(G_OBJECT(bridge->sink), "sync", FALSE, "max-buffers", static_cast<guint>(8), "drop", FALSE, nullptr);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = packet_sink_new_sample;
    gst_app_sink_set_callbacks(bridge->sink, &callbacks, bridge.get(), nullptr);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, result, "packetBufferName", JS_NewString(ctx, bufferName.c_str()));
    JS_SetPropertyStr(ctx, result, "buffers", JS_NewInt64(ctx, bridge->buffer.buffers()));
    JS_SetPropertyStr(ctx, result, "arenaSize", JS_NewInt64(ctx, bridge->buffer.arenaSize()));
    JS_SetPropertyStr(ctx, result, "maxRecord", JS_NewInt64(ctx, bridge->buffer.maxRecord()));
    box->bridges.push_back(std::move(bridge));
    return result;
}

JSValue pipeline_attach_packet_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "attachPacketSource");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (!wl2::libmembusHasV21Surface()) {
        return throw_gst_error(ctx, "gstreamer_unsupported", "attachPacketSource",
            "PacketBuffer bridges require the libmembus 2.1 surface");
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSource",
            "attachPacketSource(options) requires an options object");
    }
    std::string elementName = "wl2_packet_src";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string bufferName;
    if (!option_string(ctx, argv[0], "packetBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSource",
            "attachPacketSource requires a packetBufferName");
    }
    if (!authorize_shared_memory(ctx, "attachPacketSource", bufferName)) {
        return JS_EXCEPTION;
    }
    if (has_bridge(box, elementName)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSource",
            "Element '" + elementName + "' is already attached");
    }

    auto opened = wl2::PacketBuffer::openExisting(bufferName);
    if (!opened) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", "attachPacketSource", opened.error().message());
    }

    GstElement* element = find_app_element(ctx, box, "attachPacketSource", elementName, false);
    if (!element) {
        return JS_EXCEPTION;
    }

    int64_t track = 0;
    option_int(ctx, argv[0], "track", track);
    std::string capsText;
    option_string(ctx, argv[0], "caps", capsText);
    if (capsText.empty()) {
        std::string metadata = opened.value().metadata();
        if (metadata.find('/') != std::string::npos && metadata.find('{') == std::string::npos) {
            capsText = metadata;
        }
    }

    auto bridge = std::make_unique<PacketSourceBridge>();
    bridge->name = elementName;
    bridge->kind = "packetSource";
    bridge->bufferName = bufferName;
    bridge->src = GST_APP_SRC(element);
    bridge->buffer = std::move(opened.value());
    bridge->track = track;
    if (!capsText.empty()) {
        GstCaps* caps = gst_caps_from_string(capsText.c_str());
        if (!caps) {
            return throw_gst_error(ctx, "gstreamer_invalid_argument", "attachPacketSource",
                "Invalid packet source caps: " + capsText);
        }
        gst_app_src_set_caps(bridge->src, caps);
        gst_caps_unref(caps);
    }
    g_object_set(G_OBJECT(bridge->src), "format", GST_FORMAT_TIME, "is-live", TRUE, nullptr);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, result, "packetBufferName", JS_NewString(ctx, bufferName.c_str()));
    JS_SetPropertyStr(ctx, result, "caps", JS_NewString(ctx, capsText.c_str()));
    box->bridges.push_back(std::move(bridge));
    return result;
}

JSValue push_packet_payload(JSContext* ctx, PacketSourceBridge* bridge, const std::string& payload,
    int64_t pts, int64_t duration, const char* operation) {
    GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, payload.size(), nullptr);
    if (!gbuf) {
        return throw_gst_error(ctx, "gstreamer_membus_failed", operation, "Failed to allocate GstBuffer");
    }
    if (!payload.empty()) {
        gst_buffer_fill(gbuf, 0, payload.data(), payload.size());
    }
    if (pts < 0 || duration < 0) {
        gst_buffer_unref(gbuf);
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "pts and duration must be non-negative");
    }
    GST_BUFFER_PTS(gbuf) = static_cast<GstClockTime>(pts);
    GST_BUFFER_DTS(gbuf) = static_cast<GstClockTime>(pts);
    if (duration > 0) {
        GST_BUFFER_DURATION(gbuf) = static_cast<GstClockTime>(duration);
    }
    GstFlowReturn flow = gst_app_src_push_buffer(bridge->src, gbuf);
    if (flow != GST_FLOW_OK) {
        return throw_gst_error(ctx, "gstreamer_push_failed", operation,
            std::string("appsrc rejected the packet: ") + gst_flow_get_name(flow));
    }
    bridge->pushed.fetch_add(1);
    bridge->bytes.fetch_add(static_cast<int64_t>(payload.size()));
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "pts", JS_NewInt64(ctx, pts));
    JS_SetPropertyStr(ctx, result, "bytes", JS_NewInt64(ctx, static_cast<int64_t>(payload.size())));
    return result;
}

JSValue pipeline_push_packet(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "pushPacket");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string elementName;
    if (argc > 0) {
        option_string(ctx, argv[0], "elementName", elementName);
    }
    auto* bridge = static_cast<PacketSourceBridge*>(find_bridge(box, "packetSource", elementName));
    if (!bridge) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "pushPacket",
            "No attached packet source" + (elementName.empty() ? std::string() : " named '" + elementName + "'"));
    }

    std::string payload;
    int64_t pts = 0;
    int64_t duration = 0;
    bool havePayload = false;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue data = JS_GetPropertyStr(ctx, argv[0], "data");
        if (!JS_IsUndefined(data) && !JS_IsNull(data)) {
            payload = js_bytes(ctx, data);
            havePayload = true;
        }
        JS_FreeValue(ctx, data);
        option_int(ctx, argv[0], "pts", pts);
        option_int(ctx, argv[0], "duration", duration);
    }

    if (!havePayload) {
        int64_t slot = -1;
        int64_t waitTimeoutMs = 0;
        if (argc > 0) {
            option_int(ctx, argv[0], "slot", slot);
            option_int(ctx, argv[0], "waitTimeoutMs", waitTimeoutMs);
        }
        if (waitTimeoutMs > 0) {
            bridge->buffer.waitForPacket(std::chrono::milliseconds{waitTimeoutMs}, bridge->lastSequence);
        }
        auto record = slot >= 0 ? bridge->buffer.record(slot) : bridge->buffer.latest();
        if (!record) {
            return throw_gst_error(ctx, "gstreamer_membus_failed", "pushPacket", record.error().message());
        }
        payload = std::move(record.value().payload);
        pts = record.value().pts;
        bridge->lastSequence = record.value().sequence;
    }

    return push_packet_payload(ctx, bridge, payload, pts, duration, "pushPacket");
}

JSValue pipeline_end_of_stream(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "endOfStream");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string elementName;
    if (argc > 0) {
        option_string(ctx, argv[0], "elementName", elementName);
    }
    int64_t signalled = 0;
    for (auto& bridge : box->bridges) {
        if (bridge->kind != "videoSource" && bridge->kind != "audioSource" && bridge->kind != "packetSource") {
            continue;
        }
        if (!elementName.empty() && bridge->name != elementName) {
            continue;
        }
        GstAppSrc* src = nullptr;
        if (bridge->kind == "videoSource") {
            src = static_cast<VideoSourceBridge*>(bridge.get())->src;
        } else if (bridge->kind == "audioSource") {
            src = static_cast<AudioSourceBridge*>(bridge.get())->src;
        } else {
            src = static_cast<PacketSourceBridge*>(bridge.get())->src;
        }
        if (src) {
            gst_app_src_end_of_stream(src);
            ++signalled;
        }
    }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, result, "signalled", JS_NewInt64(ctx, signalled));
    return result;
}

JSValue pipeline_stats(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "stats");
    if (!box) {
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObject(ctx);
    for (auto& bridge : box->bridges) {
        JS_SetPropertyStr(ctx, obj, bridge->name.c_str(), bridge->stats(ctx));
    }
    return obj;
}

// Convenience helper: build a videotestsrc pipeline that publishes RGBA frames
// into a VideoBuffer and return the wired, ready-to-play Pipeline.
JSValue gst_test_pattern_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "testPattern");
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "testPattern",
            "testPattern(options) requires an options object");
    }
    std::string bufferName;
    if (!option_string(ctx, argv[0], "videoBufferName", bufferName) || bufferName.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "testPattern",
            "testPattern requires a videoBufferName");
    }
    int64_t width = 320;
    int64_t height = 240;
    int64_t fps = 30;
    int64_t numBuffers = 30;
    option_int(ctx, argv[0], "width", width);
    option_int(ctx, argv[0], "height", height);
    option_int(ctx, argv[0], "fps", fps);
    option_int(ctx, argv[0], "numBuffers", numBuffers);
    std::string pattern = "smpte";
    option_string(ctx, argv[0], "pattern", pattern);
    std::string formatName = "RGBA";
    option_string(ctx, argv[0], "format", formatName);
    auto pixel = gst_format_to_pixel(formatName);
    if (!pixel) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "testPattern",
            "Unsupported video format: " + formatName);
    }
    if (width <= 0 || height <= 0) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "testPattern",
            "testPattern requires positive width and height");
    }

    std::string description = "videotestsrc name=wl2_video_src pattern=" + pattern
        + " num-buffers=" + std::to_string(numBuffers) + " is-live=false ! videoconvert ! video/x-raw,format="
        + formatName + ",width=" + std::to_string(width) + ",height=" + std::to_string(height)
        + ",framerate=" + std::to_string(fps) + "/1 ! appsink name=wl2_video_sink sync=false";

    GError* error = nullptr;
    GstElement* element = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string message = error->message ? error->message : "gst_parse_launch failed";
        g_error_free(error);
        if (element) {
            gst_object_unref(element);
        }
        return throw_gst_error(ctx, "gstreamer_parse_failed", "testPattern", message);
    }
    if (!element || !GST_IS_PIPELINE(element)) {
        if (element) {
            gst_object_unref(element);
        }
        return throw_gst_error(ctx, "gstreamer_parse_failed", "testPattern", "testPattern did not produce a pipeline");
    }

    JSValue pipelineObj = new_pipeline_object(ctx, element);
    if (JS_IsException(pipelineObj)) {
        return pipelineObj;
    }
    auto* box = static_cast<PipelineBox*>(JS_GetOpaque(pipelineObj, g_pipelineClassId));
    JSValue attachResult = attach_video_sink_core(ctx, box, "testPattern", "wl2_video_sink", bufferName,
        true, width, height, fps, 4, *pixel);
    if (JS_IsException(attachResult)) {
        JS_FreeValue(ctx, pipelineObj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attachResult);
    return pipelineObj;
}

#endif // WL2_HAVE_QUICKJS

#else // !WL2_GSTREAMER_HAVE_APP

JSValue bridge_unsupported(JSContext* ctx, const char* operation) {
    return throw_gst_error(ctx, "gstreamer_unsupported", operation,
        "This build lacks gstreamer-app-1.0; media membus bridges are unavailable");
}
JSValue pipeline_attach_video_sink(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "attachVideoSink"); }
JSValue pipeline_attach_audio_sink(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "attachAudioSink"); }
JSValue pipeline_attach_video_source(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "attachVideoSource"); }
JSValue pipeline_attach_audio_source(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "attachAudioSource"); }
JSValue pipeline_attach_packet_sink(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "attachPacketSink"); }
JSValue pipeline_attach_packet_source(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "attachPacketSource"); }
JSValue pipeline_push_video_frame(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "pushVideoFrame"); }
JSValue pipeline_push_audio_samples(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "pushAudioSamples"); }
JSValue pipeline_push_packet(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "pushPacket"); }
JSValue pipeline_end_of_stream(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "endOfStream"); }
JSValue pipeline_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "stats"); }
JSValue gst_test_pattern_fn(JSContext* ctx, JSValueConst, int, JSValueConst*) { return bridge_unsupported(ctx, "testPattern"); }

#endif // WL2_GSTREAMER_HAVE_APP

} // namespace wl2_gstreamer

#endif // WL2_HAVE_QUICKJS
