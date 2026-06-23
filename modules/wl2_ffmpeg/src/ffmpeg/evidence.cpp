// wl2_ffmpeg Evidence workflows implementation
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// Evidence: Still Export
// ============================================================================

// Decode the frame at `timestampUs` and write it as a single still image. PNG,
// JPEG/MJPEG, and BMP are single-frame codecs, so the encoded packet is the
// complete file.
int export_still_image(const std::string& path, int64_t timestampUs, const StillCodecChoice& choice,
    const std::string& outFile, int64_t& bytesWritten, int64_t& ptsUs) {
    DecodedVideoFrame frame;
    int ret = decode_video_frame_modes(path, timestampUs, 0, VideoSeekMode::Accurate, choice.pixelFormat, frame);
    if (ret < 0) {
        return ret;
    }
    ptsUs = frame.ptsUs;

    const AVCodec* codec = avcodec_find_encoder(choice.id);
    if (!codec) {
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) {
        return AVERROR(ENOMEM);
    }
    auto freeEnc = [](AVCodecContext* c) { if (c) avcodec_free_context(&c); };
    std::unique_ptr<AVCodecContext, decltype(freeEnc)> encGuard(enc, freeEnc);
    enc->width = frame.width;
    enc->height = frame.height;
    enc->pix_fmt = choice.pixelFormat;
    enc->time_base = AVRational{1, 1};
    if (choice.id == AV_CODEC_ID_MJPEG) {
        enc->color_range = AVCOL_RANGE_JPEG;
    }
    ret = avcodec_open2(enc, codec, nullptr);
    if (ret < 0) {
        return ret;
    }

    AVFrame* avframe = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (!avframe || !packet) {
        av_frame_free(&avframe);
        av_packet_free(&packet);
        return AVERROR(ENOMEM);
    }
    avframe->format = choice.pixelFormat;
    avframe->width = frame.width;
    avframe->height = frame.height;
    ret = av_image_fill_arrays(avframe->data, avframe->linesize, frame.bytes.data(),
        choice.pixelFormat, frame.width, frame.height, 1);
    if (ret < 0) {
        av_frame_free(&avframe);
        av_packet_free(&packet);
        return ret;
    }
    avframe->pts = 0;
    ret = avcodec_send_frame(enc, avframe);
    if (ret >= 0) {
        ret = avcodec_send_frame(enc, nullptr);
    }
    bytesWritten = 0;
    bool wrote = false;
    while (avcodec_receive_packet(enc, packet) >= 0) {
        std::ofstream output(outFile, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(reinterpret_cast<const char*>(packet->data), packet->size);
            bytesWritten = packet->size;
            wrote = true;
        }
        av_packet_unref(packet);
    }
    av_frame_free(&avframe);
    av_packet_free(&packet);
    return wrote ? 0 : AVERROR_UNKNOWN;
}

#if WL2_HAVE_QUICKJS
JSValue evidence_export_still(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Evidence.exportStill(options) requires an options object");
    }
    std::string path;
    std::string outputPath;
    std::string formatName = "png";
    int64_t timestampUs = 0;
    get_string_prop(ctx, argv[0], "path", path);
    get_string_prop(ctx, argv[0], "outputPath", outputPath);
    get_string_prop(ctx, argv[0], "format", formatName);
    JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
    if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
        JS_ToInt64(ctx, &timestampUs, timestamp);
    }
    JS_FreeValue(ctx, timestamp);
    if (path.empty() || outputPath.empty()) {
        return JS_ThrowTypeError(ctx, "Evidence.exportStill requires path and outputPath");
    }
    StillCodecChoice choice;
    if (!still_codec_from_name(formatName, choice)) {
        return throw_module_error(ctx, "Evidence.exportStill", "format_unsupported",
            "Supported still formats are png, jpeg, and bmp");
    }
    int64_t bytesWritten = 0;
    int64_t ptsUs = 0;
    int ret = export_still_image(path, timestampUs, choice, outputPath, bytesWritten, ptsUs);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Evidence.exportStill", ret);
    }
    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "outputPath", outputPath);
    set_string(ctx, obj, "format", formatName);
    JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, bytesWritten));
    JS_SetPropertyStr(ctx, obj, "ptsUs", JS_NewInt64(ctx, ptsUs));
    return obj;
}
#endif

// ============================================================================
// Evidence: Clip Extraction
// ============================================================================

