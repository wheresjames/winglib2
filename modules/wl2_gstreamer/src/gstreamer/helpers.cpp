#include "internal.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>

#if WL2_GSTREAMER_HAVE_PBUTILS
#include <gst/pbutils/pbutils.h>
#endif

#if WL2_HAVE_QUICKJS

namespace wl2_gstreamer {

namespace {

std::string launch_quote(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            out += '\\';
        }
        out += ch;
    }
    out += '"';
    return out;
}

JSValue parse_pipeline(JSContext* ctx, const char* operation, const std::string& description) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, operation);
    }
    GError* error = nullptr;
    GstElement* element = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string message = error->message ? error->message : "gst_parse_launch failed";
        g_error_free(error);
        if (element) {
            gst_object_unref(element);
        }
        return throw_gst_error(ctx, "gstreamer_parse_failed", operation, message);
    }
    if (!element || !GST_IS_PIPELINE(element)) {
        if (element) {
            gst_object_unref(element);
        }
        return throw_gst_error(ctx, "gstreamer_parse_failed", operation, "Launch description did not produce a pipeline");
    }
    return new_pipeline_object(ctx, element);
}

JSValue attach_call(JSContext* ctx, JSValueConst pipeline, JSValue options,
    JSValue (*fn)(JSContext*, JSValueConst, int, JSValueConst*)) {
    JSValueConst argv[] = {options};
    JSValue result = fn(ctx, pipeline, 1, argv);
    JS_FreeValue(ctx, options);
    return result;
}

void set_str(JSContext* ctx, JSValue obj, const char* key, const std::string& value) {
    JS_SetPropertyStr(ctx, obj, key, JS_NewString(ctx, value.c_str()));
}

void set_i64(JSContext* ctx, JSValue obj, const char* key, int64_t value) {
    JS_SetPropertyStr(ctx, obj, key, JS_NewInt64(ctx, value));
}

bool require_string_option(JSContext* ctx, JSValueConst options, const char* key, const char* operation, std::string& out) {
    if (option_string(ctx, options, key, out) && !out.empty()) {
        return true;
    }
    throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
        std::string(operation) + " requires " + key);
    return false;
}

bool require_port_option(JSContext* ctx, JSValueConst options, const char* key, const char* operation, uint16_t& out) {
    int64_t parsed = 0;
    if (!option_int(ctx, options, key, parsed) || parsed < 1 || parsed > 65535) {
        throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            std::string(operation) + " requires " + key + " in range 1..65535");
        return false;
    }
    out = static_cast<uint16_t>(parsed);
    return true;
}

bool authorize_read_path(JSContext* ctx, const char* operation, std::string& path) {
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation, "Runtime is unavailable");
        return false;
    }
    auto resolved = runtime->resolveFilesystemReadPath(std::filesystem::path(path));
    if (!resolved) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation,
            "filesystem read is not permitted for this path");
        return false;
    }
    path = resolved->string();
    return true;
}

bool authorize_connect(JSContext* ctx, const char* operation, const std::string& host, uint16_t port) {
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation, "Runtime is unavailable");
        return false;
    }
    if (auto ok = runtime->authorizeNetworkConnect(host, port); !ok) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation, ok.error().message());
        return false;
    }
    return true;
}

bool authorize_listen(JSContext* ctx, const char* operation, const std::string& host, uint16_t port) {
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation, "Runtime is unavailable");
        return false;
    }
    if (auto ok = runtime->authorizeNetworkListen(host, port); !ok) {
        throw_gst_error(ctx, "gstreamer_permission_denied", operation, ok.error().message());
        return false;
    }
    return true;
}

bool parse_rtsp_endpoint(const std::string& uri, std::string& host, uint16_t& port) {
    const std::string prefix = "rtsp://";
    if (uri.rfind(prefix, 0) != 0) {
        return false;
    }
    size_t hostStart = prefix.size();
    size_t at = uri.find('@', hostStart);
    if (at != std::string::npos) {
        hostStart = at + 1;
    }
    size_t hostEnd = uri.find_first_of(":/?#", hostStart);
    if (hostEnd == hostStart || hostEnd == std::string::npos) {
        if (hostEnd == hostStart) {
            return false;
        }
        hostEnd = uri.size();
    }
    host = uri.substr(hostStart, hostEnd - hostStart);
    port = 554;
    if (hostEnd < uri.size() && uri[hostEnd] == ':') {
        size_t portStart = hostEnd + 1;
        size_t portEnd = uri.find_first_of("/?#", portStart);
        std::string portText = uri.substr(portStart, portEnd == std::string::npos ? std::string::npos : portEnd - portStart);
        if (portText.empty()) {
            return false;
        }
        int value = 0;
        for (char ch : portText) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            value = value * 10 + (ch - '0');
            if (value > 65535) {
                return false;
            }
        }
        if (value < 1) {
            return false;
        }
        port = static_cast<uint16_t>(value);
    }
    return true;
}

