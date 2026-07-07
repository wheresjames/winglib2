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
  elementInfo(name)               -> { found, name, metadata, properties, padTemplates, uriHandler? }
  hasProperty(element, property)  -> boolean
  uriHandlers(options?)           -> [ { element, direction, protocols } ]
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
  watchBus({ onMessage?, onError?, onWarning?, onEos? }) -> { ok }
                                  push-style bus callbacks on the JS thread; an
                                  active watch consumes all bus messages (busPoll
                                  returns nothing) and keeps the event loop alive
                                  until unwatchBus() or close()
  unwatchBus()                    -> { ok }
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

std::string property_type_name(GParamSpec* pspec) {
    if (!pspec) {
        return {};
    }
    GType type = G_PARAM_SPEC_VALUE_TYPE(pspec);
    const char* name = g_type_name(type);
    return name ? name : "";
}

JSValue flags_array(JSContext* ctx, GParamFlags flags) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    auto add = [&](const char* value) {
        JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, value));
    };
    if (flags & G_PARAM_READABLE) add("readable");
    if (flags & G_PARAM_WRITABLE) add("writable");
    if (flags & G_PARAM_CONSTRUCT) add("construct");
    if (flags & G_PARAM_CONSTRUCT_ONLY) add("constructOnly");
    if (flags & G_PARAM_DEPRECATED) add("deprecated");
    return array;
}

JSValue element_factory_to_info(JSContext* ctx, GstElementFactory* factory) {
    JSValue obj = JS_NewObject(ctx);
    const char* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    JS_SetPropertyStr(ctx, obj, "found", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name ? name : ""));

    JSValue metadata = JS_NewObject(ctx);
    auto set_meta = [&](const char* key, const char* gstKey) {
        const char* value = gst_element_factory_get_metadata(factory, gstKey);
        JS_SetPropertyStr(ctx, metadata, key, JS_NewString(ctx, value ? value : ""));
    };
    set_meta("longName", GST_ELEMENT_METADATA_LONGNAME);
    set_meta("klass", GST_ELEMENT_METADATA_KLASS);
    set_meta("description", GST_ELEMENT_METADATA_DESCRIPTION);
    set_meta("author", GST_ELEMENT_METADATA_AUTHOR);
    JS_SetPropertyStr(ctx, obj, "metadata", metadata);

    GstElement* element = gst_element_factory_create(factory, nullptr);
    JSValue properties = JS_NewArray(ctx);
    if (element) {
        guint count = 0;
        GParamSpec** specs = g_object_class_list_properties(G_OBJECT_GET_CLASS(element), &count);
        for (guint i = 0; i < count; ++i) {
            GParamSpec* pspec = specs[i];
            JSValue prop = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, prop, "name", JS_NewString(ctx, pspec->name ? pspec->name : ""));
            const char* nick = g_param_spec_get_nick(pspec);
            const char* blurb = g_param_spec_get_blurb(pspec);
            JS_SetPropertyStr(ctx, prop, "nick", JS_NewString(ctx, nick ? nick : ""));
            JS_SetPropertyStr(ctx, prop, "blurb", JS_NewString(ctx, blurb ? blurb : ""));
            std::string typeName = property_type_name(pspec);
            JS_SetPropertyStr(ctx, prop, "type", JS_NewString(ctx, typeName.c_str()));
            JS_SetPropertyStr(ctx, prop, "flags", flags_array(ctx, pspec->flags));
            JS_SetPropertyUint32(ctx, properties, i, prop);
        }
        g_free(specs);
        gst_object_unref(element);
    }
    JS_SetPropertyStr(ctx, obj, "properties", properties);

    JSValue pads = JS_NewArray(ctx);
    const GList* templates = gst_element_factory_get_static_pad_templates(factory);
    uint32_t padIndex = 0;
    for (const GList* item = templates; item; item = item->next) {
        auto* templ = static_cast<GstStaticPadTemplate*>(item->data);
        JSValue pad = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, pad, "nameTemplate", JS_NewString(ctx, templ->name_template ? templ->name_template : ""));
        const char* direction = "unknown";
        if (templ->direction == GST_PAD_SRC) direction = "src";
        else if (templ->direction == GST_PAD_SINK) direction = "sink";
        JS_SetPropertyStr(ctx, pad, "direction", JS_NewString(ctx, direction));
        const char* presence = "unknown";
        if (templ->presence == GST_PAD_ALWAYS) presence = "always";
        else if (templ->presence == GST_PAD_SOMETIMES) presence = "sometimes";
        else if (templ->presence == GST_PAD_REQUEST) presence = "request";
        JS_SetPropertyStr(ctx, pad, "presence", JS_NewString(ctx, presence));
        GstCaps* caps = gst_static_caps_get(&templ->static_caps);
        JS_SetPropertyStr(ctx, pad, "caps", caps_to_js(ctx, caps));
        if (caps) {
            gst_caps_unref(caps);
        }
        JS_SetPropertyUint32(ctx, pads, padIndex++, pad);
    }
    JS_SetPropertyStr(ctx, obj, "padTemplates", pads);

    if (gst_element_factory_get_uri_type(factory) != GST_URI_UNKNOWN) {
        JSValue uri = JS_NewObject(ctx);
        const GstURIType type = gst_element_factory_get_uri_type(factory);
        JS_SetPropertyStr(ctx, uri, "direction", JS_NewString(ctx, type == GST_URI_SRC ? "src" : "sink"));
        JSValue protocols = JS_NewArray(ctx);
        const gchar* const* uriProtocols = gst_element_factory_get_uri_protocols(factory);
        uint32_t protocolIndex = 0;
        if (uriProtocols) {
            for (const gchar* const* p = uriProtocols; *p; ++p) {
                JS_SetPropertyUint32(ctx, protocols, protocolIndex++, JS_NewString(ctx, *p));
            }
        }
        JS_SetPropertyStr(ctx, uri, "protocols", protocols);
        JS_SetPropertyStr(ctx, obj, "uriHandler", uri);
    }

    return obj;
}

