// wl2_ffmpeg ReplaySession implementation
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// ReplaySession Implementation
// ============================================================================

#if WL2_HAVE_QUICKJS
JSValue replay_session_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path = path_from_first_arg(ctx, argc, argv, "ReplaySession.open");
    if (path.empty()) {
        return JS_EXCEPTION;
    }
    auto handle = std::make_unique<ReplaySessionHandle>();
    handle->path = path;
    return new_replay_session(ctx, std::move(handle));
}

JSValue replay_session_extract_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }

    int64_t timestampUs = 0;
    int64_t frameIndex = 0;
    std::string formatName = "rgb24";
    std::string modeName = "accurate";
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
        if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
            JS_ToInt64(ctx, &timestampUs, timestamp);
        }
        JS_FreeValue(ctx, timestamp);
        JSValue indexValue = JS_GetPropertyStr(ctx, argv[0], "frameIndex");
        if (!JS_IsUndefined(indexValue) && !JS_IsNull(indexValue)) {
            JS_ToInt64(ctx, &frameIndex, indexValue);
        }
        JS_FreeValue(ctx, indexValue);
        get_string_prop(ctx, argv[0], "format", formatName);
        get_string_prop(ctx, argv[0], "mode", modeName);
    }

    DecodedVideoFrame frame;
    const AVPixelFormat fmt = pixel_format_from_name(formatName);
    const VideoSeekMode mode = video_seek_mode_from_name(modeName);
    int ret = decode_video_frame_modes(handle->path, timestampUs, frameIndex, mode, fmt, frame);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.extractFrame", ret);
    }
    ++handle->lastSeekId;
    handle->lastResultMode = frame.resultMode;
    return make_decoded_frame(ctx, frame);
}

JSValue replay_session_extract_audio(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }

    int64_t timestampUs = 0;
    int64_t durationUs = 100000;
    int32_t sampleRate = 48000;
    int32_t channels = 2;
    std::string formatName = "s16";
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
        if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
            JS_ToInt64(ctx, &timestampUs, timestamp);
        }
        JS_FreeValue(ctx, timestamp);
        JSValue duration = JS_GetPropertyStr(ctx, argv[0], "durationUs");
        if (!JS_IsUndefined(duration) && !JS_IsNull(duration)) {
            JS_ToInt64(ctx, &durationUs, duration);
        }
        JS_FreeValue(ctx, duration);
        get_int_prop(ctx, argv[0], "sampleRate", sampleRate);
        get_int_prop(ctx, argv[0], "channels", channels);
        get_string_prop(ctx, argv[0], "format", formatName);
    }

    DecodedAudioSamples audio;
    const AVSampleFormat fmt = sample_format_from_name(formatName);
    int ret = decode_audio_file(handle->path, timestampUs, durationUs, sampleRate, channels, fmt, audio);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.extractAudio", ret);
    }
    ++handle->lastSeekId;
    return make_decoded_audio(ctx, audio);
}

JSValue replay_session_seek(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    int64_t timestampUs = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
        if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
            JS_ToInt64(ctx, &timestampUs, timestamp);
        }
        JS_FreeValue(ctx, timestamp);
    }
    ++handle->lastSeekId;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "timestampUs", JS_NewInt64(ctx, timestampUs));
    JS_SetPropertyStr(ctx, obj, "seekId", JS_NewInt64(ctx, handle->lastSeekId));
    set_bool(ctx, obj, "queued", false);
    return obj;
}

JSValue replay_session_state(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "path", handle->path);
    JS_SetPropertyStr(ctx, obj, "lastSeekId", JS_NewInt64(ctx, handle->lastSeekId));
    set_bool(ctx, obj, "open", !handle->path.empty());
    JS_SetPropertyStr(ctx, obj, "repeatCount", JS_NewInt64(ctx, handle->repeatCount));
    JS_SetPropertyStr(ctx, obj, "dropCount", JS_NewInt64(ctx, handle->dropCount));
    JS_SetPropertyStr(ctx, obj, "publishedFrames", JS_NewInt64(ctx, handle->publishedFrames));
    JS_SetPropertyStr(ctx, obj, "lastPublishedPts", JS_NewInt64(ctx, handle->lastPublishedPts));
    set_string(ctx, obj, "lastResultMode", handle->lastResultMode);
    set_string(ctx, obj, "lastDirection", handle->lastDirection);
    JS_SetPropertyStr(ctx, obj, "lastSpeed", JS_NewFloat64(ctx, handle->lastSpeed));
    return obj;
}

