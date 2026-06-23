// wl2_ffmpeg video and audio decoding implementation
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// Video Decoding
// ============================================================================

// Scale one decoded frame into the requested output pixel format and copy its
// timing metadata into `out`.
int scale_selected_frame(AVFrame* selected, AVStream* stream, AVPixelFormat outputFormat, DecodedVideoFrame& out) {
    const int outSize = av_image_get_buffer_size(outputFormat, selected->width, selected->height, 1);
    if (outSize < 0) {
        return outSize;
    }
    out.bytes.resize(static_cast<size_t>(outSize));
    uint8_t* dstData[4] = {};
    int dstLinesize[4] = {};
    int ret = av_image_fill_arrays(dstData, dstLinesize, out.bytes.data(), outputFormat, selected->width, selected->height, 1);
    if (ret < 0) {
        return ret;
    }
    SwsContext* sws = sws_getContext(selected->width, selected->height, static_cast<AVPixelFormat>(selected->format),
        selected->width, selected->height, outputFormat, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        return AVERROR(EINVAL);
    }
    sws_scale(sws, selected->data, selected->linesize, 0, selected->height, dstData, dstLinesize);
    sws_freeContext(sws);

    out.width = selected->width;
    out.height = selected->height;
    out.format = outputFormat;
    out.pts = selected->best_effort_timestamp;
    out.ptsUs = timestamp_to_us(out.pts, stream->time_base);
    out.duration = selected->duration;
    out.keyframe = (selected->flags & AV_FRAME_FLAG_KEY) != 0;
    return 0;
}

// Decode a single frame using one of the supported seek strategies. `fast`
// returns the keyframe at/before the target without decoding forward;
// `accurate` decodes forward to the frame whose interval contains the target;
// `nearest` returns whichever decoded frame is closest in time; `frame` steps
// to an explicit frame index from the start of the stream.
int decode_video_frame_modes(
    const std::string& path,
    int64_t timestampUs,
    int64_t frameIndex,
    VideoSeekMode mode,
    AVPixelFormat outputFormat,
    DecodedVideoFrame& out) {
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
    ret = avcodec_parameters_to_context(decoder, stream->codecpar);
    if (ret < 0) {
        return ret;
    }
    ret = avcodec_open2(decoder, codec, nullptr);
    if (ret < 0) {
        return ret;
    }

    int64_t targetTs = AV_NOPTS_VALUE;
    if (mode != VideoSeekMode::Frame && timestampUs > 0) {
        targetTs = av_rescale_q(timestampUs, AVRational{1, 1000000}, stream->time_base);
        ret = av_seek_frame(format, streamIndex, targetTs, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            return ret;
        }
        avcodec_flush_buffers(decoder);
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* selected = av_frame_alloc();
    AVFrame* previous = av_frame_alloc();
    if (!packet || !frame || !selected || !previous) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&selected);
        av_frame_free(&previous);
        return AVERROR(ENOMEM);
    }
    auto cleanup = [&]() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&selected);
        av_frame_free(&previous);
    };

    bool found = false;
    bool havePrevious = false;
    int64_t decodedCount = 0;
    std::string resultMode = "exact";

    auto consider = [&](AVFrame* candidate) -> bool {
        const int64_t pts = candidate->best_effort_timestamp;
        switch (mode) {
            case VideoSeekMode::Frame: {
                if (decodedCount == frameIndex) {
                    av_frame_ref(selected, candidate);
                    resultMode = "frame";
                    return true;
                }
                ++decodedCount;
                return false;
            }
            case VideoSeekMode::Fast: {
                av_frame_ref(selected, candidate);
                resultMode = "keyframe";
                return true;
            }
            case VideoSeekMode::Nearest: {
                if (targetTs == AV_NOPTS_VALUE || pts == AV_NOPTS_VALUE) {
                    av_frame_ref(selected, candidate);
                    resultMode = "nearest";
                    return true;
                }
                if (pts >= targetTs) {
                    if (havePrevious) {
                        const int64_t prevPts = previous->best_effort_timestamp;
                        if (std::llabs(prevPts - targetTs) <= std::llabs(pts - targetTs)) {
                            av_frame_ref(selected, previous);
                        } else {
                            av_frame_ref(selected, candidate);
                        }
                    } else {
                        av_frame_ref(selected, candidate);
                    }
                    resultMode = "nearest";
                    return true;
                }
                av_frame_unref(previous);
                av_frame_ref(previous, candidate);
                havePrevious = true;
                return false;
            }
            case VideoSeekMode::Accurate:
            default: {
                if (targetTs == AV_NOPTS_VALUE || pts == AV_NOPTS_VALUE || pts >= targetTs) {
                    av_frame_ref(selected, candidate);
                    const int64_t dur = candidate->duration > 0 ? candidate->duration : 1;
                    resultMode = (pts == AV_NOPTS_VALUE || (pts <= targetTs + dur && pts >= targetTs))
                        ? "exact" : "nearest-after";
                    if (targetTs == AV_NOPTS_VALUE) {
                        resultMode = "exact";
                    }
                    return true;
                }
                return false;
            }
        }
    };

    while (!found && (ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == streamIndex) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                av_packet_unref(packet);
                break;
            }
            while ((ret = avcodec_receive_frame(decoder, frame)) >= 0) {
                if (consider(frame)) {
                    found = true;
                    av_frame_unref(frame);
                    break;
                }
                av_frame_unref(frame);
            }
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                ret = 0;
            }
        }
        av_packet_unref(packet);
    }

    if (!found) {
        ret = avcodec_send_packet(decoder, nullptr);
        while (ret >= 0 && (ret = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (consider(frame)) {
                found = true;
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);
        }
    }

    // Nearest/frame fall back to the last decoded frame if the target ran past EOF.
    if (!found && havePrevious) {
        av_frame_ref(selected, previous);
        resultMode = mode == VideoSeekMode::Nearest ? "nearest" : "clamped-eof";
        found = true;
    }

    if (!found) {
        cleanup();
        return ret < 0 ? ret : AVERROR_EOF;
    }

    ret = scale_selected_frame(selected, stream, outputFormat, out);
    out.streamIndex = streamIndex;
    out.resultMode = resultMode;
    cleanup();
    return ret;
}

