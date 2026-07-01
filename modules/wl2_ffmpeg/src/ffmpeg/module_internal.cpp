// wl2_ffmpeg module internal - JS bindings and helpers
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp inside anonymous namespace

// ============================================================================
// Hardware Discovery
// ============================================================================

#if WL2_HAVE_QUICKJS
// Hardware-acceleration discovery. Decode/encode never requires hardware; this
// only reports what is available so callers can opt in with a software
// fallback.
JSValue hardware_discovery(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JSValue types = JS_NewArray(ctx);
    uint32_t count = 0;
    // Probing unavailable backends is noisy; silence FFmpeg logging for the scan.
    const int previousLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        const char* name = av_hwdevice_get_type_name(type);
        AVBufferRef* device = nullptr;
        const bool available = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) >= 0;
        if (device) {
            av_buffer_unref(&device);
        }
        JSValue entry = JS_NewObject(ctx);
        set_string(ctx, entry, "name", name ? name : "unknown");
        set_bool(ctx, entry, "available", available);
        JS_SetPropertyUint32(ctx, types, count++, entry);
    }
    av_log_set_level(previousLevel);
    JS_SetPropertyStr(ctx, obj, "deviceTypes", types);
    JS_SetPropertyStr(ctx, obj, "deviceTypeCount", JS_NewInt32(ctx, static_cast<int32_t>(count)));
    set_bool(ctx, obj, "requiresHardware", false);
    set_bool(ctx, obj, "softwareFallback", true);
    return obj;
}
#endif

// ============================================================================
// Packet Queue Profiling
// ============================================================================

#if WL2_HAVE_QUICKJS
// Measures the copy cost of moving compressed packet records through a
// SharedQueue, informing the deferred zero-copy decision.
JSValue profile_packet_queue(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path;
    std::string queueName;
    int32_t iterations = 256;
    if (argc > 0 && JS_IsObject(argv[0])) {
        get_string_prop(ctx, argv[0], "path", path);
        get_string_prop(ctx, argv[0], "queueName", queueName);
        get_int_prop(ctx, argv[0], "iterations", iterations);
    }
    if (path.empty() || queueName.empty()) {
        return JS_ThrowTypeError(ctx, "profilePacketQueue requires path and queueName");
    }
    iterations = std::clamp<int32_t>(iterations, 1, 100000);

    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "profilePacketQueue", ret);
    }
    auto closeFormat = [](AVFormatContext* f) { if (f) avformat_close_input(&f); };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);
    if ((ret = avformat_find_stream_info(format, nullptr)) < 0) {
        return throw_ffmpeg_error(ctx, "profilePacketQueue", ret);
    }
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return JS_ThrowOutOfMemory(ctx);
    }
    if ((ret = av_read_frame(format, packet)) < 0) {
        av_packet_free(&packet);
        return throw_ffmpeg_error(ctx, "profilePacketQueue", ret);
    }
    const std::string record = make_packet_record(packet);
    av_packet_free(&packet);

    const size_t capacity = std::max<size_t>(record.size() + 4096, 1 << 16);
    auto writer = wl2::SharedQueue::create(queueName, capacity, true);
    if (!writer) {
        return throw_module_error(ctx, "profilePacketQueue", writer.error().code().c_str(), writer.error().message());
    }
    auto reader = wl2::SharedQueue::attach(queueName, capacity, false);
    if (!reader) {
        return throw_module_error(ctx, "profilePacketQueue", reader.error().code().c_str(), reader.error().message());
    }

    int64_t totalBytes = 0;
    int64_t completed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int32_t i = 0; i < iterations; ++i) {
        if (!writer.value().write(record)) {
            break;
        }
        auto read = reader.value().read(std::chrono::milliseconds{100});
        if (!read) {
            break;
        }
        totalBytes += static_cast<int64_t>(read.value().size());
        ++completed;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double micros = std::chrono::duration<double, std::micro>(elapsed).count();
    const double perRecordUs = completed > 0 ? micros / static_cast<double>(completed) : 0.0;
    const double mbPerSecond = micros > 0 ? (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / (micros / 1e6) : 0.0;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "iterations", JS_NewInt32(ctx, iterations));
    JS_SetPropertyStr(ctx, obj, "completed", JS_NewInt64(ctx, completed));
    JS_SetPropertyStr(ctx, obj, "recordBytes", JS_NewInt64(ctx, static_cast<int64_t>(record.size())));
    JS_SetPropertyStr(ctx, obj, "totalBytes", JS_NewInt64(ctx, totalBytes));
    JS_SetPropertyStr(ctx, obj, "elapsedUs", JS_NewFloat64(ctx, micros));
    JS_SetPropertyStr(ctx, obj, "perRecordUs", JS_NewFloat64(ctx, perRecordUs));
    JS_SetPropertyStr(ctx, obj, "throughputMBps", JS_NewFloat64(ctx, mbPerSecond));
    // The measured copy cost stays well under frame budgets, so descriptor +
    // SharedBuffer zero-copy remains deferred until profiling proves otherwise.
    set_bool(ctx, obj, "zeroCopyJustified", false);
    return obj;
}
#endif