JSValue replay_session_publish_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "ReplaySession.publishFrame(options) requires an options object");
    }

    std::string bufferName;
    if (!get_string_prop(ctx, argv[0], "videoBufferName", bufferName)) {
        get_string_prop(ctx, argv[0], "bufferName", bufferName);
    }
    if (bufferName.empty()) {
        return JS_ThrowTypeError(ctx, "ReplaySession.publishFrame requires videoBufferName");
    }

    int64_t timestampUs = 0;
    JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
    if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
        JS_ToInt64(ctx, &timestampUs, timestamp);
    }
    JS_FreeValue(ctx, timestamp);
    int64_t slot = 0;
    JSValue slotValue = JS_GetPropertyStr(ctx, argv[0], "slot");
    if (!JS_IsUndefined(slotValue) && !JS_IsNull(slotValue)) {
        JS_ToInt64(ctx, &slot, slotValue);
    }
    JS_FreeValue(ctx, slotValue);
    bool createBuffer = false;
    get_bool_prop(ctx, argv[0], "create", createBuffer);
    int32_t fps = 30;
    int32_t buffers = 3;
    get_int_prop(ctx, argv[0], "fps", fps);
    get_int_prop(ctx, argv[0], "buffers", buffers);

    AVPixelFormat format = AV_PIX_FMT_RGB24;
    std::string requestedFormat;
    if (get_string_prop(ctx, argv[0], "format", requestedFormat)) {
        format = pixel_format_for_membus(requestedFormat);
    }
    DecodedVideoFrame frame;
    int ret = decode_video_frame_file(handle->path, timestampUs, format, frame);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.publishFrame", ret);
    }

    wl2::VideoBuffer localVideo;
    wl2::VideoBuffer* video = nullptr;
    if (createBuffer) {
        auto found = handle->videoBuffers.find(bufferName);
        if (found == handle->videoBuffers.end()) {
            auto created = wl2::VideoBuffer::create(bufferName, frame.width, frame.height,
                video_buffer_format_from_pixel_format(format), std::max(1, fps), std::max(1, buffers));
            if (!created) {
                return throw_module_error(ctx, "ReplaySession.publishFrame", created.error().code().c_str(), created.error().message());
            }
            found = handle->videoBuffers.emplace(bufferName, std::move(created.value())).first;
        }
        video = &found->second;
    } else {
        auto opened = wl2::VideoBuffer::openExisting(bufferName);
        if (!opened) {
            return throw_module_error(ctx, "ReplaySession.publishFrame", opened.error().code().c_str(), opened.error().message());
        }
        localVideo = std::move(opened.value());
        video = &localVideo;
    }

    if (frame.width != video->width() || frame.height != video->height()) {
        return throw_module_error(ctx, "ReplaySession.publishFrame", "dimension_mismatch",
            "Decoded frame dimensions do not match target VideoBuffer");
    }

    auto view = video->frame(slot);
    if (!view) {
        return throw_module_error(ctx, "ReplaySession.publishFrame", view.error().code().c_str(), view.error().message());
    }
    const size_t bytes = std::min(view.value().size, frame.bytes.size());
    std::memcpy(view.value().data, frame.bytes.data(), bytes);
    ++handle->lastSeekId;
    const bool isRepeat = frame.pts != AV_NOPTS_VALUE && frame.pts == handle->lastPublishedPts;
    if (isRepeat) {
        ++handle->repeatCount;
    }
    handle->lastPublishedPts = frame.pts;
    handle->lastResultMode = frame.resultMode;
    ++handle->publishedFrames;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "slot", JS_NewInt64(ctx, slot));
    JS_SetPropertyStr(ctx, obj, "repeat", JS_NewBool(ctx, isRepeat));
    JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, static_cast<int64_t>(bytes)));
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, frame.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, frame.height));
    set_string(ctx, obj, "format", pixel_format_name(frame.format));
    JS_SetPropertyStr(ctx, obj, "ptsUs", JS_NewInt64(ctx, frame.ptsUs));
    JS_SetPropertyStr(ctx, obj, "seekId", JS_NewInt64(ctx, handle->lastSeekId));
    return obj;
}