bool element_exists(const char* name) {
    if (!ensure_gst_init()) {
        return false;
    }
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

#if WL2_GSTREAMER_HAVE_PBUTILS
void set_caps(JSContext* ctx, JSValue obj, const char* key, GstCaps* caps) {
    if (!caps) {
        return;
    }
    gchar* capsText = gst_caps_to_string(caps);
    JS_SetPropertyStr(ctx, obj, key, JS_NewString(ctx, capsText ? capsText : ""));
    if (capsText) {
        g_free(capsText);
    }
    gst_caps_unref(caps);
}

void set_tags(JSContext* ctx, JSValue obj, const char* key, const GstTagList* tags) {
    if (!tags) {
        return;
    }
    gchar* tagsText = gst_tag_list_to_string(tags);
    JS_SetPropertyStr(ctx, obj, key, JS_NewString(ctx, tagsText ? tagsText : ""));
    if (tagsText) {
        g_free(tagsText);
    }
}

JSValue stream_to_js(JSContext* ctx, GstDiscovererStreamInfo* stream) {
    JSValue obj = JS_NewObject(ctx);
    const char* type = gst_discoverer_stream_info_get_stream_type_nick(stream);
    const char* id = gst_discoverer_stream_info_get_stream_id(stream);
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, type ? type : ""));
    JS_SetPropertyStr(ctx, obj, "streamId", JS_NewString(ctx, id ? id : ""));
    JS_SetPropertyStr(ctx, obj, "streamNumber",
        JS_NewInt32(ctx, gst_discoverer_stream_info_get_stream_number(stream)));
    set_caps(ctx, obj, "caps", gst_discoverer_stream_info_get_caps(stream));
    set_tags(ctx, obj, "tags", gst_discoverer_stream_info_get_tags(stream));

    if (GST_IS_DISCOVERER_VIDEO_INFO(stream)) {
        auto* video = GST_DISCOVERER_VIDEO_INFO(stream);
        JS_SetPropertyStr(ctx, obj, "width",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_video_info_get_width(video))));
        JS_SetPropertyStr(ctx, obj, "height",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_video_info_get_height(video))));
        JS_SetPropertyStr(ctx, obj, "framerateNum",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_video_info_get_framerate_num(video))));
        JS_SetPropertyStr(ctx, obj, "framerateDenom",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_video_info_get_framerate_denom(video))));
        JS_SetPropertyStr(ctx, obj, "interlaced",
            JS_NewBool(ctx, gst_discoverer_video_info_is_interlaced(video)));
    } else if (GST_IS_DISCOVERER_AUDIO_INFO(stream)) {
        auto* audio = GST_DISCOVERER_AUDIO_INFO(stream);
        JS_SetPropertyStr(ctx, obj, "channels",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_audio_info_get_channels(audio))));
        JS_SetPropertyStr(ctx, obj, "sampleRate",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_audio_info_get_sample_rate(audio))));
        JS_SetPropertyStr(ctx, obj, "depth",
            JS_NewInt32(ctx, static_cast<int32_t>(gst_discoverer_audio_info_get_depth(audio))));
        const char* language = gst_discoverer_audio_info_get_language(audio);
        if (language) {
            JS_SetPropertyStr(ctx, obj, "language", JS_NewString(ctx, language));
        }
    } else if (GST_IS_DISCOVERER_SUBTITLE_INFO(stream)) {
        const char* language = gst_discoverer_subtitle_info_get_language(GST_DISCOVERER_SUBTITLE_INFO(stream));
        if (language) {
            JS_SetPropertyStr(ctx, obj, "language", JS_NewString(ctx, language));
        }
    }

    return obj;
}
#endif

JSValue device_to_js(JSContext* ctx, GstDevice* device) {
    JSValue obj = JS_NewObject(ctx);
    gchar* display = gst_device_get_display_name(device);
    JS_SetPropertyStr(ctx, obj, "displayName", JS_NewString(ctx, display ? display : ""));
    if (display) {
        g_free(display);
    }
    const char* klass = gst_device_get_device_class(device);
    JS_SetPropertyStr(ctx, obj, "class", JS_NewString(ctx, klass ? klass : ""));
    GstCaps* caps = gst_device_get_caps(device);
    if (caps) {
        gchar* capsText = gst_caps_to_string(caps);
        JS_SetPropertyStr(ctx, obj, "caps", JS_NewString(ctx, capsText ? capsText : ""));
        if (capsText) {
            g_free(capsText);
        }
        gst_caps_unref(caps);
    }
    GstStructure* props = gst_device_get_properties(device);
    if (props) {
        // The v4l2 device node lives under different property keys depending on
        // which provider enumerated the device: the native v4l2 provider uses
        // "device.path", while the PipeWire provider uses "api.v4l2.path" (and
        // "object.path" as "v4l2:/dev/videoN"). Fall back through them so path
        // is populated regardless of the enumerating provider.
        const char* path = gst_structure_get_string(props, "device.path");
        if (!path) {
            path = gst_structure_get_string(props, "api.v4l2.path");
        }
        const char* objectPath = nullptr;
        if (!path) {
            objectPath = gst_structure_get_string(props, "object.path");
            if (objectPath && g_str_has_prefix(objectPath, "v4l2:")) {
                path = objectPath + 5;  // skip the "v4l2:" scheme prefix
            }
        }
        if (path) {
            JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, path));
        }
        gchar* propsText = gst_structure_to_string(props);
        JS_SetPropertyStr(ctx, obj, "properties", JS_NewString(ctx, propsText ? propsText : ""));
        if (propsText) {
            g_free(propsText);
        }
        gst_structure_free(props);
    }
    return obj;
}

JSValue attach_packet_sink_to_pipeline(JSContext* ctx, JSValueConst pipeline, JSValueConst options,
    const std::string& bufferName, const std::string& caps) {
    JSValue sinkOptions = JS_NewObject(ctx);
    set_str(ctx, sinkOptions, "packetBufferName", bufferName);
    JS_SetPropertyStr(ctx, sinkOptions, "create", JS_NewBool(ctx, true));
    set_str(ctx, sinkOptions, "caps", caps);
    int64_t buffers = 32;
    int64_t arenaSize = 1048576;
    int64_t maxRecord = 262144;
    option_int(ctx, options, "buffers", buffers);
    option_int(ctx, options, "arenaSize", arenaSize);
    option_int(ctx, options, "maxRecord", maxRecord);
    set_i64(ctx, sinkOptions, "buffers", buffers);
    set_i64(ctx, sinkOptions, "arenaSize", arenaSize);
    set_i64(ctx, sinkOptions, "maxRecord", maxRecord);
    return attach_call(ctx, pipeline, sinkOptions, pipeline_attach_packet_sink);
}

