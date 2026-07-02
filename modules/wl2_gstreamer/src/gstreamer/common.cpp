#include "internal.h"

#include <mutex>

namespace wl2_gstreamer {

const char* const GstreamerApi = R"(Exports JavaScript module wl2:gstreamer.

Pipeline runtime backed by GStreamer.

Functions:
  version()                       -> { module, gstreamer: { string, major, minor, micro, nano } }
  capabilities()                  -> { gstreamer, features, launchTrusted }
  listPlugins(options?)           -> [ { name, version, description, license, source } ]
  listElements(options?)          -> [ { name, longName, klass, description } ]
  parseLaunch(description, opts?)  -> Pipeline
  testPattern(options)            -> Pipeline publishing videotestsrc to VideoBuffer
  filePlayback(options)           -> Pipeline reading a file into VideoBuffer/AudioBuffer
  recordVideoBuffer(options)      -> Pipeline writing VideoBuffer frames to a file
  recordPacketBuffer(options)     -> Pipeline writing PacketBuffer payloads to a file
  discoverMedia(options)          -> { duration, seekable, live, caps?, tags?, streams }
  captureDevice(options)          -> Pipeline capturing v4l2 video or a synthetic test source
  sendUdpPackets(options)         -> PacketBuffer -> UDP
  receiveUdpPackets(options)      -> UDP -> PacketBuffer
  sendRtpPackets(options)         -> PacketBuffer RTP payloads -> UDP
  receiveRtpPackets(options)      -> UDP RTP payloads -> PacketBuffer
  sendTcpPackets(options)         -> PacketBuffer -> TCP client sink
  receiveTcpPackets(options)      -> TCP server source -> PacketBuffer
  streamVideoUdp(options)         -> VideoBuffer -> encoder/payloader -> UDP
  streamVideoTcp(options)         -> VideoBuffer -> encoder/muxer -> TCP client sink
  rtspPlayback(options)           -> RTSP decoded video -> VideoBuffer
  teeVideoBuffer(options)         -> tee source into a VideoBuffer and optional file
  overlayVideoBuffer(options)     -> source through textoverlay into a VideoBuffer
  DeviceMonitor.create(options?)  -> { ok, devices }
  Caps.parse(text)                -> { text, structureCount, structures }

Pipeline methods:
  state()                         -> { state, pending, result }
  setState("null"|"ready"|"paused"|"playing") -> { state, pending, result }
  play() / pause() / stop()       -> { state, pending, result }
  queryPosition()                 -> { ok, position }   (nanoseconds)
  queryDuration()                 -> { ok, duration }   (nanoseconds)
  seek({ position, flush? })      -> { ok }
  busPoll({ timeoutMs?, max? })   -> [ { type, source, ... } ]
  attachVideoSink(options)        -> appsink -> VideoBuffer
  attachAudioSink(options)        -> appsink -> AudioBuffer
  attachPacketSink(options)       -> appsink -> PacketBuffer (requires create:true)
  attachVideoSource(options)      -> VideoBuffer -> appsrc
  attachAudioSource(options)      -> AudioBuffer -> appsrc
  attachPacketSource(options)     -> PacketBuffer -> appsrc
  pushVideoFrame(options)         -> push one VideoBuffer slot to appsrc
  pushAudioSamples(options)       -> push one AudioBuffer slot to appsrc
  pushPacket(options)             -> push one PacketBuffer record or byte payload to appsrc
  endOfStream(options?)           -> signal EOS on attached appsrc elements
  stats()                         -> bridge counters and negotiated caps
  snapshot(options?)              -> latest video sink frame metadata, optional file export
  queryLatency()                  -> { ok, supported, live?, minLatency?, maxLatency? }
  negotiatedCaps({ element, pad? }) -> current/allowed caps on a named element pad
  setOverlayText({ elementName?, text }) -> update a live textoverlay
  close()

Security model:
  parseLaunch() runs GStreamer launch syntax and is a TRUSTED-INPUT API; treat
  launch strings like code. File, device, and network access performed by a
  pipeline is still subject to the host runtime policy. See docs/security.md.)";

// GStreamer is initialized once per process. gst_init_check is safe to call
// this way; the flag guards repeated initialization from multiple contexts.
std::once_flag g_gstInitOnce;
bool g_gstInitOk = false;
std::string g_gstInitError;

bool ensure_gst_init() {
    std::call_once(g_gstInitOnce, [] {
        GError* error = nullptr;
        if (gst_init_check(nullptr, nullptr, &error)) {
            g_gstInitOk = true;
        } else {
            g_gstInitOk = false;
            g_gstInitError = error && error->message ? error->message : "gst_init_check failed";
        }
        if (error) {
            g_error_free(error);
        }
    });
    return g_gstInitOk;
}

#if WL2_HAVE_QUICKJS