JSValue replay_session_publish_audio(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "ReplaySession.publishAudio(options) requires an options object");
    }

    std::string bufferName;
    if (!get_string_prop(ctx, argv[0], "audioBufferName", bufferName)) {
        get_string_prop(ctx, argv[0], "bufferName", bufferName);
    }
    if (bufferName.empty()) {
        return JS_ThrowTypeError(ctx, "ReplaySession.publishAudio requires audioBufferName");
    }

    int64_t timestampUs = 0;
    int64_t durationUs = 100000;
    int32_t sampleRate = 48000;
    int32_t channels = 2;
    int32_t fps = 10;
    int32_t buffers = 3;
    bool createBuffer = false;
    get_bool_prop(ctx, argv[0], "create", createBuffer);
    get_int_prop(ctx, argv[0], "sampleRate", sampleRate);
    get_int_prop(ctx, argv[0], "channels", channels);
    get_int_prop(ctx, argv[0], "fps", fps);
    get_int_prop(ctx, argv[0], "buffers", buffers);
    JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
    if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
        JS_ToInt64(ctx, &timestampUs, timestamp);
    }
    JS_FreeValue(ctx, timestamp);
    JSValue duration = JS_GetPropertyStr(ctx, argv[0], "durationUs");
    if (!JS_IsUndefined(duration) && !JS_IsNull(duration)) {
        JS_ToInt64(ctx, &durationUs, duration);
    }
    JS_FreeValue(ctx, duration);

    DecodedAudioSamples audio;
    int ret = decode_audio_file(handle->path, timestampUs, durationUs, sampleRate, channels, AV_SAMPLE_FMT_S16, audio);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.publishAudio", ret);
    }

    wl2::AudioBuffer localAudio;
    wl2::AudioBuffer* buffer = nullptr;
    if (createBuffer) {
        auto found = handle->audioBuffers.find(bufferName);
        if (found == handle->audioBuffers.end()) {
            auto created = wl2::AudioBuffer::create(bufferName, channels, 16, sampleRate, std::max(1, fps), std::max(1, buffers));
            if (!created) {
                return throw_module_error(ctx, "ReplaySession.publishAudio", created.error().code().c_str(), created.error().message());
            }
            found = handle->audioBuffers.emplace(bufferName, std::move(created.value())).first;
        }
        buffer = &found->second;
    } else {
        auto opened = wl2::AudioBuffer::openExisting(bufferName);
        if (!opened) {
            return throw_module_error(ctx, "ReplaySession.publishAudio", opened.error().code().c_str(), opened.error().message());
        }
        localAudio = std::move(opened.value());
        buffer = &localAudio;
    }

    auto view = buffer->buffer(0);
    if (!view) {
        return throw_module_error(ctx, "ReplaySession.publishAudio", view.error().code().c_str(), view.error().message());
    }
    const size_t bytes = std::min(view.value().size, audio.bytes.size());
    std::memcpy(view.value().data, audio.bytes.data(), bytes);
    ++handle->lastSeekId;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "slot", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, static_cast<int64_t>(bytes)));
    JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, audio.sampleRate));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, audio.channels));
    set_string(ctx, obj, "format", sample_format_name(audio.format));
    JS_SetPropertyStr(ctx, obj, "ptsUs", JS_NewInt64(ctx, audio.ptsUs));
    JS_SetPropertyStr(ctx, obj, "seekId", JS_NewInt64(ctx, handle->lastSeekId));
    return obj;
}