JSValue attach_packet_source_to_pipeline(JSContext* ctx, JSValueConst pipeline,
    const std::string& bufferName, const std::string& caps) {
    JSValue sourceOptions = JS_NewObject(ctx);
    set_str(ctx, sourceOptions, "packetBufferName", bufferName);
    set_str(ctx, sourceOptions, "caps", caps);
    return attach_call(ctx, pipeline, sourceOptions, pipeline_attach_packet_source);
}

JSValue network_packet_source(JSContext* ctx, JSValueConst options, const char* operation,
    const std::string& sourceLaunch, const std::string& caps, const std::string& packetBufferName) {
#if WL2_GSTREAMER_HAVE_APP
    std::ostringstream launch;
    launch << sourceLaunch << " ! appsink name=wl2_packet_sink sync=false";
    JSValue pipeline = parse_pipeline(ctx, operation, launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue attached = attach_packet_sink_to_pipeline(ctx, pipeline, options, packetBufferName, caps);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)ctx;
    (void)options;
    (void)operation;
    (void)sourceLaunch;
    (void)caps;
    (void)packetBufferName;
    return JS_UNDEFINED;
#endif
}

JSValue network_packet_sink(JSContext* ctx, const char* operation,
    const std::string& sinkLaunch, const std::string& caps, const std::string& packetBufferName) {
#if WL2_GSTREAMER_HAVE_APP
    std::ostringstream launch;
    launch << "appsrc name=wl2_packet_src is-live=true format=time caps=" << caps
           << " ! " << sinkLaunch;
    JSValue pipeline = parse_pipeline(ctx, operation, launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue attached = attach_packet_source_to_pipeline(ctx, pipeline, packetBufferName, caps);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)ctx;
    (void)operation;
    (void)sinkLaunch;
    (void)caps;
    (void)packetBufferName;
    return JS_UNDEFINED;
#endif
}

} // namespace

JSValue gst_file_playback_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "filePlayback",
            "filePlayback(options) requires an options object");
    }
    std::string path;
    if (!require_string_option(ctx, argv[0], "path", "filePlayback", path)) {
        return JS_EXCEPTION;
    }
    if (!authorize_read_path(ctx, "filePlayback", path)) {
        return JS_EXCEPTION;
    }
    std::string videoName;
    if (!require_string_option(ctx, argv[0], "videoBufferName", "filePlayback", videoName)) {
        return JS_EXCEPTION;
    }
    int64_t width = 320;
    int64_t height = 240;
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
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "filePlayback", "Unsupported video format: " + formatName);
    }

    std::string audioName;
    option_string(ctx, argv[0], "audioBufferName", audioName);

    std::ostringstream launch;
    launch << "filesrc location=" << launch_quote(path) << " ! decodebin name=dec "
           << "dec. ! queue ! videoconvert ! videoscale ! video/x-raw,format=" << formatName
           << ",width=" << width << ",height=" << height << ",framerate=" << fps
           << "/1 ! appsink name=wl2_video_sink sync=false";
    if (!audioName.empty()) {
        int64_t sampleRate = 48000;
        int64_t channels = 2;
        int64_t audioFps = 50;
        int64_t audioBuffers = 16;
        option_int(ctx, argv[0], "sampleRate", sampleRate);
        option_int(ctx, argv[0], "channels", channels);
        option_int(ctx, argv[0], "audioFps", audioFps);
        option_int(ctx, argv[0], "audioBuffers", audioBuffers);
        launch << " dec. ! queue ! audioconvert ! audioresample ! audio/x-raw,format=S16LE,layout=interleaved,rate="
               << sampleRate << ",channels=" << channels << " ! appsink name=wl2_audio_sink sync=false";
    }

    JSValue pipeline = parse_pipeline(ctx, "filePlayback", launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }

    JSValue videoOptions = JS_NewObject(ctx);
    set_str(ctx, videoOptions, "videoBufferName", videoName);
    JS_SetPropertyStr(ctx, videoOptions, "create", JS_NewBool(ctx, true));
    set_i64(ctx, videoOptions, "width", width);
    set_i64(ctx, videoOptions, "height", height);
    set_i64(ctx, videoOptions, "fps", fps);
    set_i64(ctx, videoOptions, "buffers", buffers);
    set_str(ctx, videoOptions, "format", formatName);
    JSValue attached = attach_call(ctx, pipeline, videoOptions, pipeline_attach_video_sink);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);

    if (!audioName.empty()) {
        JSValue audioOptions = JS_NewObject(ctx);
        set_str(ctx, audioOptions, "audioBufferName", audioName);
        JS_SetPropertyStr(ctx, audioOptions, "create", JS_NewBool(ctx, true));
        JSValue audioAttached = attach_call(ctx, pipeline, audioOptions, pipeline_attach_audio_sink);
        if (JS_IsException(audioAttached)) {
            JS_FreeValue(ctx, pipeline);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, audioAttached);
    }
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "filePlayback",
        "This build lacks gstreamer-app-1.0; file playback helpers are unavailable");
#endif
}

JSValue gst_record_video_buffer_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "recordVideoBuffer",
            "recordVideoBuffer(options) requires an options object");
    }
    std::string bufferName;
    std::string outputPath;
    if (!require_string_option(ctx, argv[0], "videoBufferName", "recordVideoBuffer", bufferName)
        || !require_string_option(ctx, argv[0], "outputPath", "recordVideoBuffer", outputPath)) {
        return JS_EXCEPTION;
    }
    std::string encoder = "vp8enc deadline=1";
    std::string muxer = "webmmux";
    option_string(ctx, argv[0], "encoder", encoder);
    option_string(ctx, argv[0], "muxer", muxer);

    std::ostringstream launch;
    launch << "appsrc name=wl2_video_src is-live=true format=time ! videoconvert ! "
           << encoder << " ! " << muxer << " ! filesink location=" << launch_quote(outputPath);
    JSValue pipeline = parse_pipeline(ctx, "recordVideoBuffer", launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue sourceOptions = JS_NewObject(ctx);
    set_str(ctx, sourceOptions, "videoBufferName", bufferName);
    JSValue attached = attach_call(ctx, pipeline, sourceOptions, pipeline_attach_video_source);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "recordVideoBuffer",
        "This build lacks gstreamer-app-1.0; record helpers are unavailable");