// Packet-copy a timestamp range into a new container, preserving compressed
// data and source timestamps.
int evidence_clip_copy(const std::string& inPath, const std::string& outPath,
    int64_t startUs, int64_t endUs, int64_t& framesCopied, int64_t& startPtsUs, int64_t& endPtsUs) {
    AVFormatContext* inFmt = nullptr;
    int ret = avformat_open_input(&inFmt, inPath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return ret;
    }
    auto closeInput = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeInput)> inGuard(inFmt, closeInput);
    ret = avformat_find_stream_info(inFmt, nullptr);
    if (ret < 0) {
        return ret;
    }
    const int videoIndex = av_find_best_stream(inFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        return videoIndex;
    }

    AVFormatContext* outFmt = nullptr;
    ret = avformat_alloc_output_context2(&outFmt, nullptr, nullptr, outPath.c_str());
    if (ret < 0 || !outFmt) {
        return ret < 0 ? ret : AVERROR_UNKNOWN;
    }
    auto freeOutput = [](AVFormatContext* f) {
        if (f) {
            if (f->pb && !(f->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&f->pb);
            }
            avformat_free_context(f);
        }
    };
    std::unique_ptr<AVFormatContext, decltype(freeOutput)> outGuard(outFmt, freeOutput);

    std::vector<int> streamMap(inFmt->nb_streams, -1);
    for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
        const AVMediaType type = inFmt->streams[i]->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* outStream = avformat_new_stream(outFmt, nullptr);
        if (!outStream) {
            return AVERROR(ENOMEM);
        }
        if ((ret = avcodec_parameters_copy(outStream->codecpar, inFmt->streams[i]->codecpar)) < 0) {
            return ret;
        }
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inFmt->streams[i]->time_base;
        streamMap[i] = outStream->index;
    }

    const int64_t startTsVideo = startUs > 0
        ? av_rescale_q(startUs, AVRational{1, 1000000}, inFmt->streams[videoIndex]->time_base) : 0;
    const int64_t endTsVideo = endUs < INT64_MAX
        ? av_rescale_q(endUs, AVRational{1, 1000000}, inFmt->streams[videoIndex]->time_base) : INT64_MAX;
    if (startUs > 0) {
        av_seek_frame(inFmt, videoIndex, startTsVideo, AVSEEK_FLAG_BACKWARD);
    }

    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outFmt->pb, outPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            return ret;
        }
    }
    ret = avformat_write_header(outFmt, nullptr);
    if (ret < 0) {
        return ret;
    }

    framesCopied = 0;
    startPtsUs = AV_NOPTS_VALUE;
    endPtsUs = AV_NOPTS_VALUE;
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return AVERROR(ENOMEM);
    }
    bool done = false;
    while (!done && av_read_frame(inFmt, packet) >= 0) {
        const int mapped = (packet->stream_index >= 0 && packet->stream_index < static_cast<int>(streamMap.size()))
            ? streamMap[packet->stream_index] : -1;
        if (mapped < 0) {
            av_packet_unref(packet);
            continue;
        }
        const bool isVideo = static_cast<int>(packet->stream_index) == videoIndex;
        if (isVideo) {
            if (packet->pts != AV_NOPTS_VALUE && packet->pts < startTsVideo) {
                av_packet_unref(packet);
                continue;
            }
            if (packet->pts != AV_NOPTS_VALUE && packet->pts > endTsVideo) {
                done = true;
                av_packet_unref(packet);
                break;
            }
            if (startPtsUs == AV_NOPTS_VALUE) {
                startPtsUs = timestamp_to_us(packet->pts, inFmt->streams[videoIndex]->time_base);
            }
            endPtsUs = timestamp_to_us(packet->pts, inFmt->streams[videoIndex]->time_base);
            ++framesCopied;
        }
        AVStream* inStream = inFmt->streams[packet->stream_index];
        AVStream* outStream = outFmt->streams[mapped];
        packet->stream_index = mapped;
        av_packet_rescale_ts(packet, inStream->time_base, outStream->time_base);
        packet->pos = -1;
        ret = av_interleaved_write_frame(outFmt, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            break;
        }
    }
    av_packet_free(&packet);
    int writeRet = av_write_trailer(outFmt);
    return writeRet < 0 ? writeRet : 0;
}