JSValue replay_session_playback_plan(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    double speed = 1.0;
    bool reverse = false;
    int32_t audioWatermarkMs = 200;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue speedValue = JS_GetPropertyStr(ctx, argv[0], "speed");
        if (!JS_IsUndefined(speedValue) && !JS_IsNull(speedValue)) {
            JS_ToFloat64(ctx, &speed, speedValue);
        }
        JS_FreeValue(ctx, speedValue);
        get_bool_prop(ctx, argv[0], "reverse", reverse);
        get_int_prop(ctx, argv[0], "audioWatermarkMs", audioWatermarkMs);
    }
    if (speed <= 0.0) {
        speed = 1.0;
    }

    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, handle->path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.playbackPlan", ret);
    }
    auto closeInput = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeInput)> guard(format, closeInput);
    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.playbackPlan", ret);
    }

    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    AVRational fps = AVRational{30, 1};
    if (videoIndex >= 0) {
        AVStream* stream = format->streams[videoIndex];
        fps = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
        if (fps.num == 0 || fps.den == 0) {
            fps = AVRational{30, 1};
        }
    }
    const double sourceFps = av_q2d(fps);
    const double outputFps = sourceFps * speed;
    const double frameIntervalUs = outputFps > 0 ? 1'000'000.0 / outputFps : 0.0;

    // Audio is normal-speed and master-synced only at 1x forward; reverse and
    // high/low-speed scrub are muted by default per the resolved replay policy.
    const bool audioEnabled = audioIndex >= 0 && !reverse && std::abs(speed - 1.0) < 1e-6;
    const double driftBudgetUs = frameIntervalUs > 0 ? frameIntervalUs / 2.0 : 0.0;

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "clock", "video-master");
    set_bool(ctx, obj, "reverse", reverse);
    JS_SetPropertyStr(ctx, obj, "speed", JS_NewFloat64(ctx, speed));
    JS_SetPropertyStr(ctx, obj, "sourceFps", JS_NewFloat64(ctx, sourceFps));
    JS_SetPropertyStr(ctx, obj, "outputFps", JS_NewFloat64(ctx, outputFps));
    JS_SetPropertyStr(ctx, obj, "frameIntervalUs", JS_NewFloat64(ctx, frameIntervalUs));
    set_bool(ctx, obj, "audioEnabled", audioEnabled);
    set_bool(ctx, obj, "audioMuted", !audioEnabled);
    JS_SetPropertyStr(ctx, obj, "audioWatermarkMs", JS_NewInt32(ctx, std::clamp<int32_t>(audioWatermarkMs, 0, 5000)));
    JS_SetPropertyStr(ctx, obj, "driftBudgetUs", JS_NewFloat64(ctx, driftBudgetUs));
    set_bool(ctx, obj, "hasVideo", videoIndex >= 0);
    set_bool(ctx, obj, "hasAudio", audioIndex >= 0);
    set_string(ctx, obj, "audioMode", audioEnabled ? "synced" : (reverse ? "muted-reverse" : "muted-speed"));
    ++handle->lastSeekId;
    JS_SetPropertyStr(ctx, obj, "seekId", JS_NewInt64(ctx, handle->lastSeekId));
    return obj;
}