#endif
}

JSValue gst_record_packet_buffer_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "recordPacketBuffer",
            "recordPacketBuffer(options) requires an options object");
    }
    std::string bufferName;
    std::string outputPath;
    if (!require_string_option(ctx, argv[0], "packetBufferName", "recordPacketBuffer", bufferName)
        || !require_string_option(ctx, argv[0], "outputPath", "recordPacketBuffer", outputPath)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/octet-stream";
    std::string muxer;
    option_string(ctx, argv[0], "caps", caps);
    option_string(ctx, argv[0], "muxer", muxer);
    std::ostringstream launch;
    launch << "appsrc name=wl2_packet_src is-live=true format=time caps=" << caps;
    if (!muxer.empty()) {
        launch << " ! " << muxer;
    }
    launch << " ! filesink location=" << launch_quote(outputPath);
    JSValue pipeline = parse_pipeline(ctx, "recordPacketBuffer", launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue sourceOptions = JS_NewObject(ctx);
    set_str(ctx, sourceOptions, "packetBufferName", bufferName);
    set_str(ctx, sourceOptions, "caps", caps);
    JSValue attached = attach_call(ctx, pipeline, sourceOptions, pipeline_attach_packet_source);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "recordPacketBuffer",
        "This build lacks gstreamer-app-1.0; packet record helpers are unavailable");
#endif
}

JSValue gst_discover_media_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_PBUTILS
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "discoverMedia",
            "discoverMedia(options) requires an options object");
    }
    std::string path;
    if (!require_string_option(ctx, argv[0], "path", "discoverMedia", path)) {
        return JS_EXCEPTION;
    }
    if (!authorize_read_path(ctx, "discoverMedia", path)) {
        return JS_EXCEPTION;
    }
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "discoverMedia");
    }
    GError* error = nullptr;
    GstDiscoverer* discoverer = gst_discoverer_new(5 * GST_SECOND, &error);
    if (!discoverer) {
        std::string message = error && error->message ? error->message : "Could not create GstDiscoverer";
        if (error) {
            g_error_free(error);
        }
        return throw_gst_error(ctx, "gstreamer_unsupported", "discoverMedia", message);
    }
    gchar* uri = gst_filename_to_uri(path.c_str(), &error);
    if (!uri) {
        std::string message = error && error->message ? error->message : "Could not convert path to URI";
        if (error) {
            g_error_free(error);
        }
        g_object_unref(discoverer);
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "discoverMedia", message);
    }
    GstDiscovererInfo* info = gst_discoverer_discover_uri(discoverer, uri, &error);
    g_free(uri);
    if (!info) {
        std::string message = error && error->message ? error->message : "GStreamer discovery failed";
        if (error) {
            g_error_free(error);
        }
        g_object_unref(discoverer);
        return throw_gst_error(ctx, "gstreamer_discovery_failed", "discoverMedia", message);
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, static_cast<int64_t>(gst_discoverer_info_get_duration(info))));
    JS_SetPropertyStr(ctx, obj, "seekable", JS_NewBool(ctx, gst_discoverer_info_get_seekable(info)));
    JS_SetPropertyStr(ctx, obj, "live", JS_NewBool(ctx, gst_discoverer_info_get_live(info)));
    set_tags(ctx, obj, "tags", gst_discoverer_info_get_tags(info));

    JSValue streamsArray = JS_NewArray(ctx);
    uint32_t index = 0;
    GList* streams = gst_discoverer_info_get_stream_list(info);
    for (GList* item = streams; item; item = item->next) {
        JS_SetPropertyUint32(ctx, streamsArray, index++,
            stream_to_js(ctx, GST_DISCOVERER_STREAM_INFO(item->data)));
    }
    gst_discoverer_stream_info_list_free(streams);
    JS_SetPropertyStr(ctx, obj, "streams", streamsArray);

    GstDiscovererStreamInfo* root = gst_discoverer_info_get_stream_info(info);
    if (root) {
        set_caps(ctx, obj, "caps", gst_discoverer_stream_info_get_caps(root));
        gst_discoverer_stream_info_unref(root);
    }

    gst_discoverer_info_unref(info);
    g_object_unref(discoverer);
    return obj;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "discoverMedia",
        "This build lacks gstreamer-pbutils-1.0; media discovery is unavailable");
#endif
}

JSValue gst_device_monitor_create_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "DeviceMonitor.create");
    }
    std::string classes;
    if (argc > 0) {
        option_string(ctx, argv[0], "classes", classes);
    }
    GstDeviceMonitor* monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, classes.empty() ? nullptr : classes.c_str(), nullptr);
    gboolean started = gst_device_monitor_start(monitor);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, started == TRUE));
    JSValue devicesArray = JS_NewArray(ctx);
    guint32 index = 0;
    if (started) {
        GList* devices = gst_device_monitor_get_devices(monitor);
        for (GList* item = devices; item; item = item->next) {
            JS_SetPropertyUint32(ctx, devicesArray, index++, device_to_js(ctx, GST_DEVICE(item->data)));
        }
        g_list_free_full(devices, gst_object_unref);
        gst_device_monitor_stop(monitor);
    }
    JS_SetPropertyStr(ctx, obj, "devices", devicesArray);
    g_object_unref(monitor);
    return obj;
}

