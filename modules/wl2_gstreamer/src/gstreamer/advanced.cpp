#include "internal.h"

#include <string>

#if WL2_HAVE_QUICKJS

namespace wl2_gstreamer {

namespace {

std::string quote_path(const std::string& value) {
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

// Convert a possibly-unset GstClockTime to a signed nanosecond value, using -1
// for GST_CLOCK_TIME_NONE so JS gets a clean "unavailable" sentinel.
int64_t clock_time_or_unset(GstClockTime value) {
    return value == GST_CLOCK_TIME_NONE ? -1 : static_cast<int64_t>(value);
}

} // namespace

// Serialize GstCaps into { text, structureCount, structures: [{ name, fields }] }.
JSValue caps_to_js(JSContext* ctx, GstCaps* caps) {
    JSValue obj = JS_NewObject(ctx);
    gchar* text = caps ? gst_caps_to_string(caps) : nullptr;
    JS_SetPropertyStr(ctx, obj, "text", JS_NewString(ctx, text ? text : ""));
    if (text) {
        g_free(text);
    }
    const bool any = caps && gst_caps_is_any(caps);
    const bool empty = caps && gst_caps_is_empty(caps);
    JS_SetPropertyStr(ctx, obj, "any", JS_NewBool(ctx, any));
    JS_SetPropertyStr(ctx, obj, "empty", JS_NewBool(ctx, empty));
    const guint count = (caps && !any && !empty) ? gst_caps_get_size(caps) : 0;
    JS_SetPropertyStr(ctx, obj, "structureCount", JS_NewInt64(ctx, static_cast<int64_t>(count)));
    JSValue array = JS_NewArray(ctx);
    for (guint i = 0; i < count; ++i) {
        GstStructure* s = gst_caps_get_structure(caps, i);
        JSValue so = JS_NewObject(ctx);
        const char* name = gst_structure_get_name(s);
        JS_SetPropertyStr(ctx, so, "name", JS_NewString(ctx, name ? name : ""));
        JSValue fields = JS_NewObject(ctx);
        const int nf = gst_structure_n_fields(s);
        for (int f = 0; f < nf; ++f) {
            const char* fname = gst_structure_nth_field_name(s, f);
            const GValue* value = fname ? gst_structure_get_value(s, fname) : nullptr;
            gchar* serialized = value ? gst_value_serialize(value) : nullptr;
            JS_SetPropertyStr(ctx, fields, fname ? fname : "",
                JS_NewString(ctx, serialized ? serialized : ""));
            if (serialized) {
                g_free(serialized);
            }
        }
        JS_SetPropertyStr(ctx, so, "fields", fields);
        JS_SetPropertyUint32(ctx, array, i, so);
    }
    JS_SetPropertyStr(ctx, obj, "structures", array);
    return obj;
}

// Caps.parse(text) -> caps object. A caps negotiation utility for scripts that
// build launch strings by hand.
JSValue gst_caps_parse_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "Caps.parse");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "Caps.parse",
            "Caps.parse(text) requires a caps string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_EXCEPTION;
    }
    GstCaps* caps = gst_caps_from_string(text);
    JS_FreeCString(ctx, text);
    if (!caps) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "Caps.parse",
            "Could not parse the caps string");
    }
    JSValue result = caps_to_js(ctx, caps);
    gst_caps_unref(caps);
    return result;
}

// pipeline.negotiatedCaps({ element, pad? }) -> the current or allowed caps on a
// named element's pad, for debugging negotiation failures.
JSValue pipeline_negotiated_caps(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "negotiatedCaps");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string element;
    if (argc < 1 || !option_string(ctx, argv[0], "element", element) || element.empty()) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "negotiatedCaps",
            "negotiatedCaps({ element }) requires an element name");
    }
    std::string padName = "src";
    option_string(ctx, argv[0], "pad", padName);

    GstElement* el = gst_bin_get_by_name(GST_BIN(box->pipeline), element.c_str());
    if (!el) {
        return throw_gst_error(ctx, "gstreamer_element_not_found", "negotiatedCaps",
            "No element named '" + element + "' in the pipeline");
    }
    GstPad* pad = gst_element_get_static_pad(el, padName.c_str());
    if (!pad) {
        gst_object_unref(el);
        return throw_gst_error(ctx, "gstreamer_element_not_found", "negotiatedCaps",
            "Element '" + element + "' has no static pad '" + padName + "'");
    }
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const bool negotiated = caps != nullptr;
    if (!caps) {
        // Fall back to the pad's allowed/template caps when nothing is negotiated
        // yet so callers can still inspect what the pad could accept.
        caps = gst_pad_query_caps(pad, nullptr);
    }
    JSValue result = caps_to_js(ctx, caps);
    JS_SetPropertyStr(ctx, result, "negotiated", JS_NewBool(ctx, negotiated));
    JS_SetPropertyStr(ctx, result, "element", JS_NewString(ctx, element.c_str()));
    JS_SetPropertyStr(ctx, result, "pad", JS_NewString(ctx, padName.c_str()));
    if (caps) {
        gst_caps_unref(caps);
    }
    gst_object_unref(pad);
    gst_object_unref(el);
    return result;
}

