#include "internal.h"

#if WL2_HAVE_QUICKJS

namespace wl2_gstreamer {

// --- Pipeline class ---------------------------------------------------------

JSClassID g_pipelineClassId = 0;

void close_pipeline(PipelineBox* box) {
    if (!box || box->closed) {
        return;
    }
    // Drive the pipeline to NULL and wait for the transition to finish. The wait
    // is essential: set_state() can return before streaming threads have stopped,
    // and destroying a bridge while its appsink callback is still in flight is a
    // use-after-free. get_state() with an infinite timeout blocks until the
    // pipeline has fully reached NULL and every streaming thread has joined.
    if (box->pipeline) {
        gst_element_set_state(box->pipeline, GST_STATE_NULL);
        gst_element_get_state(box->pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    }
    box->bridges.clear();
    if (box->bus) {
        gst_object_unref(box->bus);
        box->bus = nullptr;
    }
    if (box->pipeline) {
        gst_object_unref(box->pipeline);
        box->pipeline = nullptr;
    }
    box->closed = true;
}

void pipeline_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<PipelineBox*>(JS_GetOpaque(value, g_pipelineClassId));
    if (box) {
        close_pipeline(box);
        delete box;
    }
}

// Fetch a live pipeline box, throwing a stable error when closed or invalid.
PipelineBox* live_pipeline(JSContext* ctx, JSValueConst thisVal, const char* operation) {
    auto* box = static_cast<PipelineBox*>(JS_GetOpaque2(ctx, thisVal, g_pipelineClassId));
    if (!box) {
        throw_gst_error(ctx, "gstreamer_invalid_argument", operation, "Not a Pipeline object");
        return nullptr;
    }
    if (box->closed || !box->pipeline) {
        throw_gst_error(ctx, "gstreamer_closed", operation, "Pipeline is closed");
        return nullptr;
    }
    return box;
}

JSValue state_result(JSContext* ctx, GstElement* pipeline, GstStateChangeReturn change) {
    GstState state = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline, &state, &pending, 0);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "state", JS_NewString(ctx, gst_element_state_get_name(state)));
    JS_SetPropertyStr(ctx, obj, "pending", JS_NewString(ctx, gst_element_state_get_name(pending)));
    const char* resultName = "unknown";
    switch (change) {
        case GST_STATE_CHANGE_FAILURE: resultName = "failure"; break;
        case GST_STATE_CHANGE_SUCCESS: resultName = "success"; break;
        case GST_STATE_CHANGE_ASYNC: resultName = "async"; break;
        case GST_STATE_CHANGE_NO_PREROLL: resultName = "no-preroll"; break;
    }
    JS_SetPropertyStr(ctx, obj, "result", JS_NewString(ctx, resultName));
    return obj;
}

JSValue set_pipeline_state(JSContext* ctx, JSValueConst thisVal, const char* operation, GstState target) {
    PipelineBox* box = live_pipeline(ctx, thisVal, operation);
    if (!box) {
        return JS_EXCEPTION;
    }
    GstStateChangeReturn change = gst_element_set_state(box->pipeline, target);
    if (change == GST_STATE_CHANGE_FAILURE) {
        return throw_gst_error(ctx, "gstreamer_state_change_failed", operation,
            std::string("Failed to change pipeline state to ") + gst_element_state_get_name(target));
    }
    return state_result(ctx, box->pipeline, change);
}

JSValue pipeline_state(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "state");
    if (!box) {
        return JS_EXCEPTION;
    }
    return state_result(ctx, box->pipeline, GST_STATE_CHANGE_SUCCESS);
}

JSValue pipeline_set_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "setState",
            "setState(name) requires a state name string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_EXCEPTION;
    }
    std::string name = text;
    JS_FreeCString(ctx, text);

    GstState target = GST_STATE_NULL;
    if (name == "null") {
        target = GST_STATE_NULL;
    } else if (name == "ready") {
        target = GST_STATE_READY;
    } else if (name == "paused") {
        target = GST_STATE_PAUSED;
    } else if (name == "playing") {
        target = GST_STATE_PLAYING;
    } else {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "setState",
            "Unknown state name: " + name + " (expected null|ready|paused|playing)");
    }
    return set_pipeline_state(ctx, thisVal, "setState", target);
}

JSValue pipeline_play(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    return set_pipeline_state(ctx, thisVal, "play", GST_STATE_PLAYING);
}

JSValue pipeline_pause(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    return set_pipeline_state(ctx, thisVal, "pause", GST_STATE_PAUSED);
}

JSValue pipeline_stop(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    return set_pipeline_state(ctx, thisVal, "stop", GST_STATE_NULL);
}