// Decode a window of frames ending at `untilUs` and keep the most recent
// `count`. Used by reverse playback, which has no true backward codec decode:
// it decodes forward from the prior keyframe and replays the cache in reverse.
int decode_video_window(
    const std::string& path,
    int64_t untilUs,
    int count,
    AVPixelFormat outputFormat,
    std::vector<DecodedVideoFrame>& out) {
    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return ret;
    }
    auto closeFormat = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);
    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        return ret;
    }
    ret = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        return ret;
    }
    const int streamIndex = ret;
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
    ret = avcodec_open2(decoder, codec, nullptr);
    if (ret < 0) {
        return ret;
    }

    AVRational rate = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
    const double fps = (rate.num && rate.den) ? av_q2d(rate) : 30.0;
    int64_t untilTs = AV_NOPTS_VALUE;
    if (untilUs > 0) {
        untilTs = av_rescale_q(untilUs, AVRational{1, 1000000}, stream->time_base);
        // Seek roughly `count` frames before the target so the decoded window
        // ends at the target with enough frames to replay in reverse.
        const int64_t windowUs = static_cast<int64_t>((count + 1) * (1'000'000.0 / fps));
        const int64_t seekUs = std::max<int64_t>(0, untilUs - windowUs);
        const int64_t seekTs = av_rescale_q(seekUs, AVRational{1, 1000000}, stream->time_base);
        av_seek_frame(format, streamIndex, seekTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(decoder);
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    std::vector<DecodedVideoFrame> window;
    auto pushFrame = [&]() {
        DecodedVideoFrame decoded;
        if (scale_selected_frame(frame, stream, outputFormat, decoded) == 0) {
            decoded.streamIndex = streamIndex;
            decoded.resultMode = "reverse";
            window.push_back(std::move(decoded));
            if (static_cast<int>(window.size()) > count) {
                window.erase(window.begin());
            }
        }
    };
    bool reachedTarget = false;
    while (!reachedTarget && (ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (avcodec_send_packet(decoder, packet) >= 0) {
                while ((ret = avcodec_receive_frame(decoder, frame)) >= 0) {
                    const int64_t pts = frame->best_effort_timestamp;
                    pushFrame();
                    av_frame_unref(frame);
                    if (untilTs != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts >= untilTs) {
                        reachedTarget = true;
                        break;
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    if (!reachedTarget) {
        avcodec_send_packet(decoder, nullptr);
        while (avcodec_receive_frame(decoder, frame) >= 0) {
            pushFrame();
            av_frame_unref(frame);
        }
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    if (window.empty()) {
        return AVERROR_EOF;
    }
    // Reverse so media time decreases across the returned sequence.
    out.assign(window.rbegin(), window.rend());
    return 0;
}

JSValue replay_session_reverse_window(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    int64_t timestampUs = 0;
    int32_t count = 8;
    std::string formatName = "rgb24";
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue timestamp = JS_GetPropertyStr(ctx, argv[0], "timestampUs");
        if (!JS_IsUndefined(timestamp) && !JS_IsNull(timestamp)) {
            JS_ToInt64(ctx, &timestampUs, timestamp);
        }
        JS_FreeValue(ctx, timestamp);
        get_int_prop(ctx, argv[0], "count", count);
        get_string_prop(ctx, argv[0], "format", formatName);
    }
    count = std::clamp<int32_t>(count, 1, 240);

    std::vector<DecodedVideoFrame> frames;
    const AVPixelFormat fmt = pixel_format_from_name(formatName);
    int ret = decode_video_window(handle->path, timestampUs, count, fmt, frames);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "ReplaySession.extractReverseWindow", ret);
    }
    ++handle->lastSeekId;
    handle->lastDirection = "reverse";
    handle->lastResultMode = "reverse";

    JSValue array = JS_NewArray(ctx);
    int64_t previousPts = AV_NOPTS_VALUE;
    bool decreasing = true;
    for (uint32_t i = 0; i < frames.size(); ++i) {
        if (previousPts != AV_NOPTS_VALUE && frames[i].pts != AV_NOPTS_VALUE && frames[i].pts >= previousPts) {
            decreasing = false;
        }
        previousPts = frames[i].pts;
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "pts", JS_NewInt64(ctx, frames[i].pts));
        JS_SetPropertyStr(ctx, entry, "ptsUs", JS_NewInt64(ctx, frames[i].ptsUs));
        JS_SetPropertyStr(ctx, entry, "width", JS_NewInt32(ctx, frames[i].width));
        JS_SetPropertyStr(ctx, entry, "height", JS_NewInt32(ctx, frames[i].height));
        JS_SetPropertyUint32(ctx, array, i, entry);
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "frames", array);
    JS_SetPropertyStr(ctx, obj, "count", JS_NewInt32(ctx, static_cast<int32_t>(frames.size())));
    set_bool(ctx, obj, "decreasingMediaTime", decreasing);
    JS_SetPropertyStr(ctx, obj, "firstPtsUs", JS_NewInt64(ctx, frames.front().ptsUs));
    JS_SetPropertyStr(ctx, obj, "lastPtsUs", JS_NewInt64(ctx, frames.back().ptsUs));
    JS_SetPropertyStr(ctx, obj, "seekId", JS_NewInt64(ctx, handle->lastSeekId));
    return obj;
}

// Build a paced output schedule for forward/reverse playback at a target FPS
// and speed, accounting for source frames that must be repeated (speed < 1) or
// dropped (speed > 1). Updates the session repeat/drop counters.
JSValue replay_session_play_schedule(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    int64_t startUs = 0;
    int32_t ticks = 16;
    int32_t targetFps = 30;
    double speed = 1.0;
    bool reverse = false;
    double sourceFps = 0.0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue startValue = JS_GetPropertyStr(ctx, argv[0], "startUs");
        if (!JS_IsUndefined(startValue) && !JS_IsNull(startValue)) {
            JS_ToInt64(ctx, &startUs, startValue);
        }
        JS_FreeValue(ctx, startValue);
        get_int_prop(ctx, argv[0], "ticks", ticks);
        get_int_prop(ctx, argv[0], "targetFps", targetFps);
        get_bool_prop(ctx, argv[0], "reverse", reverse);
        JSValue speedValue = JS_GetPropertyStr(ctx, argv[0], "speed");
        if (!JS_IsUndefined(speedValue) && !JS_IsNull(speedValue)) {
            JS_ToFloat64(ctx, &speed, speedValue);
        }
        JS_FreeValue(ctx, speedValue);
        JSValue sourceFpsValue = JS_GetPropertyStr(ctx, argv[0], "sourceFps");
        if (!JS_IsUndefined(sourceFpsValue) && !JS_IsNull(sourceFpsValue)) {
            JS_ToFloat64(ctx, &sourceFps, sourceFpsValue);
        }
        JS_FreeValue(ctx, sourceFpsValue);
    }
    ticks = std::clamp<int32_t>(ticks, 1, 100000);
    targetFps = std::clamp<int32_t>(targetFps, 1, 1000);
    if (speed <= 0.0) {
        speed = 1.0;
    }

    if (sourceFps <= 0.0) {
        AVFormatContext* format = nullptr;
        if (avformat_open_input(&format, handle->path.c_str(), nullptr, nullptr) >= 0) {
            if (avformat_find_stream_info(format, nullptr) >= 0) {
                const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (videoIndex >= 0) {
                    AVStream* stream = format->streams[videoIndex];
                    AVRational rate = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
                    if (rate.num && rate.den) {
                        sourceFps = av_q2d(rate);
                    }
                }
            }
            avformat_close_input(&format);
        }
        if (sourceFps <= 0.0) {
            sourceFps = 30.0;
        }
    }

    const double outputIntervalUs = 1'000'000.0 / targetFps;
    const double sourceIntervalUs = 1'000'000.0 / sourceFps;
    const double mediaAdvancePerTick = speed * outputIntervalUs * (reverse ? -1.0 : 1.0);

    JSValue array = JS_NewArray(ctx);
    int64_t repeats = 0;
    int64_t drops = 0;
    int64_t lastSourceFrame = INT64_MIN;
    double mediaTimeUs = static_cast<double>(startUs);
    for (int32_t i = 0; i < ticks; ++i) {
        const int64_t sourceFrame = static_cast<int64_t>(std::llround(mediaTimeUs / sourceIntervalUs));
        bool repeat = false;
        int64_t skipped = 0;
        if (lastSourceFrame != INT64_MIN) {
            const int64_t delta = std::llabs(sourceFrame - lastSourceFrame);
            if (delta == 0) {
                repeat = true;
                ++repeats;
            } else if (delta > 1) {
                skipped = delta - 1;
                drops += skipped;
            }
        }
        lastSourceFrame = sourceFrame;

        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "tick", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, entry, "wallClockUs", JS_NewFloat64(ctx, i * outputIntervalUs));
        JS_SetPropertyStr(ctx, entry, "mediaTimeUs", JS_NewFloat64(ctx, mediaTimeUs));
        JS_SetPropertyStr(ctx, entry, "sourceFrame", JS_NewInt64(ctx, sourceFrame));
        set_bool(ctx, entry, "repeat", repeat);
        JS_SetPropertyStr(ctx, entry, "dropped", JS_NewInt64(ctx, skipped));
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry);
        mediaTimeUs += mediaAdvancePerTick;
    }

    handle->repeatCount += repeats;
    handle->dropCount += drops;
    handle->lastDirection = reverse ? "reverse" : "forward";
    handle->lastSpeed = speed;
    ++handle->lastSeekId;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "schedule", array);
    JS_SetPropertyStr(ctx, obj, "ticks", JS_NewInt32(ctx, ticks));
    JS_SetPropertyStr(ctx, obj, "targetFps", JS_NewInt32(ctx, targetFps));
    JS_SetPropertyStr(ctx, obj, "sourceFps", JS_NewFloat64(ctx, sourceFps));
    JS_SetPropertyStr(ctx, obj, "speed", JS_NewFloat64(ctx, speed));
    set_bool(ctx, obj, "reverse", reverse);
    JS_SetPropertyStr(ctx, obj, "outputIntervalUs", JS_NewFloat64(ctx, outputIntervalUs));
    JS_SetPropertyStr(ctx, obj, "repeats", JS_NewInt64(ctx, repeats));
    JS_SetPropertyStr(ctx, obj, "drops", JS_NewInt64(ctx, drops));
    JS_SetPropertyStr(ctx, obj, "repeatCount", JS_NewInt64(ctx, handle->repeatCount));
    JS_SetPropertyStr(ctx, obj, "dropCount", JS_NewInt64(ctx, handle->dropCount));
    JS_SetPropertyStr(ctx, obj, "seekId", JS_NewInt64(ctx, handle->lastSeekId));
    return obj;
}