// --- Error contract ---------------------------------------------------------

JSValue throw_gst_error(
    JSContext* ctx,
    const char* code,
    const char* operation,
    const std::string& message,
    const std::string& debug,
    const std::string& element) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "GstreamerError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_gstreamer"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    if (!debug.empty()) {
        JS_SetPropertyStr(ctx, error, "debug", JS_NewString(ctx, debug.c_str()));
    }
    if (!element.empty()) {
        JS_SetPropertyStr(ctx, error, "element", JS_NewString(ctx, element.c_str()));
    }
    return JS_Throw(ctx, error);
}

wl2::Runtime* current_runtime(JSContext* ctx) {
    return static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
}

JSValue throw_gst_init_error(JSContext* ctx, const char* operation) {
    return throw_gst_error(ctx, "gstreamer_init_failed", operation,
        g_gstInitError.empty() ? "GStreamer failed to initialize" : g_gstInitError);
}

// --- Option helpers ---------------------------------------------------------

bool option_int(JSContext* ctx, JSValueConst options, const char* key, int64_t& out) {
    if (!JS_IsObject(options)) {
        return false;
    }
    JSValue value = JS_GetPropertyStr(ctx, options, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    int64_t parsed = 0;
    int rc = JS_ToInt64(ctx, &parsed, value);
    JS_FreeValue(ctx, value);
    if (rc < 0) {
        return false;
    }
    out = parsed;
    return true;
}

bool option_bool(JSContext* ctx, JSValueConst options, const char* key, bool fallback) {
    if (!JS_IsObject(options)) {
        return fallback;
    }
    JSValue value = JS_GetPropertyStr(ctx, options, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    bool result = JS_ToBool(ctx, value) == 1;
    JS_FreeValue(ctx, value);
    return result;
}

bool option_string(JSContext* ctx, JSValueConst options, const char* key, std::string& out) {
    if (!JS_IsObject(options)) {
        return false;
    }
    JSValue value = JS_GetPropertyStr(ctx, options, key);
    if (!JS_IsString(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    const char* text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (!text) {
        return false;
    }
    out = text;
    JS_FreeCString(ctx, text);
    return true;
}

// --- version / capabilities / listing ---------------------------------------

JSValue gst_version_object(JSContext* ctx) {
    guint major = 0;
    guint minor = 0;
    guint micro = 0;
    guint nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    JSValue obj = JS_NewObject(ctx);
    gchar* text = gst_version_string();
    JS_SetPropertyStr(ctx, obj, "string", JS_NewString(ctx, text ? text : ""));
    if (text) {
        g_free(text);
    }
    JS_SetPropertyStr(ctx, obj, "major", JS_NewInt32(ctx, static_cast<int32_t>(major)));
    JS_SetPropertyStr(ctx, obj, "minor", JS_NewInt32(ctx, static_cast<int32_t>(minor)));
    JS_SetPropertyStr(ctx, obj, "micro", JS_NewInt32(ctx, static_cast<int32_t>(micro)));
    JS_SetPropertyStr(ctx, obj, "nano", JS_NewInt32(ctx, static_cast<int32_t>(nano)));
    return obj;
}

JSValue gst_version_fn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JSValue module = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module, "version", JS_NewString(ctx, WL2_VERSION));
    JS_SetPropertyStr(ctx, module, "build", JS_NewString(ctx, WL2_BUILD));
    JS_SetPropertyStr(ctx, obj, "module", module);
    JS_SetPropertyStr(ctx, obj, "gstreamer", gst_version_object(ctx));
    return obj;
}

JSValue gst_capabilities_fn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "gstreamer", gst_version_object(ctx));

    JSValue features = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, features, "app", JS_NewBool(ctx, kHaveApp));
    JS_SetPropertyStr(ctx, features, "video", JS_NewBool(ctx, kHaveVideo));
    JS_SetPropertyStr(ctx, features, "audio", JS_NewBool(ctx, kHaveAudio));
    JS_SetPropertyStr(ctx, features, "deviceMonitor", JS_NewBool(ctx, kHavePbutils));
    JS_SetPropertyStr(ctx, obj, "features", features);

    JSValue bridges = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, bridges, "video", JS_NewBool(ctx, kHaveApp));
    JS_SetPropertyStr(ctx, bridges, "audio", JS_NewBool(ctx, kHaveApp));
    JS_SetPropertyStr(ctx, bridges, "packet", JS_NewBool(ctx, kHaveApp && wl2::libmembusHasV21Surface()));
    JS_SetPropertyStr(ctx, obj, "bridges", bridges);

    JS_SetPropertyStr(ctx, obj, "launchTrusted", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "initialized", JS_NewBool(ctx, ensure_gst_init()));
    return obj;
}