int decode_video_frame_file(
    const std::string& path,
    int64_t timestampUs,
    AVPixelFormat outputFormat,
    DecodedVideoFrame& out) {
    return decode_video_frame_modes(path, timestampUs, 0, VideoSeekMode::Accurate, outputFormat, out);
}

#if WL2_HAVE_QUICKJS
JSValue make_decoded_frame(JSContext* ctx, const DecodedVideoFrame& frame) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, frame.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, frame.height));
    set_string(ctx, obj, "format", pixel_format_name(frame.format));
    JS_SetPropertyStr(ctx, obj, "pts", JS_NewInt64(ctx, frame.pts));
    JS_SetPropertyStr(ctx, obj, "ptsUs", JS_NewInt64(ctx, frame.ptsUs));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, frame.duration));
    JS_SetPropertyStr(ctx, obj, "streamIndex", JS_NewInt32(ctx, frame.streamIndex));
    set_string(ctx, obj, "resultMode", frame.resultMode);
    set_bool(ctx, obj, "keyframe", frame.keyframe);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(frame.bytes.size())));
    JS_SetPropertyStr(ctx, obj, "data", make_wl2_buffer(ctx, frame.bytes.data(), frame.bytes.size()));
    return obj;
}
#endif

// ============================================================================
// Audio Decoding
// ============================================================================