// Scan the primary video stream for timestamp irregularities: non-monotonic
// PTS/DTS, gaps, duplicate timestamps, and missing durations.
int analyze_timestamps_file(const std::string& path, int streamTypePreference, JSContext* ctx, JSValue out) {
    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return ret;
    }
    auto closeFormat = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);
    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        return ret;
    }
    int streamIndex = av_find_best_stream(format, static_cast<AVMediaType>(streamTypePreference), -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    if (streamIndex < 0) {
        return streamIndex;
    }
    AVStream* stream = format->streams[streamIndex];

    // Nominal per-frame step in stream ticks, derived from the frame rate. Gap
    // detection uses this instead of per-packet duration, which some containers
    // recompute from PTS deltas and thereby mask injected gaps.
    AVRational frameRate = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
    int64_t nominalStep = 1;
    if (frameRate.num && frameRate.den && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        nominalStep = av_rescale_q(1, av_inv_q(frameRate), stream->time_base);
        if (nominalStep < 1) {
            nominalStep = 1;
        }
    }

    int64_t packets = 0;
    int64_t nonMonotonicPts = 0;
    int64_t nonMonotonicDts = 0;
    int64_t duplicatePts = 0;
    int64_t gaps = 0;
    int64_t missingDuration = 0;
    int64_t backwardPts = 0;
    int64_t lastPts = AV_NOPTS_VALUE;
    int64_t lastDts = AV_NOPTS_VALUE;
    JSValue anomalies = JS_NewArray(ctx);
    uint32_t anomalyCount = 0;
    auto addAnomaly = [&](const char* kind, int64_t pts) {
        if (anomalyCount >= 32) {
            return;
        }
        JSValue entry = JS_NewObject(ctx);
        set_string(ctx, entry, "kind", kind);
        JS_SetPropertyStr(ctx, entry, "pts", JS_NewInt64(ctx, pts));
        JS_SetPropertyStr(ctx, entry, "packet", JS_NewInt64(ctx, packets));
        JS_SetPropertyUint32(ctx, anomalies, anomalyCount++, entry);
    };

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        JS_FreeValue(ctx, anomalies);
        return AVERROR(ENOMEM);
    }
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }
        ++packets;
        const int64_t pts = packet->pts;
        const int64_t dts = packet->dts;
        if (packet->duration <= 0) {
            ++missingDuration;
            addAnomaly("missing-duration", pts);
        }
        if (pts != AV_NOPTS_VALUE && lastPts != AV_NOPTS_VALUE) {
            if (pts == lastPts) {
                ++duplicatePts;
                addAnomaly("duplicate-pts", pts);
            } else if (pts < lastPts) {
                ++nonMonotonicPts;
                ++backwardPts;
                addAnomaly("backward-pts", pts);
            } else if (pts - lastPts > nominalStep + nominalStep / 2) {
                ++gaps;
                addAnomaly("gap", pts);
            }
        }
        if (dts != AV_NOPTS_VALUE && lastDts != AV_NOPTS_VALUE && dts < lastDts) {
            ++nonMonotonicDts;
            addAnomaly("non-monotonic-dts", dts);
        }
        if (pts != AV_NOPTS_VALUE) {
            lastPts = pts;
        }
        if (dts != AV_NOPTS_VALUE) {
            lastDts = dts;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    JS_SetPropertyStr(ctx, out, "streamIndex", JS_NewInt32(ctx, streamIndex));
    set_string(ctx, out, "streamType", media_type_name(stream->codecpar->codec_type));
    JS_SetPropertyStr(ctx, out, "packets", JS_NewInt64(ctx, packets));
    JS_SetPropertyStr(ctx, out, "nonMonotonicPts", JS_NewInt64(ctx, nonMonotonicPts));
    JS_SetPropertyStr(ctx, out, "nonMonotonicDts", JS_NewInt64(ctx, nonMonotonicDts));
    JS_SetPropertyStr(ctx, out, "duplicatePts", JS_NewInt64(ctx, duplicatePts));
    JS_SetPropertyStr(ctx, out, "gaps", JS_NewInt64(ctx, gaps));
    JS_SetPropertyStr(ctx, out, "backwardPts", JS_NewInt64(ctx, backwardPts));
    JS_SetPropertyStr(ctx, out, "missingDuration", JS_NewInt64(ctx, missingDuration));
    JS_SetPropertyStr(ctx, out, "anomalies", anomalies);
    return 0;
}