JSValue gst_capture_device_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "captureDevice",
            "captureDevice(options) requires an options object");
    }
    std::string bufferName;
    if (!require_string_option(ctx, argv[0], "videoBufferName", "captureDevice", bufferName)) {
        return JS_EXCEPTION;
    }
    std::string device;
    option_string(ctx, argv[0], "device", device);
    int64_t width = 640;
    int64_t height = 480;
    int64_t fps = 30;
    int64_t buffers = 4;
    option_int(ctx, argv[0], "width", width);
    option_int(ctx, argv[0], "height", height);
    option_int(ctx, argv[0], "fps", fps);
    option_int(ctx, argv[0], "buffers", buffers);
    std::string source = device.empty() ? "videotestsrc is-live=true" : "v4l2src device=" + launch_quote(device);
    if (!device.empty() && !element_exists("v4l2src")) {
        return throw_gst_error(ctx, "gstreamer_unsupported", "captureDevice", "v4l2src is not installed");
    }
    std::ostringstream launch;
    launch << source << " ! videoconvert ! video/x-raw,format=RGBA,width=" << width
           << ",height=" << height << ",framerate=" << fps << "/1 ! appsink name=wl2_video_sink sync=false";
    JSValue pipeline = parse_pipeline(ctx, "captureDevice", launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue sinkOptions = JS_NewObject(ctx);
    set_str(ctx, sinkOptions, "videoBufferName", bufferName);
    JS_SetPropertyStr(ctx, sinkOptions, "create", JS_NewBool(ctx, true));
    set_i64(ctx, sinkOptions, "width", width);
    set_i64(ctx, sinkOptions, "height", height);
    set_i64(ctx, sinkOptions, "fps", fps);
    set_i64(ctx, sinkOptions, "buffers", buffers);
    JSValue attached = attach_call(ctx, pipeline, sinkOptions, pipeline_attach_video_sink);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "captureDevice",
        "This build lacks gstreamer-app-1.0; capture helpers are unavailable");
#endif
}

// teeVideoBuffer(options): a multi-sink helper. One source feeds a tee that
// publishes RGBA frames into a VideoBuffer and, when outputPath is set, records
// an encoded copy to a file at the same time.
JSValue gst_tee_video_buffer_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "teeVideoBuffer",
            "teeVideoBuffer(options) requires an options object");
    }
    std::string bufferName;
    if (!require_string_option(ctx, argv[0], "videoBufferName", "teeVideoBuffer", bufferName)) {
        return JS_EXCEPTION;
    }
    std::string source = "videotestsrc is-live=true pattern=ball";
    option_string(ctx, argv[0], "source", source);
    int64_t width = 640;
    int64_t height = 480;
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
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "teeVideoBuffer",
            "Unsupported video format: " + formatName);
    }
    std::string outputPath;
    option_string(ctx, argv[0], "outputPath", outputPath);
    std::string encoder = "vp8enc deadline=1";
    std::string muxer = "webmmux";
    option_string(ctx, argv[0], "encoder", encoder);
    option_string(ctx, argv[0], "muxer", muxer);

    std::ostringstream launch;
    launch << source << " ! tee name=wl2_tee"
           << " wl2_tee. ! queue ! videoconvert ! video/x-raw,format=" << formatName
           << ",width=" << width << ",height=" << height << ",framerate=" << fps
           << "/1 ! appsink name=wl2_video_sink sync=false";
    if (!outputPath.empty()) {
        launch << " wl2_tee. ! queue ! videoconvert ! " << encoder << " ! " << muxer
               << " ! filesink location=" << launch_quote(outputPath);
    }
    JSValue pipeline = parse_pipeline(ctx, "teeVideoBuffer", launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    auto* box = static_cast<PipelineBox*>(JS_GetOpaque(pipeline, g_pipelineClassId));
    JSValue attached = attach_video_sink_core(ctx, box, "teeVideoBuffer", "wl2_video_sink",
        bufferName, true, width, height, fps, buffers, *pixel);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "teeVideoBuffer",
        "This build lacks gstreamer-app-1.0; tee helpers are unavailable");
#endif
}

// overlayVideoBuffer(options): capture a source through a named textoverlay into
// a VideoBuffer. Use pipeline.setOverlayText({ text }) to update the overlay
// live, typically driven by low-rate records read from a SharedQueue.
JSValue gst_overlay_video_buffer_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "overlayVideoBuffer",
            "overlayVideoBuffer(options) requires an options object");
    }
    std::string bufferName;
    if (!require_string_option(ctx, argv[0], "videoBufferName", "overlayVideoBuffer", bufferName)) {
        return JS_EXCEPTION;
    }
    if (!element_exists("textoverlay")) {
        return throw_gst_error(ctx, "gstreamer_unsupported", "overlayVideoBuffer",
            "textoverlay element is not installed");
    }
    std::string source = "videotestsrc is-live=true";
    option_string(ctx, argv[0], "source", source);
    std::string text;
    option_string(ctx, argv[0], "text", text);
    int64_t width = 640;
    int64_t height = 480;
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
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "overlayVideoBuffer",
            "Unsupported video format: " + formatName);
    }

    std::ostringstream launch;
    launch << source << " ! videoconvert ! textoverlay name=wl2_overlay ! videoconvert"
           << " ! video/x-raw,format=" << formatName << ",width=" << width << ",height=" << height
           << ",framerate=" << fps << "/1 ! appsink name=wl2_video_sink sync=false";
    JSValue pipeline = parse_pipeline(ctx, "overlayVideoBuffer", launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    auto* box = static_cast<PipelineBox*>(JS_GetOpaque(pipeline, g_pipelineClassId));
    // Seed the initial overlay text directly on the element (avoids launch-string
    // quoting); later updates go through pipeline.setOverlayText().
    if (!text.empty() && box && box->pipeline) {
        GstElement* overlay = gst_bin_get_by_name(GST_BIN(box->pipeline), "wl2_overlay");
        if (overlay) {
            g_object_set(G_OBJECT(overlay), "text", text.c_str(), nullptr);
            gst_object_unref(overlay);
        }
    }
    JSValue attached = attach_video_sink_core(ctx, box, "overlayVideoBuffer", "wl2_video_sink",
        bufferName, true, width, height, fps, buffers, *pixel);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "overlayVideoBuffer",
        "This build lacks gstreamer-app-1.0; overlay helpers are unavailable");