JSValue gst_element_info_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "elementInfo");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "elementInfo",
            "elementInfo(name) requires an element name");
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string name = s ? s : "";
    if (s) {
        JS_FreeCString(ctx, s);
    }
    GstElementFactory* factory = gst_element_factory_find(name.c_str());
    if (!factory) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "found", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name.c_str()));
        return obj;
    }
    JSValue obj = element_factory_to_info(ctx, factory);
    gst_object_unref(factory);
    return obj;
}

JSValue gst_has_property_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "hasProperty");
    }
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "hasProperty",
            "hasProperty(element, property) requires element and property names");
    }
    const char* es = JS_ToCString(ctx, argv[0]);
    const char* ps = JS_ToCString(ctx, argv[1]);
    std::string elementName = es ? es : "";
    std::string propertyName = ps ? ps : "";
    if (es) JS_FreeCString(ctx, es);
    if (ps) JS_FreeCString(ctx, ps);
    GstElementFactory* factory = gst_element_factory_find(elementName.c_str());
    if (!factory) {
        return JS_NewBool(ctx, false);
    }
    GstElement* element = gst_element_factory_create(factory, nullptr);
    gst_object_unref(factory);
    if (!element) {
        return JS_NewBool(ctx, false);
    }
    const bool found = g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName.c_str()) != nullptr;
    gst_object_unref(element);
    return JS_NewBool(ctx, found);
}

JSValue gst_uri_handlers_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "uriHandlers");
    }
    std::string protocolFilter;
    if (argc > 0) {
        option_string(ctx, argv[0], "protocol", protocolFilter);
    }
    GstRegistry* registry = gst_registry_get();
    GList* features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (GList* item = features; item != nullptr; item = item->next) {
        auto* factory = GST_ELEMENT_FACTORY(item->data);
        const GstURIType type = gst_element_factory_get_uri_type(factory);
        if (type == GST_URI_UNKNOWN) {
            continue;
        }
        const gchar* const* protocolsRaw = gst_element_factory_get_uri_protocols(factory);
        bool include = protocolFilter.empty();
        JSValue protocols = JS_NewArray(ctx);
        uint32_t protocolIndex = 0;
        if (protocolsRaw) {
            for (const gchar* const* p = protocolsRaw; *p; ++p) {
                if (!protocolFilter.empty() && protocolFilter == *p) {
                    include = true;
                }
                JS_SetPropertyUint32(ctx, protocols, protocolIndex++, JS_NewString(ctx, *p));
            }
        }
        if (include) {
            JSValue obj = JS_NewObject(ctx);
            const char* name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
            JS_SetPropertyStr(ctx, obj, "element", JS_NewString(ctx, name ? name : ""));
            JS_SetPropertyStr(ctx, obj, "direction", JS_NewString(ctx, type == GST_URI_SRC ? "src" : "sink"));
            JS_SetPropertyStr(ctx, obj, "protocols", protocols);
            JS_SetPropertyUint32(ctx, array, index++, obj);
        } else {
            JS_FreeValue(ctx, protocols);
        }
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