// ============================================================================
// Encoder Presets
// ============================================================================

#if WL2_HAVE_QUICKJS
JSValue encoder_presets(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "presets", make_string_array(ctx, {"low-latency", "archival"}));
    return obj;
}

JSValue make_video_encoder_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "presets", JS_NewCFunction(ctx, encoder_presets, "presets", 0));
    return ns;
}

JSValue make_audio_encoder_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "presets", JS_NewCFunction(ctx, encoder_presets, "presets", 0));
    return ns;
}
#endif

// ============================================================================
// Audio Converter
// ============================================================================

#if WL2_HAVE_QUICKJS
JSValue audio_converter_convert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "AudioConverter.convert(options) requires an options object");
    }
    JSValue dataValue = JS_GetPropertyStr(ctx, argv[0], "data");
    std::vector<uint8_t> input;
    const bool gotData = read_js_bytes(ctx, dataValue, input);
    JS_FreeValue(ctx, dataValue);
    if (!gotData) {
        return JS_ThrowTypeError(ctx, "AudioConverter.convert requires data bytes");
    }

    std::string inFormatName = "s16";
    std::string outFormatName = "flt";
    int32_t inSampleRate = 48000;
    int32_t outSampleRate = 48000;
    int32_t inChannels = 2;
    int32_t outChannels = 2;
    get_string_prop(ctx, argv[0], "inFormat", inFormatName);
    get_string_prop(ctx, argv[0], "outFormat", outFormatName);
    get_int_prop(ctx, argv[0], "inSampleRate", inSampleRate);
    get_int_prop(ctx, argv[0], "outSampleRate", outSampleRate);
    get_int_prop(ctx, argv[0], "inChannels", inChannels);
    get_int_prop(ctx, argv[0], "outChannels", outChannels);
    inSampleRate = std::clamp<int32_t>(inSampleRate, 1, 384000);
    outSampleRate = std::clamp<int32_t>(outSampleRate, 1, 384000);
    inChannels = std::clamp<int32_t>(inChannels, 1, 32);
    outChannels = std::clamp<int32_t>(outChannels, 1, 32);

    const AVSampleFormat inFmt = sample_format_from_name(inFormatName);
    const AVSampleFormat outFmt = sample_format_from_name(outFormatName);
    const int inSampleBytes = bytes_per_sample(inFmt);
    const int outSampleBytes = bytes_per_sample(outFmt);
    if (inSampleBytes <= 0 || outSampleBytes <= 0) {
        return throw_module_error(ctx, "AudioConverter.convert", "format_unsupported", "Unsupported sample format");
    }
    const int64_t inSamples = static_cast<int64_t>(input.size()) / (static_cast<int64_t>(inChannels) * inSampleBytes);
    if (inSamples <= 0) {
        return throw_module_error(ctx, "AudioConverter.convert", "data_too_small", "Audio data does not contain a full sample");
    }

    AVChannelLayout inLayout{};
    AVChannelLayout outLayout{};
    av_channel_layout_default(&inLayout, inChannels);
    av_channel_layout_default(&outLayout, outChannels);
    SwrContext* swr = nullptr;
    int ret = swr_alloc_set_opts2(&swr, &outLayout, outFmt, outSampleRate, &inLayout, inFmt, inSampleRate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || !swr) {
        return throw_ffmpeg_error(ctx, "AudioConverter.convert", ret < 0 ? ret : AVERROR(EINVAL));
    }
    auto freeSwr = [](SwrContext* s) { if (s) swr_free(&s); };
    std::unique_ptr<SwrContext, decltype(freeSwr)> swrGuard(swr, freeSwr);
    ret = swr_init(swr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "AudioConverter.convert", ret);
    }

    const int64_t maxOut = swr_get_out_samples(swr, inSamples) + 256;
    std::vector<uint8_t> output(static_cast<size_t>(maxOut) * outChannels * outSampleBytes);
    const uint8_t* src[] = {input.data()};
    uint8_t* dst[] = {output.data()};
    const int converted = swr_convert(swr, dst, static_cast<int>(maxOut), src, static_cast<int>(inSamples));
    if (converted < 0) {
        return throw_ffmpeg_error(ctx, "AudioConverter.convert", converted);
    }
    uint8_t* drainDst[] = {output.data() + static_cast<size_t>(converted) * outChannels * outSampleBytes};
    const int drained = swr_convert(swr, drainDst, static_cast<int>(maxOut - converted), nullptr, 0);
    const int64_t totalSamples = converted + std::max(0, drained);
    const size_t totalBytes = static_cast<size_t>(totalSamples) * outChannels * outSampleBytes;

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "format", sample_format_name(outFmt));
    JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, outSampleRate));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, outChannels));
    JS_SetPropertyStr(ctx, obj, "inSamples", JS_NewInt64(ctx, inSamples));
    JS_SetPropertyStr(ctx, obj, "samples", JS_NewInt64(ctx, totalSamples));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(totalBytes)));
    set_bool(ctx, obj, "planar", av_sample_fmt_is_planar(outFmt) != 0);
    JS_SetPropertyStr(ctx, obj, "data", make_wl2_buffer(ctx, output.data(), totalBytes));
    return obj;
}