// pipeline.queryLatency() -> { ok, live, minLatency, maxLatency } or a clear
// unsupported result when the pipeline does not answer the latency query.
JSValue pipeline_query_latency(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "queryLatency");
    if (!box) {
        return JS_EXCEPTION;
    }
    GstQuery* query = gst_query_new_latency();
    const gboolean answered = gst_element_query(box->pipeline, query);
    JSValue obj = JS_NewObject(ctx);
    if (answered) {
        gboolean live = FALSE;
        GstClockTime minLatency = 0;
        GstClockTime maxLatency = 0;
        gst_query_parse_latency(query, &live, &minLatency, &maxLatency);
        JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, true));
        JS_SetPropertyStr(ctx, obj, "supported", JS_NewBool(ctx, true));
        JS_SetPropertyStr(ctx, obj, "live", JS_NewBool(ctx, live == TRUE));
        JS_SetPropertyStr(ctx, obj, "minLatency", JS_NewInt64(ctx, clock_time_or_unset(minLatency)));
        JS_SetPropertyStr(ctx, obj, "maxLatency", JS_NewInt64(ctx, clock_time_or_unset(maxLatency)));
    } else {
        JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, obj, "supported", JS_NewBool(ctx, false));
    }
    gst_query_unref(query);
    return obj;
}

// pipeline.setOverlayText({ elementName?, text }) -> update a live textoverlay.
JSValue pipeline_set_overlay_text(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "setOverlayText");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string elementName = "wl2_overlay";
    option_string(ctx, argv[0], "elementName", elementName);
    std::string text;
    if (argc < 1 || !option_string(ctx, argv[0], "text", text)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "setOverlayText",
            "setOverlayText({ text }) requires a text string");
    }
    GstElement* el = gst_bin_get_by_name(GST_BIN(box->pipeline), elementName.c_str());
    if (!el) {
        return throw_gst_error(ctx, "gstreamer_element_not_found", "setOverlayText",
            "No element named '" + elementName + "' in the pipeline");
    }
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(el), "text")) {
        gst_object_unref(el);
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "setOverlayText",
            "Element '" + elementName + "' has no 'text' property");
    }
    g_object_set(G_OBJECT(el), "text", text.c_str(), nullptr);
    gst_object_unref(el);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "elementName", JS_NewString(ctx, elementName.c_str()));
    JS_SetPropertyStr(ctx, obj, "text", JS_NewString(ctx, text.c_str()));
    return obj;
}

#if WL2_GSTREAMER_HAVE_APP

namespace {

// Encode a single retained sample to a file through a one-shot appsrc pipeline.
// Returns true on EOS; on failure sets errorOut to a human-readable reason.
bool encode_sample_to_file(GstSample* sample, const std::string& encoder,
    const std::string& path, std::string& errorOut) {
    std::string launch = "appsrc name=wl2_snap_src ! videoconvert ! " + encoder
        + " ! filesink location=" + quote_path(path);
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(launch.c_str(), &error);
    if (error) {
        errorOut = error->message ? error->message : "snapshot encode parse failed";
        g_error_free(error);
        if (pipeline) {
            gst_object_unref(pipeline);
        }
        return false;
    }
    if (!pipeline) {
        errorOut = "snapshot encode produced no pipeline";
        return false;
    }
    GstElement* srcElement = gst_bin_get_by_name(GST_BIN(pipeline), "wl2_snap_src");
    GstAppSrc* appsrc = srcElement ? GST_APP_SRC(srcElement) : nullptr;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!appsrc || !buffer) {
        errorOut = "snapshot encode could not access appsrc/buffer";
        if (srcElement) {
            gst_object_unref(srcElement);
        }
        gst_object_unref(pipeline);
        return false;
    }
    GstCaps* caps = gst_sample_get_caps(sample);
    if (caps) {
        gst_app_src_set_caps(appsrc, caps);
    }
    // Push a ref of the frame, then signal EOS so the encoder flushes one image.
    gst_app_src_push_buffer(appsrc, gst_buffer_ref(buffer));
    gst_app_src_end_of_stream(appsrc);
    gst_object_unref(srcElement);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    bool ok = false;
    if (message) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            ok = true;
        } else {
            GError* gerror = nullptr;
            gst_message_parse_error(message, &gerror, nullptr);
            errorOut = gerror && gerror->message ? gerror->message : "snapshot encode error";
            if (gerror) {
                g_error_free(gerror);
            }
        }
        gst_message_unref(message);
    } else {
        errorOut = "snapshot encode timed out";
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

} // namespace

