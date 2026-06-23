// wl2_ffmpeg Demuxer implementation
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// Demuxer Implementation
// ============================================================================

// Forward declarations for replay session functions defined in replay.cpp
#if WL2_HAVE_QUICKJS
JSValue replay_session_seek(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_extract_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_extract_audio(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_publish_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_publish_audio(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_playback_plan(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_reverse_window(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_play_schedule(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_analyze_timestamps(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue replay_session_state(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*);
JSValue replay_session_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*);
#endif

#if WL2_HAVE_QUICKJS
JSValue demuxer_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path = path_from_first_arg(ctx, argc, argv, "Demuxer.open");
    if (path.empty()) {
        return JS_EXCEPTION;
    }

    auto handle = std::make_unique<DemuxerHandle>();
    handle->path = path;
    int ret = avformat_open_input(&handle->format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Demuxer.open", ret, "Unable to open input: " + path);
    }
    ret = avformat_find_stream_info(handle->format, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Demuxer.open", ret, "Unable to read stream info: " + path);
    }
    ret = av_find_best_stream(handle->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret >= 0) {
        handle->videoStream = ret;
    }
    return new_demuxer(ctx, std::move(handle));
}

JSValue demuxer_streams(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* handle = get_demuxer(ctx, thisVal);
    if (!handle || !handle->format) {
        return JS_EXCEPTION;
    }
    JSValue array = JS_NewArray(ctx);
    for (unsigned i = 0; i < handle->format->nb_streams; ++i) {
        JS_SetPropertyUint32(ctx, array, i, make_stream_info(ctx, handle->format->streams[i]));
    }
    return array;
}

JSValue demuxer_read_packet(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* handle = get_demuxer(ctx, thisVal);
    if (!handle || !handle->format) {
        return JS_EXCEPTION;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return JS_ThrowOutOfMemory(ctx);
    }
    int ret = av_read_frame(handle->format, packet);
    if (ret == AVERROR_EOF) {
        av_packet_free(&packet);
        return JS_NULL;
    }
    if (ret < 0) {
        av_packet_free(&packet);
        return throw_ffmpeg_error(ctx, "Demuxer.readPacket", ret);
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "streamIndex", JS_NewInt32(ctx, packet->stream_index));
    JS_SetPropertyStr(ctx, obj, "pts", JS_NewInt64(ctx, packet->pts));
    JS_SetPropertyStr(ctx, obj, "dts", JS_NewInt64(ctx, packet->dts));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, packet->duration));
    JS_SetPropertyStr(ctx, obj, "flags", JS_NewInt32(ctx, packet->flags));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt32(ctx, packet->size));
    JS_SetPropertyStr(ctx, obj, "data", make_wl2_buffer(ctx, packet->data, packet->size));
    av_packet_free(&packet);
    return obj;
}

JSValue demuxer_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* handle = get_demuxer(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    if (handle->format) {
        avformat_close_input(&handle->format);
    }
    return JS_UNDEFINED;
}

JSValue make_demuxer_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "open", JS_NewCFunction(ctx, demuxer_open, "open", 1));
    return ns;
}

int ensure_native_classes(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (demuxer_class_id == 0) {
        JS_NewClassID(&demuxer_class_id);
    }
    JSClassDef demuxerDef{};
    demuxerDef.class_name = "Demuxer";
    demuxerDef.finalizer = demuxer_finalizer;
    JS_NewClass(rt, demuxer_class_id, &demuxerDef);
    JSValue demuxerProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, demuxerProto, "streams", JS_NewCFunction(ctx, demuxer_streams, "streams", 0));
    JS_SetPropertyStr(ctx, demuxerProto, "readPacket", JS_NewCFunction(ctx, demuxer_read_packet, "readPacket", 0));
    JS_SetPropertyStr(ctx, demuxerProto, "close", JS_NewCFunction(ctx, demuxer_close, "close", 0));
    JS_SetClassProto(ctx, demuxer_class_id, demuxerProto);

    if (replay_session_class_id == 0) {
        JS_NewClassID(&replay_session_class_id);
    }
    JSClassDef replayDef{};
    replayDef.class_name = "ReplaySession";
    replayDef.finalizer = replay_session_finalizer;
    JS_NewClass(rt, replay_session_class_id, &replayDef);
    JSValue replayProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, replayProto, "seek", JS_NewCFunction(ctx, replay_session_seek, "seek", 1));
    JS_SetPropertyStr(ctx, replayProto, "extractFrame", JS_NewCFunction(ctx, replay_session_extract_frame, "extractFrame", 1));
    JS_SetPropertyStr(ctx, replayProto, "extractAudio", JS_NewCFunction(ctx, replay_session_extract_audio, "extractAudio", 1));
    JS_SetPropertyStr(ctx, replayProto, "publishFrame", JS_NewCFunction(ctx, replay_session_publish_frame, "publishFrame", 1));
    JS_SetPropertyStr(ctx, replayProto, "publishAudio", JS_NewCFunction(ctx, replay_session_publish_audio, "publishAudio", 1));
    JS_SetPropertyStr(ctx, replayProto, "playbackPlan", JS_NewCFunction(ctx, replay_session_playback_plan, "playbackPlan", 1));
    JS_SetPropertyStr(ctx, replayProto, "extractReverseWindow", JS_NewCFunction(ctx, replay_session_reverse_window, "extractReverseWindow", 1));
    JS_SetPropertyStr(ctx, replayProto, "playSchedule", JS_NewCFunction(ctx, replay_session_play_schedule, "playSchedule", 1));
    JS_SetPropertyStr(ctx, replayProto, "analyzeTimestamps", JS_NewCFunction(ctx, replay_session_analyze_timestamps, "analyzeTimestamps", 1));
    JS_SetPropertyStr(ctx, replayProto, "state", JS_NewCFunction(ctx, replay_session_state, "state", 0));
    JS_SetPropertyStr(ctx, replayProto, "close", JS_NewCFunction(ctx, replay_session_close, "close", 0));
    JS_SetClassProto(ctx, replay_session_class_id, replayProto);
    return 0;
}
#endif // WL2_HAVE_QUICKJS