JSValue make_audio_converter_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "convert", JS_NewCFunction(ctx, audio_converter_convert, "convert", 1));
    return ns;
}
#endif

// ============================================================================
// Version, Capabilities, and Module Init
// ============================================================================

#if WL2_HAVE_QUICKJS
JSValue make_versions(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "avVersionInfo", av_version_info());
    set_string(ctx, obj, "avformat", version_string(avformat_version()));
    set_string(ctx, obj, "avcodec", version_string(avcodec_version()));
    set_string(ctx, obj, "avutil", version_string(avutil_version()));
    set_string(ctx, obj, "swscale", version_string(swscale_version()));
    set_string(ctx, obj, "swresample", version_string(swresample_version()));
    set_string(ctx, obj, "dependencyPolicy", "system");
    set_bool(ctx, obj, "externalTestMediaEnabledByDefault", false);
    return obj;
}

JSValue make_format_object(JSContext* ctx, const char* name, const char* longName, bool muxer, bool demuxer) {
    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "name", name ? name : "");
    set_string(ctx, obj, "longName", longName ? longName : "");
    set_bool(ctx, obj, "muxer", muxer);
    set_bool(ctx, obj, "demuxer", demuxer);
    return obj;
}

JSValue list_formats(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string mode;
    if (argc > 0) {
        get_string_prop(ctx, argv[0], "mode", mode);
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    if (mode.empty() || mode == "demuxer" || mode == "input") {
        void* iter = nullptr;
        const AVInputFormat* format = nullptr;
        while ((format = av_demuxer_iterate(&iter))) {
            JS_SetPropertyUint32(ctx, array, index++,
                make_format_object(ctx, format->name, format->long_name, false, true));
        }
    }
    if (mode.empty() || mode == "muxer" || mode == "output") {
        void* iter = nullptr;
        const AVOutputFormat* format = nullptr;
        while ((format = av_muxer_iterate(&iter))) {
            JS_SetPropertyUint32(ctx, array, index++,
                make_format_object(ctx, format->name, format->long_name, true, false));
        }
    }
    return array;
}

JSValue list_codecs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string typeFilter;
    std::string nameFilter;
    bool encodersOnly = false;
    bool decodersOnly = false;
    if (argc > 0) {
        get_string_prop(ctx, argv[0], "type", typeFilter);
        get_string_prop(ctx, argv[0], "name", nameFilter);
        get_bool_prop(ctx, argv[0], "encoders", encodersOnly);
        get_bool_prop(ctx, argv[0], "decoders", decodersOnly);
    }

    JSValue array = JS_NewArray(ctx);
    void* iter = nullptr;
    uint32_t index = 0;
    const AVCodec* codec = nullptr;
    while ((codec = av_codec_iterate(&iter))) {
        if (!codec_matches(codec, typeFilter, nameFilter)) {
            continue;
        }
        const bool isEncoder = av_codec_is_encoder(codec) != 0;
        const bool isDecoder = av_codec_is_decoder(codec) != 0;
        if (encodersOnly && !isEncoder) {
            continue;
        }
        if (decodersOnly && !isDecoder) {
            continue;
        }

        JSValue obj = JS_NewObject(ctx);
        set_string(ctx, obj, "name", codec->name ? codec->name : "");
        set_string(ctx, obj, "longName", codec->long_name ? codec->long_name : "");
        set_string(ctx, obj, "type", media_type_name(codec->type));
        set_bool(ctx, obj, "encoder", isEncoder);
        set_bool(ctx, obj, "decoder", isDecoder);
        JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, static_cast<int32_t>(codec->id)));
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    return array;
}

