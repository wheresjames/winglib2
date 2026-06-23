// wl2_ffmpeg Filter Graph implementation
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// Filter Graph Operations
// ============================================================================

#if WL2_FFMPEG_HAVE_FILTERS
// Decode one frame, run it through a libavfilter chain (for example
// "scale=160:120,crop=80:80", "fps=15", "rotate=PI/2"), and return the
// filtered frame metadata. Compiled only when filter support is enabled.
int apply_video_filter_file(const std::string& path, int64_t timestampUs, const std::string& filterSpec,
    AVPixelFormat outputFormat, DecodedVideoFrame& out) {
    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return ret;
    }
    auto closeFormat = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);
    if ((ret = avformat_find_stream_info(format, nullptr)) < 0) {
        return ret;
    }
    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        return streamIndex;
    }
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        return AVERROR_DECODER_NOT_FOUND;
    }
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        return AVERROR(ENOMEM);
    }
    auto freeDecoder = [](AVCodecContext* c) { if (c) avcodec_free_context(&c); };
    std::unique_ptr<AVCodecContext, decltype(freeDecoder)> decoderGuard(decoder, freeDecoder);
    avcodec_parameters_to_context(decoder, stream->codecpar);
    if ((ret = avcodec_open2(decoder, codec, nullptr)) < 0) {
        return ret;
    }
    if (timestampUs > 0) {
        const int64_t targetTs = av_rescale_q(timestampUs, AVRational{1, 1000000}, stream->time_base);
        av_seek_frame(format, streamIndex, targetTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(decoder);
    }

    // Decode the first available frame as filter input.
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    bool gotFrame = false;
    while (!gotFrame && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == streamIndex && avcodec_send_packet(decoder, packet) >= 0) {
            if (avcodec_receive_frame(decoder, frame) >= 0) {
                gotFrame = true;
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (!gotFrame) {
        av_frame_free(&frame);
        return AVERROR_EOF;
    }

    AVFilterGraph* graph = avfilter_graph_alloc();
    AVFilterContext* srcCtx = nullptr;
    AVFilterContext* sinkCtx = nullptr;
    if (!graph) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
        frame->width, frame->height, frame->format, stream->time_base.num, stream->time_base.den);
    ret = avfilter_graph_create_filter(&srcCtx, avfilter_get_by_name("buffer"), "in", args, nullptr, graph);
    if (ret >= 0) {
        ret = avfilter_graph_create_filter(&sinkCtx, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFrame* filtered = av_frame_alloc();
    if (ret >= 0 && outputs && inputs && filtered) {
        outputs->name = av_strdup("in");
        outputs->filter_ctx = srcCtx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sinkCtx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        ret = avfilter_graph_parse_ptr(graph, filterSpec.c_str(), &inputs, &outputs, nullptr);
        if (ret >= 0) {
            ret = avfilter_graph_config(graph, nullptr);
        }
        if (ret >= 0) {
            ret = av_buffersrc_add_frame_flags(srcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        }
        if (ret >= 0) {
            ret = av_buffersink_get_frame(sinkCtx, filtered);
        }
        if (ret >= 0) {
            ret = scale_selected_frame(filtered, stream, outputFormat, out);
            out.streamIndex = streamIndex;
            out.resultMode = "filtered";
        }
    } else {
        ret = ret < 0 ? ret : AVERROR(ENOMEM);
    }

    av_frame_free(&filtered);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    return ret;
}
#endif

#if WL2_HAVE_QUICKJS
JSValue filter_graph_apply(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#if WL2_FFMPEG_HAVE_FILTERS
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "FilterGraph.apply(options) requires an options object");
    }
    std::string path;
    std::string spec;
    std::string formatName = "rgb24";
    int64_t timestampUs = 0;
    get_string_prop(ctx, argv[0], "path", path);
    get_string_prop(ctx, argv[0], "filter", spec);
    get_string_prop(ctx, argv[0], "format", formatName);
    JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
    if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
        JS_ToInt64(ctx, &timestampUs, timestamp);
    }
    JS_FreeValue(ctx, timestamp);
    if (path.empty() || spec.empty()) {
        return JS_ThrowTypeError(ctx, "FilterGraph.apply requires path and filter");
    }
    DecodedVideoFrame frame;
    int ret = apply_video_filter_file(path, timestampUs, spec, pixel_format_from_name(formatName), frame);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "FilterGraph.apply", ret);
    }
    JSValue obj = make_decoded_frame(ctx, frame);
    set_string(ctx, obj, "filter", spec);
    return obj;
#else
    (void)argc;
    (void)argv;
    return throw_module_error(ctx, "FilterGraph.apply", "filters_disabled",
        "Filter graph support was not compiled in (enable WL2_FFMPEG_ENABLE_FILTERS)");
#endif
}

JSValue filter_graph_info(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
#if WL2_FFMPEG_HAVE_FILTERS
    set_bool(ctx, obj, "enabled", true);
    JSValue available = JS_NewArray(ctx);
    uint32_t count = 0;
    static const char* kFilters[] = {"scale", "crop", "fps", "rotate", "volume", "aresample", "atempo"};
    for (const char* name : kFilters) {
        if (avfilter_get_by_name(name)) {
            JS_SetPropertyUint32(ctx, available, count++, JS_NewString(ctx, name));
        }
    }
    JS_SetPropertyStr(ctx, obj, "filters", available);
#else
    set_bool(ctx, obj, "enabled", false);
    JS_SetPropertyStr(ctx, obj, "filters", JS_NewArray(ctx));
#endif
    return obj;
}

JSValue make_filter_graph_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "apply", JS_NewCFunction(ctx, filter_graph_apply, "apply", 1));
    JS_SetPropertyStr(ctx, ns, "info", JS_NewCFunction(ctx, filter_graph_info, "info", 0));
    return ns;
}
#endif // WL2_HAVE_QUICKJS