#endif
}

JSValue gst_send_udp_packets_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "sendUdpPackets";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "sendUdpPackets(options) requires an options object");
    }
    std::string bufferName;
    std::string host;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "packetBufferName", operation, bufferName)
        || !require_string_option(ctx, argv[0], "host", operation, host)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    if (!authorize_connect(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/octet-stream";
    option_string(ctx, argv[0], "caps", caps);
    std::ostringstream sink;
    sink << "udpsink host=" << launch_quote(host) << " port=" << port << " sync=false async=false";
    return network_packet_sink(ctx, operation, sink.str(), caps, bufferName);
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "sendUdpPackets",
        "This build lacks gstreamer-app-1.0; network helpers are unavailable");
#endif
}

JSValue gst_receive_udp_packets_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "receiveUdpPackets";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "receiveUdpPackets(options) requires an options object");
    }
    std::string bufferName;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "packetBufferName", operation, bufferName)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    std::string host = "127.0.0.1";
    option_string(ctx, argv[0], "host", host);
    if (!authorize_listen(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/octet-stream";
    option_string(ctx, argv[0], "caps", caps);
    std::ostringstream source;
    source << "udpsrc address=" << launch_quote(host) << " port=" << port << " caps=" << caps;
    return network_packet_source(ctx, argv[0], operation, source.str(), caps, bufferName);
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "receiveUdpPackets",
        "This build lacks gstreamer-app-1.0; network helpers are unavailable");
#endif
}

JSValue gst_send_rtp_packets_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "sendRtpPackets";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "sendRtpPackets(options) requires an options object");
    }
    std::string bufferName;
    std::string host;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "packetBufferName", operation, bufferName)
        || !require_string_option(ctx, argv[0], "host", operation, host)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    if (!authorize_connect(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/x-rtp,media=video,encoding-name=H264,payload=96";
    option_string(ctx, argv[0], "caps", caps);
    std::ostringstream sink;
    sink << "udpsink host=" << launch_quote(host) << " port=" << port << " sync=false async=false";
    return network_packet_sink(ctx, operation, sink.str(), caps, bufferName);
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "sendRtpPackets",
        "This build lacks gstreamer-app-1.0; RTP helpers are unavailable");
#endif
}

JSValue gst_receive_rtp_packets_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "receiveRtpPackets";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "receiveRtpPackets(options) requires an options object");
    }
    std::string bufferName;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "packetBufferName", operation, bufferName)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    std::string host = "127.0.0.1";
    option_string(ctx, argv[0], "host", host);
    if (!authorize_listen(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/x-rtp,media=video,encoding-name=H264,payload=96";
    std::string depay;
    option_string(ctx, argv[0], "caps", caps);
    option_string(ctx, argv[0], "depay", depay);
    std::ostringstream source;
    source << "udpsrc address=" << launch_quote(host) << " port=" << port << " caps=" << caps;
    if (!depay.empty()) {
        source << " ! " << depay;
    }
    return network_packet_source(ctx, argv[0], operation, source.str(), caps, bufferName);
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "receiveRtpPackets",
        "This build lacks gstreamer-app-1.0; RTP helpers are unavailable");
#endif
}

JSValue gst_send_tcp_packets_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "sendTcpPackets";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "sendTcpPackets(options) requires an options object");
    }
    std::string bufferName;
    std::string host;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "packetBufferName", operation, bufferName)
        || !require_string_option(ctx, argv[0], "host", operation, host)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    if (!authorize_connect(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/octet-stream";
    option_string(ctx, argv[0], "caps", caps);
    std::ostringstream sink;
    sink << "tcpclientsink host=" << launch_quote(host) << " port=" << port << " sync=false";
    return network_packet_sink(ctx, operation, sink.str(), caps, bufferName);
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "sendTcpPackets",
        "This build lacks gstreamer-app-1.0; TCP helpers are unavailable");
#endif
}

JSValue gst_receive_tcp_packets_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "receiveTcpPackets";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "receiveTcpPackets(options) requires an options object");
    }
    std::string bufferName;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "packetBufferName", operation, bufferName)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    std::string host = "127.0.0.1";
    option_string(ctx, argv[0], "host", host);
    if (!authorize_listen(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string caps = "application/octet-stream";
    option_string(ctx, argv[0], "caps", caps);
    std::ostringstream source;
    source << "tcpserversrc host=" << launch_quote(host) << " port=" << port;
    return network_packet_source(ctx, argv[0], operation, source.str(), caps, bufferName);
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "receiveTcpPackets",
        "This build lacks gstreamer-app-1.0; TCP helpers are unavailable");
#endif
}