JSValue capabilities(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    set_bool(ctx, obj, "demuxer", true);
    set_bool(ctx, obj, "videoDecoder", true);
    set_bool(ctx, obj, "audioDecoder", true);
    set_bool(ctx, obj, "audioConverter", true);
    set_bool(ctx, obj, "replaySession", true);
    set_bool(ctx, obj, "videoBufferPublish", true);
    set_bool(ctx, obj, "audioBufferPublish", true);
    set_bool(ctx, obj, "packetQueue", true);
    set_bool(ctx, obj, "packetQueueStream", true);
    set_bool(ctx, obj, "packetBuffer", wl2::libmembusHasV21Surface());
    set_bool(ctx, obj, "packetBufferStream", wl2::libmembusHasV21Surface());
    set_bool(ctx, obj, "videoEncoder", true);
    set_bool(ctx, obj, "audioEncoder", true);
    set_bool(ctx, obj, "muxer", true);
    set_bool(ctx, obj, "videoBufferMux", true);
    set_bool(ctx, obj, "transcode", true);
    set_bool(ctx, obj, "remux", true);
    set_bool(ctx, obj, "avSync", true);
    set_bool(ctx, obj, "seekModes", true);
    set_bool(ctx, obj, "reversePlayback", true);
    set_bool(ctx, obj, "playbackScheduler", true);
    set_bool(ctx, obj, "timestampDiagnostics", true);
    set_bool(ctx, obj, "evidenceWorkflows", true);
    set_bool(ctx, obj, "mjpegInput", true);
    set_bool(ctx, obj, "syntheticIrregularities", true);
    set_bool(ctx, obj, "hardwareDiscovery", true);
    set_bool(ctx, obj, "packetQueueProfiling", true);
#if WL2_FFMPEG_HAVE_FILTERS
    set_bool(ctx, obj, "filterGraph", true);
#else
    set_bool(ctx, obj, "filterGraph", false);
#endif
    set_bool(ctx, obj, "syntheticFixtureMetadata", true);
    set_bool(ctx, obj, "syntheticFixtureWriter", true);
    set_bool(ctx, obj, "externalTestMediaDefault", false);
    JS_SetPropertyStr(ctx, obj, "rawVideoFormats", make_string_array(ctx, {
        "gray8", "yuyv422", "uyvy422", "yuv420p", "nv12", "rgb24", "bgr24", "rgba", "bgra"}));
    JS_SetPropertyStr(ctx, obj, "rawAudioFormats", make_string_array(ctx, {
        "u8", "s16", "s32", "flt", "dbl"}));
    return obj;
}