int decode_audio_file(
    const std::string& path,
    int64_t timestampUs,
    int64_t durationUs,
    int outputSampleRate,
    int outputChannels,
    AVSampleFormat outputFormat,
    DecodedAudioSamples& out) {
    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return ret;
    }
    auto closeFormat = [](AVFormatContext* f) {
        if (f) {
            avformat_close_input(&f);
        }
    };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);

    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        return ret;
    }
    ret = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
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
    auto freeDecoder = [](AVCodecContext* c) {
        if (c) {
            avcodec_free_context(&c);
        }
    };
    std::unique_ptr<AVCodecContext, decltype(freeDecoder)> decoderGuard(decoder, freeDecoder);
    ret = avcodec_parameters_to_context(decoder, stream->codecpar);
    if (ret < 0) {
        return ret;
    }
    ret = avcodec_open2(decoder, codec, nullptr);
    if (ret < 0) {
        return ret;
    }

    out.sourceFormat = decoder->sample_fmt;
    out.sourceSampleRate = decoder->sample_rate;
    out.sourceChannels = decoder->ch_layout.nb_channels;
    out.sourcePlanar = av_sample_fmt_is_planar(decoder->sample_fmt) != 0;

    if (outputSampleRate <= 0) {
        outputSampleRate = decoder->sample_rate;
    }
    if (outputChannels <= 0) {
        outputChannels = decoder->ch_layout.nb_channels > 0 ? decoder->ch_layout.nb_channels : 2;
    }
    if (durationUs <= 0) {
        durationUs = 100000;
    }

    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, outputChannels);
    SwrContext* swr = nullptr;
    ret = swr_alloc_set_opts2(
        &swr,
        &outLayout,
        outputFormat,
        outputSampleRate,
        &decoder->ch_layout,
        decoder->sample_fmt,
        decoder->sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || !swr) {
        return ret < 0 ? ret : AVERROR(EINVAL);
    }
    auto freeSwr = [](SwrContext* s) {
        if (s) {
            swr_free(&s);
        }
    };
    std::unique_ptr<SwrContext, decltype(freeSwr)> swrGuard(swr, freeSwr);
    ret = swr_init(swr);
    if (ret < 0) {
        return ret;
    }

    int64_t targetTs = AV_NOPTS_VALUE;
    if (timestampUs > 0) {
        targetTs = av_rescale_q(timestampUs, AVRational{1, 1000000}, stream->time_base);
        ret = av_seek_frame(format, streamIndex, targetTs, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            return ret;
        }
        avcodec_flush_buffers(decoder);
    }

    const int64_t wantedSamples = std::max<int64_t>(1, av_rescale(durationUs, outputSampleRate, 1000000));
    const int sampleBytes = bytes_per_sample(outputFormat);
    if (sampleBytes <= 0) {
        return AVERROR(EINVAL);
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    int64_t writtenSamples = 0;
    bool sawAudio = false;
    while (writtenSamples < wantedSamples && (ret = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == streamIndex) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                av_packet_unref(packet);
                break;
            }
            while (writtenSamples < wantedSamples && (ret = avcodec_receive_frame(decoder, frame)) >= 0) {
                const int64_t pts = frame->best_effort_timestamp;
                if (targetTs == AV_NOPTS_VALUE || pts == AV_NOPTS_VALUE || pts >= targetTs) {
                    if (!sawAudio) {
                        out.pts = pts;
                        out.ptsUs = timestamp_to_us(pts, stream->time_base);
                        sawAudio = true;
                    }
                    const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
                    std::vector<uint8_t> chunk(static_cast<size_t>(maxOut) * outputChannels * sampleBytes);
                    uint8_t* dst[] = {chunk.data()};
                    const int converted = swr_convert(
                        swr,
                        dst,
                        maxOut,
                        const_cast<const uint8_t**>(frame->extended_data),
                        frame->nb_samples);
                    if (converted < 0) {
                        av_packet_free(&packet);
                        av_frame_free(&frame);
                        return converted;
                    }
                    const int64_t remaining = wantedSamples - writtenSamples;
                    const int64_t takeSamples = std::min<int64_t>(converted, remaining);
                    const size_t takeBytes = static_cast<size_t>(takeSamples) * outputChannels * sampleBytes;
                    out.bytes.insert(out.bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(takeBytes));
                    writtenSamples += takeSamples;
                }
                av_frame_unref(frame);
            }
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                ret = 0;
            }
        }
        av_packet_unref(packet);
    }

    if (!sawAudio) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ret < 0 ? ret : AVERROR_EOF;
    }

    out.sampleRate = outputSampleRate;
    out.channels = outputChannels;
    out.format = outputFormat;
    out.samples = writtenSamples;
    out.streamIndex = streamIndex;

    av_packet_free(&packet);
    av_frame_free(&frame);
    return 0;
}

#if WL2_HAVE_QUICKJS
JSValue make_decoded_audio(JSContext* ctx, const DecodedAudioSamples& audio) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, audio.sampleRate));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, audio.channels));
    set_string(ctx, obj, "format", sample_format_name(audio.format));
    JS_SetPropertyStr(ctx, obj, "pts", JS_NewInt64(ctx, audio.pts));
    JS_SetPropertyStr(ctx, obj, "ptsUs", JS_NewInt64(ctx, audio.ptsUs));
    JS_SetPropertyStr(ctx, obj, "samples", JS_NewInt64(ctx, audio.samples));
    JS_SetPropertyStr(ctx, obj, "streamIndex", JS_NewInt32(ctx, audio.streamIndex));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(audio.bytes.size())));
    JS_SetPropertyStr(ctx, obj, "data", make_wl2_buffer(ctx, audio.bytes.data(), audio.bytes.size()));
    return obj;
}

JSValue audio_decoder_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "AudioDecoder.decode(options) requires an options object");
    }
    std::string path;
    get_string_prop(ctx, argv[0], "path", path);
    if (path.empty()) {
        return JS_ThrowTypeError(ctx, "AudioDecoder.decode requires a path");
    }
    int64_t timestampUs = 0;
    int64_t durationUs = 100000;
    int32_t sampleRate = 0;
    int32_t channels = 0;
    std::string formatName = "s16";
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

    DecodedAudioSamples audio;
    const AVSampleFormat fmt = sample_format_from_name(formatName);
    int ret = decode_audio_file(path, timestampUs, durationUs, sampleRate, channels, fmt, audio);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "AudioDecoder.decode", ret);
    }
    JSValue obj = make_decoded_audio(ctx, audio);
    set_string(ctx, obj, "sourceFormat", sample_format_name(audio.sourceFormat));
    JS_SetPropertyStr(ctx, obj, "sourceSampleRate", JS_NewInt32(ctx, audio.sourceSampleRate));
    JS_SetPropertyStr(ctx, obj, "sourceChannels", JS_NewInt32(ctx, audio.sourceChannels));
    set_bool(ctx, obj, "sourcePlanar", audio.sourcePlanar);
    return obj;
}

JSValue make_audio_decoder_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "decode", JS_NewCFunction(ctx, audio_decoder_decode, "decode", 1));
    return ns;
}
#endif // WL2_HAVE_QUICKJS