JSValue gst_stream_video_udp_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "streamVideoUdp";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "streamVideoUdp(options) requires an options object");
    }
    std::string bufferName;
    std::string host;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "videoBufferName", operation, bufferName)
        || !require_string_option(ctx, argv[0], "host", operation, host)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    if (!authorize_connect(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string encoder = "vp8enc deadline=1";
    std::string payloader = "rtpvp8pay pt=96";
    option_string(ctx, argv[0], "encoder", encoder);
    option_string(ctx, argv[0], "payloader", payloader);
    std::ostringstream launch;
    launch << "appsrc name=wl2_video_src is-live=true format=time ! videoconvert ! "
           << encoder << " ! " << payloader << " ! udpsink host=" << launch_quote(host)
           << " port=" << port << " sync=false async=false";
    JSValue pipeline = parse_pipeline(ctx, operation, launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue sourceOptions = JS_NewObject(ctx);
    set_str(ctx, sourceOptions, "videoBufferName", bufferName);
    JSValue attached = attach_call(ctx, pipeline, sourceOptions, pipeline_attach_video_source);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "streamVideoUdp",
        "This build lacks gstreamer-app-1.0; video streaming helpers are unavailable");
#endif
}

JSValue gst_stream_video_tcp_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "streamVideoTcp";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "streamVideoTcp(options) requires an options object");
    }
    std::string bufferName;
    std::string host;
    uint16_t port = 0;
    if (!require_string_option(ctx, argv[0], "videoBufferName", operation, bufferName)
        || !require_string_option(ctx, argv[0], "host", operation, host)
        || !require_port_option(ctx, argv[0], "port", operation, port)) {
        return JS_EXCEPTION;
    }
    if (!authorize_connect(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    std::string encoder = "vp8enc deadline=1";
    std::string muxer = "webmmux streamable=true";
    option_string(ctx, argv[0], "encoder", encoder);
    option_string(ctx, argv[0], "muxer", muxer);
    std::ostringstream launch;
    launch << "appsrc name=wl2_video_src is-live=true format=time ! videoconvert ! "
           << encoder << " ! " << muxer << " ! tcpclientsink host=" << launch_quote(host)
           << " port=" << port << " sync=false";
    JSValue pipeline = parse_pipeline(ctx, operation, launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue sourceOptions = JS_NewObject(ctx);
    set_str(ctx, sourceOptions, "videoBufferName", bufferName);
    JSValue attached = attach_call(ctx, pipeline, sourceOptions, pipeline_attach_video_source);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "streamVideoTcp",
        "This build lacks gstreamer-app-1.0; video streaming helpers are unavailable");
#endif
}

JSValue gst_rtsp_playback_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_GSTREAMER_HAVE_APP
    const char* operation = "rtspPlayback";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "rtspPlayback(options) requires an options object");
    }
    std::string uri;
    std::string bufferName;
    if (!require_string_option(ctx, argv[0], "uri", operation, uri)
        || !require_string_option(ctx, argv[0], "videoBufferName", operation, bufferName)) {
        return JS_EXCEPTION;
    }
    std::string host;
    uint16_t port = 0;
    if (!parse_rtsp_endpoint(uri, host, port)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation,
            "rtspPlayback requires an rtsp:// URI with a valid host and port");
    }
    if (!authorize_connect(ctx, operation, host, port)) {
        return JS_EXCEPTION;
    }
    if (!element_exists("rtspsrc")) {
        return throw_gst_error(ctx, "gstreamer_unsupported", operation, "rtspsrc is not installed");
    }
    int64_t width = 320;
    int64_t height = 240;
    int64_t fps = 30;
    int64_t buffers = 4;
    int64_t latency = 200;
    option_int(ctx, argv[0], "width", width);
    option_int(ctx, argv[0], "height", height);
    option_int(ctx, argv[0], "fps", fps);
    option_int(ctx, argv[0], "buffers", buffers);
    option_int(ctx, argv[0], "latency", latency);
    std::ostringstream launch;
    launch << "rtspsrc location=" << launch_quote(uri) << " latency=" << latency
           << " ! decodebin ! videoconvert ! videoscale ! video/x-raw,format=RGBA,width="
           << width << ",height=" << height << ",framerate=" << fps
           << "/1 ! appsink name=wl2_video_sink sync=false";
    JSValue pipeline = parse_pipeline(ctx, operation, launch.str());
    if (JS_IsException(pipeline)) {
        return pipeline;
    }
    JSValue sinkOptions = JS_NewObject(ctx);
    set_str(ctx, sinkOptions, "videoBufferName", bufferName);
    JS_SetPropertyStr(ctx, sinkOptions, "create", JS_NewBool(ctx, true));
    set_i64(ctx, sinkOptions, "width", width);
    set_i64(ctx, sinkOptions, "height", height);
    set_i64(ctx, sinkOptions, "fps", fps);
    set_i64(ctx, sinkOptions, "buffers", buffers);
    set_str(ctx, sinkOptions, "format", "RGBA");
    JSValue attached = attach_call(ctx, pipeline, sinkOptions, pipeline_attach_video_sink);
    if (JS_IsException(attached)) {
        JS_FreeValue(ctx, pipeline);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, attached);
    return pipeline;
#else
    (void)argc;
    (void)argv;
    return throw_gst_error(ctx, "gstreamer_unsupported", "rtspPlayback",
        "This build lacks gstreamer-app-1.0; RTSP helpers are unavailable");
#endif
}

namespace {

std::vector<std::string> required_elements_for_uri_text(const std::string& uri) {
    std::vector<std::string> out;
    auto starts = [&](const char* prefix) { return uri.rfind(prefix, 0) == 0; };
    if (starts("rtsp://") || starts("rtsps://")) {
        out.push_back("rtspsrc");
    } else if (starts("srt://")) {
        out.push_back("srtsrc");
    } else if (starts("rist://")) {
        out.push_back("ristsrc");
        out.push_back("rtpmp2tdepay");
        out.push_back("tsdemux");
    } else if (starts("rtmp://") || starts("rtmps://")) {
        out.push_back("rtmpsrc");
    } else if (starts("http://") || starts("https://")) {
        if (uri.find(".m3u8") != std::string::npos) {
            out.push_back("souphttpsrc");
            out.push_back("hlsdemux");
        } else if (uri.find(".mpd") != std::string::npos) {
            out.push_back("souphttpsrc");
            out.push_back("dashdemux");
        } else if (uri.find("mjpeg") != std::string::npos) {
            out.push_back("souphttpsrc");
            out.push_back("multipartdemux");
            out.push_back("jpegparse");
            out.push_back("jpegdec");
        } else {
            out.push_back("souphttpsrc");
        }
    } else if (starts("file://")) {
        out.push_back("filesrc");
    }
    out.push_back("decodebin");
    return out;
}

JSValue string_array(JSContext* ctx, const std::vector<std::string>& values) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& value : values) {
        JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, value.c_str()));
    }
    return array;
}