// pipeline.snapshot({ elementName?, path?, format?, encoder? }) -> still-frame
// metadata from the latest video sink sample, optionally encoded to a file.
JSValue pipeline_snapshot(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "snapshot");
    if (!box) {
        return JS_EXCEPTION;
    }
    std::string elementName;
    if (argc > 0) {
        option_string(ctx, argv[0], "elementName", elementName);
    }

    // Find the video sink bridge (named, or the first one) and take its sample.
    GstSample* sample = nullptr;
    for (auto& bridge : box->bridges) {
        if (bridge->kind != "videoSink") {
            continue;
        }
        if (!elementName.empty() && bridge->name != elementName) {
            continue;
        }
        sample = bridge->acquireLastSample();
        break;
    }
    if (!sample) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "snapshot",
            elementName.empty()
                ? "No video sink has produced a frame to snapshot yet"
                : "No frame available from video sink '" + elementName + "'");
    }

    JSValue result = JS_NewObject(ctx);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (caps && gst_caps_get_size(caps) > 0) {
        GstStructure* s = gst_caps_get_structure(caps, 0);
        gint w = 0;
        gint h = 0;
        gst_structure_get_int(s, "width", &w);
        gst_structure_get_int(s, "height", &h);
        const char* fmt = gst_structure_get_string(s, "format");
        JS_SetPropertyStr(ctx, result, "width", JS_NewInt64(ctx, w));
        JS_SetPropertyStr(ctx, result, "height", JS_NewInt64(ctx, h));
        JS_SetPropertyStr(ctx, result, "format", JS_NewString(ctx, fmt ? fmt : ""));
        gchar* capsText = gst_caps_to_string(caps);
        JS_SetPropertyStr(ctx, result, "caps", JS_NewString(ctx, capsText ? capsText : ""));
        if (capsText) {
            g_free(capsText);
        }
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    JS_SetPropertyStr(ctx, result, "size",
        JS_NewInt64(ctx, buffer ? static_cast<int64_t>(gst_buffer_get_size(buffer)) : 0));
    const GstClockTime pts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    JS_SetPropertyStr(ctx, result, "pts", JS_NewInt64(ctx, clock_time_or_unset(pts)));

    // Optionally encode to a file. Default encoder follows the requested format.
    std::string path;
    if (argc > 0 && option_string(ctx, argv[0], "path", path) && !path.empty()) {
        std::string format = "png";
        option_string(ctx, argv[0], "format", format);
        std::string encoder = format == "jpeg" || format == "jpg" ? "jpegenc" : "pngenc";
        option_string(ctx, argv[0], "encoder", encoder); // explicit override wins
        std::string encodeError;
        const bool wrote = encode_sample_to_file(sample, encoder, path, encodeError);
        if (!wrote) {
            gst_sample_unref(sample);
            JS_FreeValue(ctx, result);
            return throw_gst_error(ctx, "gstreamer_membus_failed", "snapshot",
                "Could not write snapshot to '" + path + "': " + encodeError);
        }
        JS_SetPropertyStr(ctx, result, "path", JS_NewString(ctx, path.c_str()));
    }

    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, true));
    gst_sample_unref(sample);
    return result;
}

#else // !WL2_GSTREAMER_HAVE_APP

JSValue pipeline_snapshot(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return throw_gst_error(ctx, "gstreamer_unsupported", "snapshot",
        "This build lacks gstreamer-app-1.0; snapshot is unavailable");
}

#endif // WL2_GSTREAMER_HAVE_APP

} // namespace wl2_gstreamer

#endif // WL2_HAVE_QUICKJS