#if WL2_HAVE_QUICKJS
JSValue evidence_extract_clip(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Evidence.extractClip(options) requires an options object");
    }
    std::string path;
    std::string outputPath;
    std::string mode = "auto";
    int64_t startUs = 0;
    int64_t endUs = 0;
    get_string_prop(ctx, argv[0], "path", path);
    get_string_prop(ctx, argv[0], "outputPath", outputPath);
    get_string_prop(ctx, argv[0], "mode", mode);
    JSValue startValue = JS_GetPropertyStr(ctx, argv[0], "startUs");
    if (!JS_IsUndefined(startValue) && !JS_IsNull(startValue)) {
        JS_ToInt64(ctx, &startUs, startValue);
    }
    JS_FreeValue(ctx, startValue);
    JSValue endValue = JS_GetPropertyStr(ctx, argv[0], "endUs");
    if (!JS_IsUndefined(endValue) && !JS_IsNull(endValue)) {
        JS_ToInt64(ctx, &endUs, endValue);
    }
    JS_FreeValue(ctx, endValue);
    if (path.empty() || outputPath.empty()) {
        return JS_ThrowTypeError(ctx, "Evidence.extractClip requires path and outputPath");
    }
    if (endUs <= 0) {
        endUs = INT64_MAX;
    }

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "outputPath", outputPath);
    JS_SetPropertyStr(ctx, obj, "requestedStartUs", JS_NewInt64(ctx, startUs));
    JS_SetPropertyStr(ctx, obj, "requestedEndUs", JS_NewInt64(ctx, endUs == INT64_MAX ? -1 : endUs));

    if (mode == "transcode") {
        TranscodeResult result;
        int ret = transcode_file(path, outputPath, "", "", "low-latency", true, result, startUs, endUs);
        if (ret < 0) {
            JS_FreeValue(ctx, obj);
            return throw_ffmpeg_error(ctx, "Evidence.extractClip", ret);
        }
        set_string(ctx, obj, "mode", "transcode");
        set_string(ctx, obj, "videoCodec", result.videoCodec);
        JS_SetPropertyStr(ctx, obj, "videoFrames", JS_NewInt64(ctx, result.videoPackets));
        set_bool(ctx, obj, "reopened", result.reopened);
        return obj;
    }

    int64_t framesCopied = 0;
    int64_t startPtsUs = 0;
    int64_t endPtsUs = 0;
    int ret = evidence_clip_copy(path, outputPath, startUs, endUs, framesCopied, startPtsUs, endPtsUs);
    if (ret < 0) {
        JS_FreeValue(ctx, obj);
        return throw_ffmpeg_error(ctx, "Evidence.extractClip", ret);
    }
    set_string(ctx, obj, "mode", "packet-copy");
    JS_SetPropertyStr(ctx, obj, "videoFrames", JS_NewInt64(ctx, framesCopied));
    JS_SetPropertyStr(ctx, obj, "startPtsUs", JS_NewInt64(ctx, startPtsUs));
    JS_SetPropertyStr(ctx, obj, "endPtsUs", JS_NewInt64(ctx, endPtsUs));
    return obj;
}

// ============================================================================
// Evidence: Keyframe Window
// ============================================================================

JSValue evidence_keyframe_window(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Evidence.keyframeWindow(options) requires an options object");
    }
    std::string path;
    int64_t centerUs = 0;
    int64_t windowUs = 1'000'000;
    get_string_prop(ctx, argv[0], "path", path);
    JSValue centerValue = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
    if (!JS_IsUndefined(centerValue) && !JS_IsNull(centerValue)) {
        JS_ToInt64(ctx, &centerUs, centerValue);
    }
    JS_FreeValue(ctx, centerValue);
    JSValue windowValue = JS_GetPropertyStr(ctx, argv[0], "windowUs");
    if (!JS_IsUndefined(windowValue) && !JS_IsNull(windowValue)) {
        JS_ToInt64(ctx, &windowUs, windowValue);
    }
    JS_FreeValue(ctx, windowValue);
    if (path.empty()) {
        return JS_ThrowTypeError(ctx, "Evidence.keyframeWindow requires a path");
    }

    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Evidence.keyframeWindow", ret);
    }
    auto closeFormat = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);
    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Evidence.keyframeWindow", ret);
    }
    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        return throw_ffmpeg_error(ctx, "Evidence.keyframeWindow", videoIndex);
    }
    AVStream* stream = format->streams[videoIndex];
    const int64_t lowUs = centerUs - windowUs;
    const int64_t highUs = centerUs + windowUs;

    JSValue array = JS_NewArray(ctx);
    uint32_t count = 0;
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        JS_FreeValue(ctx, array);
        return JS_ThrowOutOfMemory(ctx);
    }
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == videoIndex && (packet->flags & AV_PKT_FLAG_KEY)) {
            const int64_t ptsUs = timestamp_to_us(packet->pts, stream->time_base);
            if (ptsUs == AV_NOPTS_VALUE || (ptsUs >= lowUs && ptsUs <= highUs)) {
                JSValue entry = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, entry, "pts", JS_NewInt64(ctx, packet->pts));
                JS_SetPropertyStr(ctx, entry, "ptsUs", JS_NewInt64(ctx, ptsUs));
                JS_SetPropertyStr(ctx, entry, "size", JS_NewInt32(ctx, packet->size));
                JS_SetPropertyUint32(ctx, array, count++, entry);
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "keyframes", array);
    JS_SetPropertyStr(ctx, obj, "count", JS_NewInt32(ctx, static_cast<int32_t>(count)));
    JS_SetPropertyStr(ctx, obj, "centerUs", JS_NewInt64(ctx, centerUs));
    JS_SetPropertyStr(ctx, obj, "windowUs", JS_NewInt64(ctx, windowUs));
    return obj;
}

JSValue make_evidence_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "exportStill", JS_NewCFunction(ctx, evidence_export_still, "exportStill", 1));
    JS_SetPropertyStr(ctx, ns, "extractClip", JS_NewCFunction(ctx, evidence_extract_clip, "extractClip", 1));
    JS_SetPropertyStr(ctx, ns, "keyframeWindow", JS_NewCFunction(ctx, evidence_keyframe_window, "keyframeWindow", 1));
    return ns;
}
#endif // WL2_HAVE_QUICKJS