JSValue gst_list_plugins_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "listPlugins");
    }
    std::string filter;
    if (argc > 0) {
        option_string(ctx, argv[0], "filter", filter);
    }

    GstRegistry* registry = gst_registry_get();
    GList* plugins = gst_registry_get_plugin_list(registry);
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (GList* item = plugins; item != nullptr; item = item->next) {
        auto* plugin = static_cast<GstPlugin*>(item->data);
        const char* name = gst_plugin_get_name(plugin);
        if (!filter.empty() && (!name || std::string(name).find(filter) == std::string::npos)) {
            continue;
        }
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name ? name : ""));
        const char* version = gst_plugin_get_version(plugin);
        JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, version ? version : ""));
        const char* description = gst_plugin_get_description(plugin);
        JS_SetPropertyStr(ctx, obj, "description", JS_NewString(ctx, description ? description : ""));
        const char* license = gst_plugin_get_license(plugin);
        JS_SetPropertyStr(ctx, obj, "license", JS_NewString(ctx, license ? license : ""));
        const char* source = gst_plugin_get_source(plugin);
        JS_SetPropertyStr(ctx, obj, "source", JS_NewString(ctx, source ? source : ""));
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    gst_plugin_list_free(plugins);
    return array;
}

JSValue gst_list_elements_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "listElements");
    }
    std::string filter;
    if (argc > 0) {
        option_string(ctx, argv[0], "filter", filter);
    }

    GstRegistry* registry = gst_registry_get();
    GList* features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (GList* item = features; item != nullptr; item = item->next) {
        auto* factory = GST_ELEMENT_FACTORY(item->data);
        const char* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        if (!filter.empty() && (!name || std::string(name).find(filter) == std::string::npos)) {
            continue;
        }
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name ? name : ""));
        const char* longName = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_LONGNAME);
        JS_SetPropertyStr(ctx, obj, "longName", JS_NewString(ctx, longName ? longName : ""));
        const char* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
        JS_SetPropertyStr(ctx, obj, "klass", JS_NewString(ctx, klass ? klass : ""));
        const char* description = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_DESCRIPTION);
        JS_SetPropertyStr(ctx, obj, "description", JS_NewString(ctx, description ? description : ""));
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    gst_plugin_feature_list_free(features);
    return array;
}

// --- Raw media format mapping -----------------------------------------------

// Map a GStreamer video caps format string to the packed libmembus pixel format.
std::optional<wl2::VideoPixelFormat> gst_format_to_pixel(const std::string& format) {
    if (format == "RGBA") return wl2::VideoPixelFormat::Rgba32;
    if (format == "BGRA") return wl2::VideoPixelFormat::Bgra32;
    if (format == "RGB") return wl2::VideoPixelFormat::Rgb24;
    if (format == "BGR") return wl2::VideoPixelFormat::Bgr24;
    if (format == "GRAY8") return wl2::VideoPixelFormat::Gray8;
    if (format == "YUY2") return wl2::VideoPixelFormat::Yuyv422;
    if (format == "UYVY") return wl2::VideoPixelFormat::Uyvy422;
    return std::nullopt;
}

const char* pixel_to_gst_format(wl2::VideoPixelFormat format) {
    switch (format) {
        case wl2::VideoPixelFormat::Rgba32: return "RGBA";
        case wl2::VideoPixelFormat::Bgra32: return "BGRA";
        case wl2::VideoPixelFormat::Rgb24: return "RGB";
        case wl2::VideoPixelFormat::Bgr24: return "BGR";
        case wl2::VideoPixelFormat::Gray8: return "GRAY8";
        case wl2::VideoPixelFormat::Yuyv422: return "YUY2";
        case wl2::VideoPixelFormat::Uyvy422: return "UYVY";
    }
    return "RGBA";
}

std::optional<wl2::AudioSampleFormat> gst_format_to_sample(const std::string& format) {
    if (format == "U8") return wl2::AudioSampleFormat::U8;
    if (format == "S16LE") return wl2::AudioSampleFormat::S16Le;
    if (format == "S24LE") return wl2::AudioSampleFormat::S24Le;
    if (format == "S32LE") return wl2::AudioSampleFormat::S32Le;
    if (format == "F32LE") return wl2::AudioSampleFormat::F32Le;
    return std::nullopt;
}

const char* sample_to_gst_format(wl2::AudioSampleFormat format) {
    switch (format) {
        case wl2::AudioSampleFormat::U8: return "U8";
        case wl2::AudioSampleFormat::S16Le: return "S16LE";
        case wl2::AudioSampleFormat::S24Le: return "S24LE";
        case wl2::AudioSampleFormat::S32Le: return "S32LE";
        case wl2::AudioSampleFormat::F32Le: return "F32LE";
    }
    return "S16LE";
}

#endif // WL2_HAVE_QUICKJS

} // namespace wl2_gstreamer