JSValue pipeline_query_position(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "queryPosition");
    if (!box) {
        return JS_EXCEPTION;
    }
    gint64 position = 0;
    gboolean ok = gst_element_query_position(box->pipeline, GST_FORMAT_TIME, &position);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, ok == TRUE));
    JS_SetPropertyStr(ctx, obj, "position", JS_NewInt64(ctx, ok ? static_cast<int64_t>(position) : -1));
    return obj;
}

JSValue pipeline_query_duration(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "queryDuration");
    if (!box) {
        return JS_EXCEPTION;
    }
    gint64 duration = 0;
    gboolean ok = gst_element_query_duration(box->pipeline, GST_FORMAT_TIME, &duration);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, ok == TRUE));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, ok ? static_cast<int64_t>(duration) : -1));
    return obj;
}

JSValue pipeline_seek(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "seek");
    if (!box) {
        return JS_EXCEPTION;
    }
    int64_t position = 0;
    if (argc < 1 || !option_int(ctx, argv[0], "position", position)) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "seek",
            "seek({ position }) requires a position in nanoseconds");
    }
    if (position < 0) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "seek", "seek position must be non-negative");
    }
    const bool flush = option_bool(ctx, argv[0], "flush", true);
    GstSeekFlags flags = static_cast<GstSeekFlags>(
        (flush ? GST_SEEK_FLAG_FLUSH : GST_SEEK_FLAG_NONE) | GST_SEEK_FLAG_KEY_UNIT);
    gboolean ok = gst_element_seek_simple(box->pipeline, GST_FORMAT_TIME, flags, position);
    if (!ok) {
        return throw_gst_error(ctx, "gstreamer_seek_failed", "seek", "Pipeline did not handle the seek");
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, true));
    return obj;
}

// Build a JS object describing one bus message, then unref the message.
JSValue message_to_js(JSContext* ctx, GstMessage* message) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, GST_MESSAGE_TYPE_NAME(message)));
    GstObject* src = GST_MESSAGE_SRC(message);
    const char* srcName = src ? GST_OBJECT_NAME(src) : nullptr;
    JS_SetPropertyStr(ctx, obj, "source", JS_NewString(ctx, srcName ? srcName : ""));

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
        case GST_MESSAGE_WARNING:
        case GST_MESSAGE_INFO: {
            GError* gerror = nullptr;
            gchar* debug = nullptr;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                gst_message_parse_error(message, &gerror, &debug);
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
                gst_message_parse_warning(message, &gerror, &debug);
            } else {
                gst_message_parse_info(message, &gerror, &debug);
            }
            if (gerror) {
                JS_SetPropertyStr(ctx, obj, "message", JS_NewString(ctx, gerror->message ? gerror->message : ""));
                JS_SetPropertyStr(ctx, obj, "domain", JS_NewInt64(ctx, static_cast<int64_t>(gerror->domain)));
                JS_SetPropertyStr(ctx, obj, "gcode", JS_NewInt64(ctx, static_cast<int64_t>(gerror->code)));
                g_error_free(gerror);
            }
            if (debug) {
                JS_SetPropertyStr(ctx, obj, "debug", JS_NewString(ctx, debug));
                g_free(debug);
            }
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState oldState = GST_STATE_VOID_PENDING;
            GstState newState = GST_STATE_VOID_PENDING;
            GstState pending = GST_STATE_VOID_PENDING;
            gst_message_parse_state_changed(message, &oldState, &newState, &pending);
            JS_SetPropertyStr(ctx, obj, "oldState", JS_NewString(ctx, gst_element_state_get_name(oldState)));
            JS_SetPropertyStr(ctx, obj, "newState", JS_NewString(ctx, gst_element_state_get_name(newState)));
            JS_SetPropertyStr(ctx, obj, "pending", JS_NewString(ctx, gst_element_state_get_name(pending)));
            break;
        }
        case GST_MESSAGE_QOS: {
            // QoS reporting: which element is dropping/late and by how much.
            gboolean live = FALSE;
            guint64 runningTime = 0;
            guint64 streamTime = 0;
            guint64 timestamp = 0;
            guint64 duration = 0;
            gst_message_parse_qos(message, &live, &runningTime, &streamTime, &timestamp, &duration);
            JS_SetPropertyStr(ctx, obj, "live", JS_NewBool(ctx, live == TRUE));
            JS_SetPropertyStr(ctx, obj, "runningTime", JS_NewInt64(ctx, static_cast<int64_t>(runningTime)));
            JS_SetPropertyStr(ctx, obj, "streamTime", JS_NewInt64(ctx, static_cast<int64_t>(streamTime)));
            JS_SetPropertyStr(ctx, obj, "timestamp", JS_NewInt64(ctx, static_cast<int64_t>(timestamp)));
            JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, static_cast<int64_t>(duration)));
            gint64 jitter = 0;
            gdouble proportion = 0.0;
            gint quality = 0;
            gst_message_parse_qos_values(message, &jitter, &proportion, &quality);
            JS_SetPropertyStr(ctx, obj, "jitter", JS_NewInt64(ctx, static_cast<int64_t>(jitter)));
            JS_SetPropertyStr(ctx, obj, "proportion", JS_NewFloat64(ctx, proportion));
            JS_SetPropertyStr(ctx, obj, "quality", JS_NewInt64(ctx, static_cast<int64_t>(quality)));
            GstFormat format = GST_FORMAT_UNDEFINED;
            guint64 processed = 0;
            guint64 dropped = 0;
            gst_message_parse_qos_stats(message, &format, &processed, &dropped);
            JS_SetPropertyStr(ctx, obj, "processed", JS_NewInt64(ctx, static_cast<int64_t>(processed)));
            JS_SetPropertyStr(ctx, obj, "dropped", JS_NewInt64(ctx, static_cast<int64_t>(dropped)));
            break;
        }
        default:
            break;
    }
    return obj;
}