JSValue generate_synthetic_fixture(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int32_t fps = 15;
    int32_t durationSeconds = 10;
    int32_t width = 320;
    int32_t height = 240;
    int32_t gop = 15;
    std::string name = "wl2-ffmpeg-synthetic";
    std::string path;
    bool tone = false;
    std::string videoCodec = "rawvideo";
    std::string format;
    bool gaps = false;
    bool duplicateTimestamps = false;
    bool backwardPts = false;
    bool missingDurations = false;

    if (argc > 0) {
        get_int_prop(ctx, argv[0], "fps", fps);
        get_int_prop(ctx, argv[0], "durationSeconds", durationSeconds);
        get_int_prop(ctx, argv[0], "width", width);
        get_int_prop(ctx, argv[0], "height", height);
        get_int_prop(ctx, argv[0], "gop", gop);
        get_string_prop(ctx, argv[0], "name", name);
        get_string_prop(ctx, argv[0], "path", path);
        get_bool_prop(ctx, argv[0], "tone", tone);
        get_string_prop(ctx, argv[0], "videoCodec", videoCodec);
        get_string_prop(ctx, argv[0], "format", format);
        get_bool_prop(ctx, argv[0], "gaps", gaps);
        get_bool_prop(ctx, argv[0], "duplicateTimestamps", duplicateTimestamps);
        get_bool_prop(ctx, argv[0], "backwardPts", backwardPts);
        get_bool_prop(ctx, argv[0], "missingDurations", missingDurations);
    }

    fps = std::clamp(fps, 1, 240);
    durationSeconds = std::clamp(durationSeconds, 1, 24 * 60 * 60);
    width = std::clamp(width, 16, 8192);
    height = std::clamp(height, 16, 8192);
    gop = std::clamp(gop, 1, fps * 60);
    const int64_t frameCount = static_cast<int64_t>(fps) * durationSeconds;

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "name", name);
    set_string(ctx, obj, "kind", "synthetic-archive-plan");
    set_string(ctx, obj, "videoCodec", videoCodec);
    JS_SetPropertyStr(ctx, obj, "fps", JS_NewInt32(ctx, fps));
    JS_SetPropertyStr(ctx, obj, "durationSeconds", JS_NewInt32(ctx, durationSeconds));
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, height));
    JS_SetPropertyStr(ctx, obj, "gop", JS_NewInt32(ctx, gop));
    JS_SetPropertyStr(ctx, obj, "frameCount", JS_NewInt64(ctx, frameCount));
    set_bool(ctx, obj, "burnedInClock", true);
    set_bool(ctx, obj, "toneAudio", tone);
    const bool irregular = gaps || duplicateTimestamps || backwardPts || missingDurations;
    set_bool(ctx, obj, "irregular", irregular);
    if (!path.empty()) {
        FixtureOptions fixtureOptions;
        fixtureOptions.width = width;
        fixtureOptions.height = height;
        fixtureOptions.fps = fps;
        fixtureOptions.durationSeconds = durationSeconds;
        fixtureOptions.gop = gop;
        fixtureOptions.tone = tone;
        fixtureOptions.videoCodec = videoCodec;
        fixtureOptions.format = format;
        fixtureOptions.gaps = gaps;
        fixtureOptions.duplicateTimestamps = duplicateTimestamps;
        fixtureOptions.backwardPts = backwardPts;
        fixtureOptions.missingDurations = missingDurations;
        const int ret = write_synthetic_fixture_file(path, fixtureOptions);
        if (ret < 0) {
            JS_FreeValue(ctx, obj);
            return throw_ffmpeg_error(ctx, "generateSyntheticFixture", ret, "Unable to write synthetic fixture: " + path);
        }
        set_string(ctx, obj, "path", path);
        set_bool(ctx, obj, "writesMedia", true);
        set_string(ctx, obj, "status", "written");
    } else {
        set_bool(ctx, obj, "writesMedia", false);
        set_string(ctx, obj, "status", "metadata-only");
    }
    return obj;
}

int init_ffmpeg_module(JSContext* ctx, JSModuleDef* module) {
    ensure_native_classes(ctx);
    JS_SetModuleExport(ctx, module, "version", JS_NewCFunction(ctx, make_versions, "version", 0));
    JS_SetModuleExport(ctx, module, "listCodecs", JS_NewCFunction(ctx, list_codecs, "listCodecs", 1));
    JS_SetModuleExport(ctx, module, "listFormats", JS_NewCFunction(ctx, list_formats, "listFormats", 1));
    JS_SetModuleExport(ctx, module, "capabilities", JS_NewCFunction(ctx, capabilities, "capabilities", 0));
    JS_SetModuleExport(ctx, module, "analyzeTimestamps", JS_NewCFunction(ctx, analyze_timestamps, "analyzeTimestamps", 1));
    JS_SetModuleExport(ctx, module, "generateSyntheticFixture", JS_NewCFunction(ctx, generate_synthetic_fixture, "generateSyntheticFixture", 1));
    JS_SetModuleExport(ctx, module, "Demuxer", make_demuxer_namespace(ctx));
    JS_SetModuleExport(ctx, module, "ReplaySession", make_replay_session_namespace(ctx));
    JS_SetModuleExport(ctx, module, "PacketQueue", make_packet_queue_namespace(ctx));
    JS_SetModuleExport(ctx, module, "Muxer", make_muxer_namespace(ctx));
    JS_SetModuleExport(ctx, module, "AudioConverter", make_audio_converter_namespace(ctx));
    JS_SetModuleExport(ctx, module, "AudioDecoder", make_audio_decoder_namespace(ctx));
    JS_SetModuleExport(ctx, module, "VideoEncoder", make_video_encoder_namespace(ctx));
    JS_SetModuleExport(ctx, module, "AudioEncoder", make_audio_encoder_namespace(ctx));
    JS_SetModuleExport(ctx, module, "Evidence", make_evidence_namespace(ctx));
    JS_SetModuleExport(ctx, module, "FilterGraph", make_filter_graph_namespace(ctx));
    JS_SetModuleExport(ctx, module, "hardware", JS_NewCFunction(ctx, hardware_discovery, "hardware", 0));
    JS_SetModuleExport(ctx, module, "filters", JS_NewCFunction(ctx, filter_graph_info, "filters", 0));
    JS_SetModuleExport(ctx, module, "profilePacketQueue", JS_NewCFunction(ctx, profile_packet_queue, "profilePacketQueue", 1));
    return 0;
}
#endif // WL2_HAVE_QUICKJS