JSValue required_result(JSContext* ctx, const std::vector<std::string>& required) {
    std::vector<std::string> missing;
    for (const auto& name : required) {
        if (!element_exists(name.c_str())) {
            missing.push_back(name);
        }
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "requiredElements", string_array(ctx, required));
    JS_SetPropertyStr(ctx, obj, "missingElements", string_array(ctx, missing));
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, missing.empty()));
    return obj;
}

std::string source_option(JSContext* ctx, JSValueConst options) {
    std::string source;
    if (!option_string(ctx, options, "source", source) || source.empty()) {
        source = "videotestsrc is-live=true pattern=ball";
    }
    return source;
}

std::string h264_head(JSContext* ctx, JSValueConst options) {
    int64_t width = 640;
    int64_t height = 360;
    int64_t fps = 30;
    option_int(ctx, options, "width", width);
    option_int(ctx, options, "height", height);
    option_int(ctx, options, "fps", fps);
    std::ostringstream launch;
    launch << source_option(ctx, options)
           << " ! queue ! videoconvert ! videoscale ! videorate ! video/x-raw,width="
           << width << ",height=" << height << ",framerate=" << fps
           << "/1 ! x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 ! video/x-h264,profile=baseline";
    return launch.str();
}

JSValue launch_result(JSContext* ctx, const std::string& launch, const std::string& url,
    const std::vector<std::string>& required) {
    JSValue obj = required_result(ctx, required);
    JS_SetPropertyStr(ctx, obj, "launch", JS_NewString(ctx, launch.c_str()));
    JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url.c_str()));
    return obj;
}

} // namespace

JSValue gst_required_elements_for_uri_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "requiredElementsForUri",
            "requiredElementsForUri(uri) requires a URI string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    std::string uri = text;
    JS_FreeCString(ctx, text);
    return string_array(ctx, required_elements_for_uri_text(uri));
}

JSValue gst_can_decode_uri_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "canDecodeUri",
            "canDecodeUri(uri) requires a URI string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    std::string uri = text;
    JS_FreeCString(ctx, text);
    return required_result(ctx, required_elements_for_uri_text(uri));
}

JSValue gst_build_hls_output_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const char* operation = "buildHlsOutput";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation, "buildHlsOutput(options) requires an object");
    }
    std::string outDir;
    if (!require_string_option(ctx, argv[0], "outDir", operation, outDir)) return JS_EXCEPTION;
    std::string url;
    option_string(ctx, argv[0], "url", url);
    if (url.empty()) url = "http://127.0.0.1:8080/hls/stream.m3u8";
    int64_t segmentSeconds = 2;
    int64_t playlistLength = 5;
    int64_t maxFiles = 10;
    option_int(ctx, argv[0], "segmentSeconds", segmentSeconds);
    option_int(ctx, argv[0], "playlistLength", playlistLength);
    option_int(ctx, argv[0], "maxFiles", maxFiles);
    segmentSeconds = std::clamp<int64_t>(segmentSeconds, 1, 30);
    playlistLength = std::clamp<int64_t>(playlistLength, 2, 120);
    maxFiles = std::clamp<int64_t>(std::max(maxFiles, playlistLength), playlistLength, 240);
    std::ostringstream launch;
    launch << h264_head(ctx, argv[0])
           << " ! mpegtsmux ! hlssink location=" << launch_quote(outDir + "/seg_%05d.ts")
           << " playlist-location=" << launch_quote(outDir + "/stream.m3u8")
           << " target-duration=" << segmentSeconds
           << " max-files=" << maxFiles
           << " playlist-length=" << playlistLength;
    return launch_result(ctx, launch.str(), url, {"x264enc", "mpegtsmux", "hlssink"});
}

JSValue gst_build_dash_output_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const char* operation = "buildDashOutput";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation, "buildDashOutput(options) requires an object");
    }
    std::string outDir;
    if (!require_string_option(ctx, argv[0], "outDir", operation, outDir)) return JS_EXCEPTION;
    std::string url;
    option_string(ctx, argv[0], "url", url);
    if (url.empty()) url = "http://127.0.0.1:8080/dash/stream.mpd";
    int64_t segmentSeconds = 2;
    option_int(ctx, argv[0], "segmentSeconds", segmentSeconds);
    segmentSeconds = std::clamp<int64_t>(segmentSeconds, 1, 30);
    std::ostringstream launch;
    launch << h264_head(ctx, argv[0])
           << " ! dashsink mpd-filename=stream.mpd mpd-root-path=" << launch_quote(outDir)
           << " target-duration=" << segmentSeconds << " dynamic=true";
    return launch_result(ctx, launch.str(), url, {"x264enc", "dashsink"});
}

JSValue gst_build_srt_output_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const char* operation = "buildSrtOutput";
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", operation, "buildSrtOutput(options) requires an object");
    }
    int64_t port = 7001;
    option_int(ctx, argv[0], "port", port);
    std::string host = "0.0.0.0";
    option_string(ctx, argv[0], "host", host);
    std::string publicHost = "127.0.0.1";
    option_string(ctx, argv[0], "publicHost", publicHost);
    std::ostringstream launch;
    launch << h264_head(ctx, argv[0])
           << " ! mpegtsmux ! srtsink uri="
           << launch_quote("srt://" + host + ":" + std::to_string(port) + "?mode=listener");
    std::string url = "srt://" + publicHost + ":" + std::to_string(port);
    return launch_result(ctx, launch.str(), url, {"x264enc", "mpegtsmux", "srtsink"});
}

} // namespace wl2_gstreamer

#endif // WL2_HAVE_QUICKJS