JSValue pipeline_bus_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PipelineBox* box = live_pipeline(ctx, thisVal, "busPoll");
    if (!box) {
        return JS_EXCEPTION;
    }
    if (!box->bus) {
        return JS_NewArray(ctx);
    }
    int64_t timeoutMs = 0;
    int64_t maxMessages = 64;
    if (argc > 0) {
        option_int(ctx, argv[0], "timeoutMs", timeoutMs);
        option_int(ctx, argv[0], "max", maxMessages);
    }
    if (maxMessages < 0) {
        maxMessages = 0;
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    // Wait up to timeoutMs for the first message, then drain the rest without
    // blocking so a single call returns a batch of pending events.
    GstClockTime timeout = timeoutMs > 0 ? static_cast<GstClockTime>(timeoutMs) * GST_MSECOND : 0;
    for (int64_t i = 0; i < maxMessages; ++i) {
        GstMessage* message = gst_bus_timed_pop_filtered(
            box->bus, i == 0 ? timeout : 0, static_cast<GstMessageType>(GST_MESSAGE_ANY));
        if (!message) {
            break;
        }
        JS_SetPropertyUint32(ctx, array, index++, message_to_js(ctx, message));
        gst_message_unref(message);
    }
    return array;
}

JSValue pipeline_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<PipelineBox*>(JS_GetOpaque2(ctx, thisVal, g_pipelineClassId));
    if (box) {
        close_pipeline(box);
    }
    return JS_UNDEFINED;
}

JSValue new_pipeline_object(JSContext* ctx, GstElement* pipeline) {
    JSValue obj = JS_NewObjectClass(ctx, g_pipelineClassId);
    if (JS_IsException(obj)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return obj;
    }
    auto* box = new PipelineBox{};
    box->pipeline = pipeline;
    box->bus = gst_element_get_bus(pipeline);
    JS_SetOpaque(obj, box);
    return obj;
}

JSValue gst_parse_launch_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!ensure_gst_init()) {
        return throw_gst_init_error(ctx, "parseLaunch");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_gst_error(ctx, "gstreamer_invalid_argument", "parseLaunch",
            "parseLaunch(description) requires a launch string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_EXCEPTION;
    }
    std::string description = text;
    JS_FreeCString(ctx, text);

    GError* error = nullptr;
    GstElement* element = gst_parse_launch(description.c_str(), &error);
    // gst_parse_launch may return a partially built element even on error (for
    // example an unknown element name), so treat any set error as a parse
    // failure and release whatever was produced.
    if (error) {
        std::string message = error->message ? error->message : "gst_parse_launch failed";
        g_error_free(error);
        if (element) {
            gst_object_unref(element);
        }
        return throw_gst_error(ctx, "gstreamer_parse_failed", "parseLaunch", message);
    }
    if (!element) {
        return throw_gst_error(ctx, "gstreamer_parse_failed", "parseLaunch", "gst_parse_launch returned no pipeline");
    }
    if (!GST_IS_PIPELINE(element)) {
        gst_object_unref(element);
        return throw_gst_error(ctx, "gstreamer_not_a_pipeline", "parseLaunch",
            "Launch description did not produce a pipeline");
    }
    return new_pipeline_object(ctx, element);
}

} // namespace wl2_gstreamer

#endif // WL2_HAVE_QUICKJS