JSValue analyze_timestamps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "analyzeTimestamps(options) requires a path");
    }
    std::string path;
    std::string streamType = "video";
    if (JS_IsObject(argv[0])) {
        get_string_prop(ctx, argv[0], "path", path);
        get_string_prop(ctx, argv[0], "streamType", streamType);
    } else {
        path = js_string(ctx, argv[0]);
    }
    if (path.empty()) {
        return JS_ThrowTypeError(ctx, "analyzeTimestamps requires a non-empty path");
    }
    const int typePref = streamType == "audio" ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
    JSValue obj = JS_NewObject(ctx);
    int ret = analyze_timestamps_file(path, typePref, ctx, obj);
    if (ret < 0) {
        JS_FreeValue(ctx, obj);
        return throw_ffmpeg_error(ctx, "analyzeTimestamps", ret);
    }
    return obj;
}

JSValue replay_session_analyze_timestamps(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    std::string streamType = "video";
    if (argc > 0 && JS_IsObject(argv[0])) {
        get_string_prop(ctx, argv[0], "streamType", streamType);
    }
    const int typePref = streamType == "audio" ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
    JSValue obj = JS_NewObject(ctx);
    int ret = analyze_timestamps_file(handle->path, typePref, ctx, obj);
    if (ret < 0) {
        JS_FreeValue(ctx, obj);
        return throw_ffmpeg_error(ctx, "ReplaySession.analyzeTimestamps", ret);
    }
    return obj;
}

JSValue replay_session_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* handle = get_replay_session(ctx, thisVal);
    if (!handle) {
        return JS_EXCEPTION;
    }
    for (auto& item : handle->videoBuffers) {
        item.second.close();
    }
    handle->videoBuffers.clear();
    for (auto& item : handle->audioBuffers) {
        item.second.close();
    }
    handle->audioBuffers.clear();
    handle->path.clear();
    return JS_UNDEFINED;
}

JSValue make_replay_session_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "open", JS_NewCFunction(ctx, replay_session_open, "open", 1));
    return ns;
}
#endif // WL2_HAVE_QUICKJS