// wl2_ffmpeg Muxer, transcode, remux, and packet queue implementation
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// Synthetic Fixture Writer
// ============================================================================

// Writes a deterministic synthetic archive. The video stream keeps strictly
// monotonic DTS (so any muxer accepts it) and monotonic keyframe PTS, while
// non-keyframe packets can carry the requested PTS irregularities; explicit-
// timestamp containers such as NUT preserve those for diagnostics. rawvideo and
// MJPEG fixtures write encoded frames; irregular fixtures use an inter-frame
// codec (mpeg4) so non-keyframe packets exist to perturb.
int write_synthetic_fixture_file(const std::string& path, const FixtureOptions& options) {
    const int width = options.width;
    const int height = options.height;
    const int fps = options.fps;
    const bool irregular = options.gaps || options.duplicateTimestamps
        || options.backwardPts || options.missingDurations;

    std::string videoCodec = options.videoCodec;
    if (irregular && (videoCodec == "rawvideo" || videoCodec.empty() || videoCodec == "mjpeg")) {
        // MPEG-TS + mpeg2video preserves exact written PTS/DTS in stream order
        // (no PTS reordering), so injected irregularities survive readback.
        videoCodec = "mpeg2video";
    }
    const bool encoded = videoCodec != "rawvideo";
    const bool mjpeg = videoCodec == "mjpeg";

    AVFormatContext* fmt = nullptr;
    const char* formatName = !options.format.empty() ? options.format.c_str()
        : (irregular ? "mpegts" : nullptr);
    int ret = avformat_alloc_output_context2(&fmt, nullptr, formatName, path.c_str());
    if (ret < 0 || !fmt) {
        return ret < 0 ? ret : AVERROR_UNKNOWN;
    }
    auto freeFmt = [](AVFormatContext* f) {
        if (f) {
            if (f->pb && !(f->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&f->pb);
            }
            avformat_free_context(f);
        }
    };
    std::unique_ptr<AVFormatContext, decltype(freeFmt)> fmtGuard(fmt, freeFmt);

    AVStream* videoStream = avformat_new_stream(fmt, nullptr);
    if (!videoStream) {
        return AVERROR(ENOMEM);
    }
    videoStream->time_base = AVRational{1, fps};
    videoStream->avg_frame_rate = AVRational{fps, 1};
    videoStream->r_frame_rate = AVRational{fps, 1};

    AVCodecContext* venc = nullptr;
    SwsContext* sws = nullptr;
    auto freeVenc = [](AVCodecContext* c) { if (c) avcodec_free_context(&c); };
    std::unique_ptr<AVCodecContext, decltype(freeVenc)> vencGuard(nullptr, freeVenc);
    AVPixelFormat encPixFmt = AV_PIX_FMT_YUV420P;
    if (encoded) {
        const AVCodec* codec = avcodec_find_encoder_by_name(videoCodec.c_str());
        if (!codec) {
            return AVERROR_ENCODER_NOT_FOUND;
        }
        venc = avcodec_alloc_context3(codec);
        vencGuard.reset(venc);
        encPixFmt = encoder_first_pixel_format(codec, mjpeg ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P);
        venc->width = width;
        venc->height = height;
        venc->pix_fmt = encPixFmt;
        venc->time_base = AVRational{1, fps};
        venc->framerate = AVRational{fps, 1};
        venc->max_b_frames = 0; // keep DTS == encode order so timestamps stay simple
        venc->gop_size = mjpeg ? 1 : std::max(2, options.gop);
        if (mjpeg) {
            venc->color_range = AVCOL_RANGE_JPEG;
        }
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) {
            venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        ret = avcodec_open2(venc, codec, nullptr);
        if (ret < 0) {
            return ret;
        }
        avcodec_parameters_from_context(videoStream->codecpar, venc);
        sws = sws_getContext(width, height, AV_PIX_FMT_BGR24, width, height, encPixFmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            return AVERROR(EINVAL);
        }
    } else {
        videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        videoStream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        videoStream->codecpar->format = AV_PIX_FMT_BGR24;
        videoStream->codecpar->width = width;
        videoStream->codecpar->height = height;
        videoStream->codecpar->bits_per_coded_sample = 24;
    }

    constexpr int audioSampleRate = 48000;
    constexpr int audioChannels = 2;
    AVStream* audioStream = nullptr;
    if (options.tone) {
        audioStream = avformat_new_stream(fmt, nullptr);
        if (!audioStream) {
            return AVERROR(ENOMEM);
        }
        audioStream->time_base = AVRational{1, audioSampleRate};
        audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        audioStream->codecpar->format = AV_SAMPLE_FMT_S16;
        audioStream->codecpar->sample_rate = audioSampleRate;
        av_channel_layout_default(&audioStream->codecpar->ch_layout, audioChannels);
        audioStream->codecpar->bits_per_coded_sample = 16;
        audioStream->codecpar->block_align = audioChannels * 2;
        audioStream->codecpar->bit_rate = audioSampleRate * audioChannels * 16;
    }

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            return ret;
        }
    }
    ret = avformat_write_header(fmt, nullptr);
    if (ret < 0) {
        return ret;
    }

    const int frameCount = fps * options.durationSeconds;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> audioBytes;
    int audioCursor = 0;
    AVFrame* encFrame = av_frame_alloc();
    AVPacket* encPacket = av_packet_alloc();
    auto cleanup = [&]() {
        if (sws) sws_freeContext(sws);
        av_frame_free(&encFrame);
        av_packet_free(&encPacket);
    };

    // Assigns DTS/PTS to each emitted video packet. DTS and keyframe PTS stay
    // monotonic; non-keyframe PTS carries the requested irregularities.
    int64_t outIndex = 0;
    auto emit_video_packet = [&](AVPacket* pkt) -> int {
        const bool isKey = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        const int64_t k = outIndex++;
        int64_t pts = k;
        int64_t duration = 1;
        if (irregular && !isKey) {
            if (options.duplicateTimestamps && k > 0 && (k % 10) == 0) {
                pts = k + 1; // equals the next packet's natural PTS -> duplicate
            } else if (options.gaps && k > 0 && (k % 8) == 0) {
                pts = k + 3; // forward jump leaves a gap and a backward recovery
            } else if (options.backwardPts && k > 1 && (k % 7) == 0) {
                pts = k + 2; // forward jump yields a later non-monotonic step
            }
        }
        if (options.missingDurations && (k % 5) == 0) {
            duration = 0;
        }
        pkt->stream_index = videoStream->index;
        pkt->pts = pts;
        pkt->dts = k;
        pkt->duration = duration;
        return av_interleaved_write_frame(fmt, pkt);
    };

    for (int i = 0; i < frameCount; ++i) {
        fill_bgr_frame(bytes, width, height, i, fps);

        if (encoded) {
            uint8_t* srcData[4] = {bytes.data(), nullptr, nullptr, nullptr};
            int srcLinesize[4] = {width * 3, 0, 0, 0};
            av_frame_unref(encFrame);
            encFrame->format = encPixFmt;
            encFrame->width = width;
            encFrame->height = height;
            if (av_frame_get_buffer(encFrame, 0) < 0) {
                ret = AVERROR(ENOMEM);
                break;
            }
            sws_scale(sws, srcData, srcLinesize, 0, height, encFrame->data, encFrame->linesize);
            encFrame->pts = i;
            ret = avcodec_send_frame(venc, encFrame);
            if (ret < 0) {
                break;
            }
            while ((ret = avcodec_receive_packet(venc, encPacket)) >= 0) {
                ret = emit_video_packet(encPacket);
                av_packet_unref(encPacket);
                if (ret < 0) {
                    break;
                }
            }
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            }
            if (ret < 0) {
                break;
            }
        } else {
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) {
                ret = AVERROR(ENOMEM);
                break;
            }
            pkt->data = bytes.data();
            pkt->size = static_cast<int>(bytes.size());
            pkt->flags = AV_PKT_FLAG_KEY;
            ret = emit_video_packet(pkt);
            pkt->data = nullptr;
            pkt->size = 0;
            av_packet_free(&pkt);
            if (ret < 0) {
                break;
            }
        }

        if (audioStream) {
            const int nextAudioCursor = static_cast<int>(av_rescale_q(i + 1, AVRational{1, fps}, AVRational{1, audioSampleRate}));
            const int sampleCount = std::max(0, nextAudioCursor - audioCursor);
            fill_s16_tone(audioBytes, audioSampleRate, audioChannels, audioCursor, sampleCount);
            AVPacket* audioPkt = av_packet_alloc();
            if (!audioPkt) {
                ret = AVERROR(ENOMEM);
                break;
            }
            audioPkt->stream_index = audioStream->index;
            audioPkt->data = audioBytes.data();
            audioPkt->size = static_cast<int>(audioBytes.size());
            audioPkt->pts = audioCursor;
            audioPkt->dts = audioCursor;
            audioPkt->duration = sampleCount;
            audioPkt->flags = AV_PKT_FLAG_KEY;
            ret = av_interleaved_write_frame(fmt, audioPkt);
            audioPkt->data = nullptr;
            audioPkt->size = 0;
            av_packet_free(&audioPkt);
            audioCursor = nextAudioCursor;
            if (ret < 0) {
                break;
            }
        }
    }

    if (ret >= 0 && encoded) {
        avcodec_send_frame(venc, nullptr);
        while (avcodec_receive_packet(venc, encPacket) >= 0) {
            ret = emit_video_packet(encPacket);
            av_packet_unref(encPacket);
            if (ret < 0) {
                break;
            }
        }
    }
    if (ret >= 0) {
        ret = av_write_trailer(fmt);
    }
    cleanup();
    return ret;
}

// ============================================================================
// Transcode
// ============================================================================

int transcode_file(
    const std::string& inPath,
    const std::string& outPath,
    const std::string& wantVideoCodec,
    const std::string& wantAudioCodec,
    const std::string& preset,
    bool includeAudio,
    TranscodeResult& result,
    int64_t startUs,
    int64_t endUs) {
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
    int audioIndex = includeAudio ? av_find_best_stream(inFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
    const int64_t startTsVideo = startUs > 0
        ? av_rescale_q(startUs, AVRational{1, 1000000}, inFmt->streams[videoIndex]->time_base) : 0;
    const int64_t endTsVideo = endUs < INT64_MAX
        ? av_rescale_q(endUs, AVRational{1, 1000000}, inFmt->streams[videoIndex]->time_base) : INT64_MAX;
    if (startUs > 0) {
        av_seek_frame(inFmt, videoIndex, startTsVideo, AVSEEK_FLAG_BACKWARD);
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
    result.container = outFmt->oformat->name ? outFmt->oformat->name : "";

    // Video decoder.
    AVStream* inVideo = inFmt->streams[videoIndex];
    const AVCodec* vDecCodec = avcodec_find_decoder(inVideo->codecpar->codec_id);
    if (!vDecCodec) {
        return AVERROR_DECODER_NOT_FOUND;
    }
    AVCodecContext* vDec = avcodec_alloc_context3(vDecCodec);
    if (!vDec) {
        return AVERROR(ENOMEM);
    }
    auto freeCtx = [](AVCodecContext* c) { if (c) avcodec_free_context(&c); };
    std::unique_ptr<AVCodecContext, decltype(freeCtx)> vDecGuard(vDec, freeCtx);
    avcodec_parameters_to_context(vDec, inVideo->codecpar);
    ret = avcodec_open2(vDec, vDecCodec, nullptr);
    if (ret < 0) {
        return ret;
    }

    const AVCodec* vEncCodec = find_encoder_preference(video_encoder_candidates(wantVideoCodec));
    if (!vEncCodec) {
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AVCodecContext* vEnc = avcodec_alloc_context3(vEncCodec);
    if (!vEnc) {
        return AVERROR(ENOMEM);
    }
    std::unique_ptr<AVCodecContext, decltype(freeCtx)> vEncGuard(vEnc, freeCtx);
    AVRational fps = inVideo->avg_frame_rate.num ? inVideo->avg_frame_rate : inVideo->r_frame_rate;
    if (fps.num == 0 || fps.den == 0) {
        fps = AVRational{30, 1};
    }
    vEnc->width = inVideo->codecpar->width;
    vEnc->height = inVideo->codecpar->height;
    vEnc->pix_fmt = encoder_first_pixel_format(vEncCodec, AV_PIX_FMT_YUV420P);
    vEnc->time_base = av_inv_q(fps);
    vEnc->framerate = fps;
    apply_video_preset(vEnc, preset);
    if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
        vEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ret = avcodec_open2(vEnc, vEncCodec, nullptr);
    if (ret < 0) {
        return ret;
    }
    AVStream* outVideo = avformat_new_stream(outFmt, nullptr);
    if (!outVideo) {
        return AVERROR(ENOMEM);
    }
    outVideo->time_base = vEnc->time_base;
    avcodec_parameters_from_context(outVideo->codecpar, vEnc);
    result.videoCodec = vEncCodec->name ? vEncCodec->name : "";
    result.videoPixelFormat = pixel_format_name(vEnc->pix_fmt);
    result.width = vEnc->width;
    result.height = vEnc->height;
    result.preset = preset.empty() ? "low-latency" : preset;

    // Audio decoder/encoder (optional).
    AVCodecContext* aDec = nullptr;
    AVCodecContext* aEnc = nullptr;
    SwrContext* aSwr = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVStream* outAudio = nullptr;
    std::unique_ptr<AVCodecContext, decltype(freeCtx)> aDecGuard(nullptr, freeCtx);
    std::unique_ptr<AVCodecContext, decltype(freeCtx)> aEncGuard(nullptr, freeCtx);
    auto freeSwr = [](SwrContext* s) { if (s) swr_free(&s); };
    std::unique_ptr<SwrContext, decltype(freeSwr)> aSwrGuard(nullptr, freeSwr);
    auto freeFifo = [](AVAudioFifo* f) { if (f) av_audio_fifo_free(f); };
    std::unique_ptr<AVAudioFifo, decltype(freeFifo)> fifoGuard(nullptr, freeFifo);

    const AVCodec* aEncCodec = nullptr;
    if (audioIndex >= 0) {
        aEncCodec = find_encoder_preference(audio_encoder_candidates(wantAudioCodec));
    }
    if (audioIndex >= 0 && aEncCodec) {
        AVStream* inAudio = inFmt->streams[audioIndex];
        const AVCodec* aDecCodec = avcodec_find_decoder(inAudio->codecpar->codec_id);
        if (aDecCodec) {
            aDec = avcodec_alloc_context3(aDecCodec);
            aDecGuard.reset(aDec);
            avcodec_parameters_to_context(aDec, inAudio->codecpar);
            if (avcodec_open2(aDec, aDecCodec, nullptr) >= 0) {
                aEnc = avcodec_alloc_context3(aEncCodec);
                aEncGuard.reset(aEnc);
                aEnc->sample_fmt = encoder_first_sample_format(aEncCodec, AV_SAMPLE_FMT_S16);
                aEnc->sample_rate = encoder_pick_sample_rate(aEncCodec, aDec->sample_rate > 0 ? aDec->sample_rate : 48000);
                const int outChannels = aDec->ch_layout.nb_channels > 0 ? aDec->ch_layout.nb_channels : 2;
                av_channel_layout_default(&aEnc->ch_layout, outChannels);
                aEnc->bit_rate = 128'000;
                aEnc->time_base = AVRational{1, aEnc->sample_rate};
                if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
                    aEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }
                if (avcodec_open2(aEnc, aEncCodec, nullptr) >= 0) {
                    ret = swr_alloc_set_opts2(&aSwr, &aEnc->ch_layout, aEnc->sample_fmt, aEnc->sample_rate,
                        &aDec->ch_layout, aDec->sample_fmt, aDec->sample_rate, 0, nullptr);
                    aSwrGuard.reset(aSwr);
                    if (ret >= 0 && aSwr && swr_init(aSwr) >= 0) {
                        fifo = av_audio_fifo_alloc(aEnc->sample_fmt, outChannels, 1);
                        fifoGuard.reset(fifo);
                        outAudio = avformat_new_stream(outFmt, nullptr);
                        if (outAudio) {
                            outAudio->time_base = aEnc->time_base;
                            avcodec_parameters_from_context(outAudio->codecpar, aEnc);
                            result.audioCodec = aEncCodec->name ? aEncCodec->name : "";
                            result.audioSampleFormat = sample_format_name(aEnc->sample_fmt);
                        }
                    }
                }
            }
        }
        if (!outAudio) {
            // Audio path could not be fully configured; continue video-only.
            aDecGuard.reset();
            aEncGuard.reset();
            aSwrGuard.reset();
            fifoGuard.reset();
            aDec = nullptr;
            aEnc = nullptr;
            aSwr = nullptr;
            fifo = nullptr;
            audioIndex = -1;
        }
    } else {
        audioIndex = -1;
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

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVPacket* encPacket = av_packet_alloc();
    SwsContext* sws = nullptr;
    AVFrame* vFrame = av_frame_alloc();
    AVFrame* aFrame = av_frame_alloc();
    if (!packet || !frame || !encPacket || !vFrame || !aFrame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_packet_free(&encPacket);
        av_frame_free(&vFrame);
        av_frame_free(&aFrame);
        return AVERROR(ENOMEM);
    }
    int64_t nextVideoPts = 0;
    int64_t nextAudioPts = 0;

    auto writeEncoded = [&](AVCodecContext* enc, AVStream* stream, int64_t& counter) -> int {
        int drainRet = 0;
        while ((drainRet = avcodec_receive_packet(enc, encPacket)) >= 0) {
            encPacket->stream_index = stream->index;
            av_packet_rescale_ts(encPacket, enc->time_base, stream->time_base);
            drainRet = av_interleaved_write_frame(outFmt, encPacket);
            av_packet_unref(encPacket);
            if (drainRet < 0) {
                return drainRet;
            }
            ++counter;
        }
        if (drainRet == AVERROR(EAGAIN) || drainRet == AVERROR_EOF) {
            return 0;
        }
        return drainRet;
    };

    auto drainFifo = [&](bool flush) -> int {
        const int frameSize = aEnc->frame_size > 0 ? aEnc->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= frameSize || (flush && av_audio_fifo_size(fifo) > 0)) {
            const int take = std::min(frameSize, av_audio_fifo_size(fifo));
            av_frame_unref(aFrame);
            aFrame->nb_samples = take;
            aFrame->format = aEnc->sample_fmt;
            av_channel_layout_copy(&aFrame->ch_layout, &aEnc->ch_layout);
            aFrame->sample_rate = aEnc->sample_rate;
            int allocRet = av_frame_get_buffer(aFrame, 0);
            if (allocRet < 0) {
                return allocRet;
            }
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(aFrame->data), take) < take) {
                return AVERROR_UNKNOWN;
            }
            aFrame->pts = nextAudioPts;
            nextAudioPts += take;
            int sendRet = avcodec_send_frame(aEnc, aFrame);
            if (sendRet < 0) {
                return sendRet;
            }
            sendRet = writeEncoded(aEnc, outAudio, result.audioPackets);
            if (sendRet < 0) {
                return sendRet;
            }
        }
        return 0;
    };

    bool rangeDone = false;
    while (!rangeDone && (ret = av_read_frame(inFmt, packet)) >= 0) {
        if (packet->stream_index == videoIndex) {
            ret = avcodec_send_packet(vDec, packet);
            if (ret >= 0 || ret == AVERROR(EAGAIN)) {
                while ((ret = avcodec_receive_frame(vDec, frame)) >= 0) {
                    const int64_t framePts = frame->best_effort_timestamp;
                    if (framePts != AV_NOPTS_VALUE && framePts < startTsVideo) {
                        av_frame_unref(frame);
                        continue;
                    }
                    if (framePts != AV_NOPTS_VALUE && framePts > endTsVideo) {
                        av_frame_unref(frame);
                        rangeDone = true;
                        break;
                    }
                    if (!sws) {
                        sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                            vEnc->width, vEnc->height, vEnc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    }
                    av_frame_unref(vFrame);
                    vFrame->format = vEnc->pix_fmt;
                    vFrame->width = vEnc->width;
                    vFrame->height = vEnc->height;
                    if (av_frame_get_buffer(vFrame, 0) < 0 || !sws) {
                        av_frame_unref(frame);
                        continue;
                    }
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, vFrame->data, vFrame->linesize);
                    vFrame->pts = nextVideoPts++;
                    int sendRet = avcodec_send_frame(vEnc, vFrame);
                    if (sendRet >= 0) {
                        writeEncoded(vEnc, outVideo, result.videoPackets);
                    }
                    av_frame_unref(frame);
                }
            }
        } else if (audioIndex >= 0 && packet->stream_index == audioIndex) {
            ret = avcodec_send_packet(aDec, packet);
            if (ret >= 0 || ret == AVERROR(EAGAIN)) {
                while ((ret = avcodec_receive_frame(aDec, frame)) >= 0) {
                    const int maxOut = swr_get_out_samples(aSwr, frame->nb_samples);
                    std::vector<uint8_t*> dst(aEnc->ch_layout.nb_channels);
                    AVFrame* converted = av_frame_alloc();
                    converted->format = aEnc->sample_fmt;
                    converted->sample_rate = aEnc->sample_rate;
                    converted->nb_samples = maxOut;
                    av_channel_layout_copy(&converted->ch_layout, &aEnc->ch_layout);
                    if (av_frame_get_buffer(converted, 0) >= 0) {
                        const int got = swr_convert(aSwr, converted->data, maxOut,
                            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                        if (got > 0) {
                            av_audio_fifo_write(fifo, reinterpret_cast<void**>(converted->data), got);
                        }
                    }
                    av_frame_free(&converted);
                    av_frame_unref(frame);
                    drainFifo(false);
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush video encoder.
    avcodec_send_frame(vEnc, nullptr);
    writeEncoded(vEnc, outVideo, result.videoPackets);
    if (audioIndex >= 0) {
        drainFifo(true);
        avcodec_send_frame(aEnc, nullptr);
        writeEncoded(aEnc, outAudio, result.audioPackets);
    }

    int writeRet = av_write_trailer(outFmt);
    if (sws) {
        sws_freeContext(sws);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_packet_free(&encPacket);
    av_frame_free(&vFrame);
    av_frame_free(&aFrame);
    if (writeRet < 0) {
        return writeRet;
    }

    // Reopen to validate metadata round-trip.
    outGuard.reset();
    AVFormatContext* verify = nullptr;
    if (avformat_open_input(&verify, outPath.c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(verify, nullptr) >= 0) {
            result.reopened = true;
            result.reopenedStreams = static_cast<int>(verify->nb_streams);
            for (unsigned i = 0; i < verify->nb_streams; ++i) {
                const AVCodecParameters* par = verify->streams[i]->codecpar;
                if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    result.reopenedWidth = par->width;
                    result.reopenedHeight = par->height;
                } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                    result.reopenedHasAudio = true;
                    result.reopenedSampleRate = par->sample_rate;
                }
            }
        }
        avformat_close_input(&verify);
    }
    return 0;
}

// ============================================================================
// Remux
// ============================================================================

int remux_file(const std::string& inPath, const std::string& outPath, RemuxResult& result) {
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
    result.container = outFmt->oformat->name ? outFmt->oformat->name : "";

    std::vector<int> streamMap(inFmt->nb_streams, -1);
    int videoStreamIndex = -1;
    for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
        AVStream* inStream = inFmt->streams[i];
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO
            && inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* outStream = avformat_new_stream(outFmt, nullptr);
        if (!outStream) {
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (ret < 0) {
            return ret;
        }
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;
        streamMap[i] = outStream->index;
        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex < 0) {
            videoStreamIndex = static_cast<int>(i);
            result.sourceExtradataSize = inStream->codecpar->extradata_size;
        }
    }
    result.streams = outFmt->nb_streams;

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

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return AVERROR(ENOMEM);
    }
    while ((ret = av_read_frame(inFmt, packet)) >= 0) {
        const int mapped = (packet->stream_index >= 0 && packet->stream_index < static_cast<int>(streamMap.size()))
            ? streamMap[packet->stream_index]
            : -1;
        if (mapped < 0) {
            av_packet_unref(packet);
            continue;
        }
        if (static_cast<int>(packet->stream_index) == videoStreamIndex) {
            if (result.firstVideoPts == AV_NOPTS_VALUE) {
                result.firstVideoPts = packet->pts;
            }
            result.lastVideoPts = packet->pts;
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
        ++result.packets;
    }
    av_packet_free(&packet);
    int writeRet = av_write_trailer(outFmt);
    outGuard.reset();
    if (ret < 0 && ret != AVERROR_EOF) {
        return ret;
    }
    if (writeRet < 0) {
        return writeRet;
    }

    AVFormatContext* verify = nullptr;
    if (avformat_open_input(&verify, outPath.c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(verify, nullptr) >= 0) {
            for (unsigned i = 0; i < verify->nb_streams; ++i) {
                if (verify->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    result.outputExtradataSize = verify->streams[i]->codecpar->extradata_size;
                    break;
                }
            }
        }
        avformat_close_input(&verify);
    }
    result.extradataPreserved = result.outputExtradataSize == result.sourceExtradataSize;
    return 0;
}

// ============================================================================
// Packet Queue Streaming
// ============================================================================

// Drives a producer/consumer compressed-packet path: every demuxed packet is
// written to a SharedQueue as a sequenced record, then drained and re-muxed.
// `dropOldestUntilKeyframe` models live-preview backpressure by dropping
// non-keyframe packets once the in-flight depth exceeds maxDepth.
int stream_through_queue(
    const std::string& inPath,
    const std::string& outPath,
    const std::string& queueName,
    size_t queueSize,
    const std::string& policy,
    int maxDepth,
    QueueRemuxResult& result) {
    result.policy = policy;
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
    result.remux.container = outFmt->oformat->name ? outFmt->oformat->name : "";

    std::vector<int> streamMap(inFmt->nb_streams, -1);
    std::vector<AVRational> sourceTimeBase(inFmt->nb_streams, AVRational{0, 1});
    int videoStreamIndex = -1;
    for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
        AVStream* inStream = inFmt->streams[i];
        sourceTimeBase[i] = inStream->time_base;
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO
            && inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* outStream = avformat_new_stream(outFmt, nullptr);
        if (!outStream) {
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (ret < 0) {
            return ret;
        }
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;
        streamMap[i] = outStream->index;
        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex < 0) {
            videoStreamIndex = static_cast<int>(i);
            result.remux.sourceExtradataSize = inStream->codecpar->extradata_size;
        }
    }
    result.remux.streams = outFmt->nb_streams;

    // Stream configuration record validates that consumers can carry enough
    // state to rebuild a muxer without re-reading the source.
    const std::string configRecord = make_stream_config_record(inFmt);

    auto writer = wl2::SharedQueue::create(queueName, queueSize, true);
    if (!writer) {
        return AVERROR_EXTERNAL;
    }
    auto reader = wl2::SharedQueue::attach(queueName, queueSize, false);
    if (!reader) {
        return AVERROR_EXTERNAL;
    }
    // Configuration record travels first so a fresh consumer sees stream layout.
    if (!writer.value().write(configRecord)) {
        return AVERROR_EXTERNAL;
    }
    {
        auto configRead = reader.value().read(std::chrono::milliseconds{100});
        if (!configRead || configRead.value().compare(0, 7, "WL2FSC1") != 0) {
            return AVERROR_INVALIDDATA;
        }
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

    const bool dropPolicy = policy == "dropOldestUntilKeyframe";
    uint64_t sequence = 0;
    int inFlight = 0;
    bool pendingDiscontinuity = false;
    int64_t firstVideoSourcePts = AV_NOPTS_VALUE;
    int64_t lastVideoSourcePts = AV_NOPTS_VALUE;
    int64_t firstWrittenVideoPts = AV_NOPTS_VALUE;
    bool comparedFirstPayload = false;

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return AVERROR(ENOMEM);
    }

    auto consume_one = [&](int /*timeoutMs*/) -> int {
        auto record = reader.value().read(std::chrono::milliseconds{100});
        if (!record) {
            return -1;
        }
        const std::string& payload = record.value();
        constexpr size_t kHeaderSize = 60;
        if (payload.size() < kHeaderSize || payload.compare(0, 7, "WL2FQP2") != 0) {
            return -1;
        }
        ++result.recordsRead;
        --inFlight;
        const int streamIndex = static_cast<int>(read_u32(payload.data() + 12));
        const int64_t pts = read_i64(payload.data() + 16);
        const int64_t dts = read_i64(payload.data() + 24);
        const int64_t duration = read_i64(payload.data() + 32);
        const uint32_t flags = read_u32(payload.data() + 40);
        const uint32_t size = read_u32(payload.data() + 44);
        const uint32_t discontinuity = read_u32(payload.data() + 56);
        if (discontinuity) {
            ++result.discontinuities;
        }
        const char* data = payload.data() + kHeaderSize;
        if (payload.size() < kHeaderSize + size) {
            return -1;
        }
        const int mapped = (streamIndex >= 0 && streamIndex < static_cast<int>(streamMap.size()))
            ? streamMap[streamIndex] : -1;
        if (mapped < 0) {
            return 0;
        }
        AVPacket* outPacket = av_packet_alloc();
        if (!outPacket) {
            return AVERROR(ENOMEM);
        }
        if (av_new_packet(outPacket, static_cast<int>(size)) < 0) {
            av_packet_free(&outPacket);
            return AVERROR(ENOMEM);
        }
        std::memcpy(outPacket->data, data, size);
        outPacket->stream_index = mapped;
        outPacket->pts = pts;
        outPacket->dts = dts;
        outPacket->duration = duration;
        outPacket->flags = static_cast<int>(flags);
        if (streamIndex == videoStreamIndex && firstWrittenVideoPts == AV_NOPTS_VALUE) {
            firstWrittenVideoPts = pts;
        }
        const AVRational tb = sourceTimeBase[streamIndex];
        AVStream* outStream = outFmt->streams[mapped];
        av_packet_rescale_ts(outPacket, tb, outStream->time_base);
        outPacket->pos = -1;
        int writeRet = av_interleaved_write_frame(outFmt, outPacket);
        av_packet_free(&outPacket);
        return writeRet;
    };

    while ((ret = av_read_frame(inFmt, packet)) >= 0) {
        const int mapped = (packet->stream_index >= 0 && packet->stream_index < static_cast<int>(streamMap.size()))
            ? streamMap[packet->stream_index] : -1;
        if (mapped < 0) {
            av_packet_unref(packet);
            continue;
        }
        ++result.packetsRead;
        const bool isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        if (static_cast<int>(packet->stream_index) == videoStreamIndex) {
            if (firstVideoSourcePts == AV_NOPTS_VALUE) {
                firstVideoSourcePts = packet->pts;
            }
            lastVideoSourcePts = packet->pts;
        }

        if (dropPolicy && inFlight >= maxDepth && !isKeyframe) {
            // Backpressure: drop this packet and flag the next surviving one.
            ++result.dropped;
            pendingDiscontinuity = true;
            av_packet_unref(packet);
            continue;
        }

        const bool discontinuity = pendingDiscontinuity;
        pendingDiscontinuity = false;
        const std::string record = make_sequenced_packet_record(packet, sequence++, discontinuity);
        if (!comparedFirstPayload) {
            comparedFirstPayload = true;
            result.payloadPreserved = record.size() == 60 + static_cast<size_t>(packet->size)
                && std::memcmp(record.data() + 60, packet->data, packet->size) == 0;
        }
        if (!writer.value().write(record)) {
            av_packet_unref(packet);
            break;
        }
        ++result.recordsWritten;
        ++inFlight;
        av_packet_unref(packet);

        // Drain when depth allows, keeping the queue bounded.
        const int drainTo = dropPolicy ? std::max(1, maxDepth / 2) : 1;
        while (inFlight >= drainTo) {
            int consumeRet = consume_one(100);
            if (consumeRet < 0) {
                break;
            }
        }
    }
    av_packet_free(&packet);

    // Drain remaining records.
    while (inFlight > 0) {
        int consumeRet = consume_one(100);
        if (consumeRet < 0) {
            break;
        }
    }

    int writeRet = av_write_trailer(outFmt);
    outGuard.reset();
    if (writeRet < 0) {
        return writeRet;
    }

    if (firstVideoSourcePts != AV_NOPTS_VALUE && firstWrittenVideoPts != AV_NOPTS_VALUE) {
        result.timestampsPreserved = firstVideoSourcePts == firstWrittenVideoPts;
    }
    result.remux.firstVideoPts = firstWrittenVideoPts;
    result.remux.lastVideoPts = lastVideoSourcePts;
    result.remux.packets = result.recordsRead;

    AVFormatContext* verify = nullptr;
    if (avformat_open_input(&verify, outPath.c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(verify, nullptr) >= 0) {
            for (unsigned i = 0; i < verify->nb_streams; ++i) {
                if (verify->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    result.remux.outputExtradataSize = verify->streams[i]->codecpar->extradata_size;
                    break;
                }
            }
        }
        avformat_close_input(&verify);
    }
    result.remux.extradataPreserved = result.remux.outputExtradataSize == result.remux.sourceExtradataSize;
    return 0;
}

// ============================================================================
// JS Bindings
// ============================================================================

#if WL2_HAVE_QUICKJS
JSValue muxer_transcode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Muxer.transcode(options) requires an options object");
    }
    std::string inputPath;
    std::string outputPath;
    std::string videoCodec;
    std::string audioCodec;
    std::string preset = "low-latency";
    bool includeAudio = true;
    get_string_prop(ctx, argv[0], "path", inputPath);
    if (inputPath.empty()) {
        get_string_prop(ctx, argv[0], "inputPath", inputPath);
    }
    get_string_prop(ctx, argv[0], "outputPath", outputPath);
    get_string_prop(ctx, argv[0], "videoCodec", videoCodec);
    get_string_prop(ctx, argv[0], "audioCodec", audioCodec);
    get_string_prop(ctx, argv[0], "preset", preset);
    get_bool_prop(ctx, argv[0], "audio", includeAudio);
    if (inputPath.empty() || outputPath.empty()) {
        return JS_ThrowTypeError(ctx, "Muxer.transcode requires path and outputPath");
    }

    TranscodeResult result;
    int ret = transcode_file(inputPath, outputPath, videoCodec, audioCodec, preset, includeAudio, result, 0, INT64_MAX);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Muxer.transcode", ret);
    }

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "container", result.container);
    set_string(ctx, obj, "videoCodec", result.videoCodec);
    set_string(ctx, obj, "audioCodec", result.audioCodec);
    set_string(ctx, obj, "videoPixelFormat", result.videoPixelFormat);
    set_string(ctx, obj, "audioSampleFormat", result.audioSampleFormat);
    set_string(ctx, obj, "preset", result.preset);
    set_string(ctx, obj, "outputPath", outputPath);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, result.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, result.height));
    JS_SetPropertyStr(ctx, obj, "videoPackets", JS_NewInt64(ctx, result.videoPackets));
    JS_SetPropertyStr(ctx, obj, "audioPackets", JS_NewInt64(ctx, result.audioPackets));
    set_bool(ctx, obj, "reopened", result.reopened);
    JS_SetPropertyStr(ctx, obj, "reopenedStreams", JS_NewInt32(ctx, result.reopenedStreams));
    JS_SetPropertyStr(ctx, obj, "reopenedWidth", JS_NewInt32(ctx, result.reopenedWidth));
    JS_SetPropertyStr(ctx, obj, "reopenedHeight", JS_NewInt32(ctx, result.reopenedHeight));
    set_bool(ctx, obj, "reopenedHasAudio", result.reopenedHasAudio);
    JS_SetPropertyStr(ctx, obj, "reopenedSampleRate", JS_NewInt32(ctx, result.reopenedSampleRate));
    return obj;
}

JSValue muxer_remux(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Muxer.remux(options) requires an options object");
    }
    std::string inputPath;
    std::string outputPath;
    get_string_prop(ctx, argv[0], "path", inputPath);
    if (inputPath.empty()) {
        get_string_prop(ctx, argv[0], "inputPath", inputPath);
    }
    get_string_prop(ctx, argv[0], "outputPath", outputPath);
    if (inputPath.empty() || outputPath.empty()) {
        return JS_ThrowTypeError(ctx, "Muxer.remux requires path and outputPath");
    }

    RemuxResult result;
    int ret = remux_file(inputPath, outputPath, result);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "Muxer.remux", ret);
    }
    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "container", result.container);
    set_string(ctx, obj, "outputPath", outputPath);
    JS_SetPropertyStr(ctx, obj, "streams", JS_NewInt32(ctx, result.streams));
    JS_SetPropertyStr(ctx, obj, "packets", JS_NewInt64(ctx, result.packets));
    JS_SetPropertyStr(ctx, obj, "firstVideoPts", JS_NewInt64(ctx, result.firstVideoPts));
    JS_SetPropertyStr(ctx, obj, "lastVideoPts", JS_NewInt64(ctx, result.lastVideoPts));
    set_bool(ctx, obj, "extradataPreserved", result.extradataPreserved);
    JS_SetPropertyStr(ctx, obj, "sourceExtradataSize", JS_NewInt32(ctx, result.sourceExtradataSize));
    JS_SetPropertyStr(ctx, obj, "outputExtradataSize", JS_NewInt32(ctx, result.outputExtradataSize));
    return obj;
}

JSValue packet_queue_round_trip_first_packet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "PacketQueue.roundTripFirstPacket(options) requires an options object");
    }
    std::string path;
    std::string queueName;
    int32_t queueSize = 65536;
    get_string_prop(ctx, argv[0], "path", path);
    get_string_prop(ctx, argv[0], "queueName", queueName);
    get_int_prop(ctx, argv[0], "queueSize", queueSize);
    if (path.empty() || queueName.empty()) {
        return JS_ThrowTypeError(ctx, "PacketQueue.roundTripFirstPacket requires path and queueName");
    }

    AVFormatContext* format = nullptr;
    int ret = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "PacketQueue.roundTripFirstPacket", ret);
    }
    auto closeFormat = [](AVFormatContext* f) {
        if (f) {
            avformat_close_input(&f);
        }
    };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatGuard(format, closeFormat);
    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "PacketQueue.roundTripFirstPacket", ret);
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return JS_ThrowOutOfMemory(ctx);
    }
    ret = av_read_frame(format, packet);
    if (ret < 0) {
        av_packet_free(&packet);
        return throw_ffmpeg_error(ctx, "PacketQueue.roundTripFirstPacket", ret);
    }
    const std::string record = make_packet_record(packet);
    const int streamIndex = packet->stream_index;
    const int packetSize = packet->size;
    const int64_t pts = packet->pts;
    const int flags = packet->flags;
    av_packet_free(&packet);

    const size_t capacity = static_cast<size_t>(std::max<int32_t>(queueSize, static_cast<int32_t>(record.size() + 1024)));
    auto writer = wl2::SharedQueue::create(queueName, capacity, true);
    if (!writer) {
        return throw_module_error(ctx, "PacketQueue.roundTripFirstPacket", writer.error().code().c_str(), writer.error().message());
    }
    auto reader = wl2::SharedQueue::attach(queueName, capacity, false);
    if (!reader) {
        return throw_module_error(ctx, "PacketQueue.roundTripFirstPacket", reader.error().code().c_str(), reader.error().message());
    }
    auto written = writer.value().write(record);
    if (!written) {
        return throw_module_error(ctx, "PacketQueue.roundTripFirstPacket", written.error().code().c_str(), written.error().message());
    }
    auto read = reader.value().read(std::chrono::milliseconds{100});
    if (!read) {
        return throw_module_error(ctx, "PacketQueue.roundTripFirstPacket", read.error().code().c_str(), read.error().message());
    }
    const std::string& payload = read.value();
    if (payload.size() < 48 || payload.compare(0, 7, "WL2FQP1") != 0) {
        return throw_module_error(ctx, "PacketQueue.roundTripFirstPacket", "packet_record_invalid", "Invalid compressed packet record");
    }

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "magic", "WL2FQP1");
    JS_SetPropertyStr(ctx, obj, "version", JS_NewUint32(ctx, read_u32(payload.data() + 8)));
    JS_SetPropertyStr(ctx, obj, "streamIndex", JS_NewInt32(ctx, static_cast<int32_t>(read_u32(payload.data() + 12))));
    JS_SetPropertyStr(ctx, obj, "pts", JS_NewInt64(ctx, read_i64(payload.data() + 16)));
    JS_SetPropertyStr(ctx, obj, "dts", JS_NewInt64(ctx, read_i64(payload.data() + 24)));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, read_i64(payload.data() + 32)));
    JS_SetPropertyStr(ctx, obj, "flags", JS_NewUint32(ctx, read_u32(payload.data() + 40)));
    JS_SetPropertyStr(ctx, obj, "packetSize", JS_NewUint32(ctx, read_u32(payload.data() + 44)));
    JS_SetPropertyStr(ctx, obj, "recordSize", JS_NewInt64(ctx, static_cast<int64_t>(payload.size())));
    JS_SetPropertyStr(ctx, obj, "sourceStreamIndex", JS_NewInt32(ctx, streamIndex));
    JS_SetPropertyStr(ctx, obj, "sourcePacketSize", JS_NewInt32(ctx, packetSize));
    JS_SetPropertyStr(ctx, obj, "sourcePts", JS_NewInt64(ctx, pts));
    JS_SetPropertyStr(ctx, obj, "sourceFlags", JS_NewInt32(ctx, flags));
    return obj;
}

JSValue packet_queue_stream_through(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "PacketQueue.streamThroughQueue(options) requires an options object");
    }
    std::string inputPath;
    std::string outputPath;
    std::string queueName;
    std::string policy = "lossless";
    int32_t queueSize = 1 << 20;
    int32_t maxDepth = 8;
    get_string_prop(ctx, argv[0], "path", inputPath);
    if (inputPath.empty()) {
        get_string_prop(ctx, argv[0], "inputPath", inputPath);
    }
    get_string_prop(ctx, argv[0], "outputPath", outputPath);
    get_string_prop(ctx, argv[0], "queueName", queueName);
    get_string_prop(ctx, argv[0], "policy", policy);
    get_int_prop(ctx, argv[0], "queueSize", queueSize);
    get_int_prop(ctx, argv[0], "maxDepth", maxDepth);
    if (inputPath.empty() || outputPath.empty() || queueName.empty()) {
        return JS_ThrowTypeError(ctx, "PacketQueue.streamThroughQueue requires path, outputPath, and queueName");
    }
    if (policy != "lossless" && policy != "dropOldestUntilKeyframe") {
        return throw_module_error(ctx, "PacketQueue.streamThroughQueue", "policy_invalid",
            "policy must be lossless or dropOldestUntilKeyframe");
    }
    maxDepth = std::clamp<int32_t>(maxDepth, 1, 1024);

    QueueRemuxResult result;
    int ret = stream_through_queue(inputPath, outputPath, queueName,
        static_cast<size_t>(std::max<int32_t>(queueSize, 65536)), policy, maxDepth, result);
    if (ret < 0) {
        return throw_ffmpeg_error(ctx, "PacketQueue.streamThroughQueue", ret);
    }

    JSValue obj = JS_NewObject(ctx);
    set_string(ctx, obj, "policy", result.policy);
    set_string(ctx, obj, "container", result.remux.container);
    set_string(ctx, obj, "outputPath", outputPath);
    JS_SetPropertyStr(ctx, obj, "packetsRead", JS_NewInt64(ctx, result.packetsRead));
    JS_SetPropertyStr(ctx, obj, "recordsWritten", JS_NewInt64(ctx, result.recordsWritten));
    JS_SetPropertyStr(ctx, obj, "recordsRead", JS_NewInt64(ctx, result.recordsRead));
    JS_SetPropertyStr(ctx, obj, "dropped", JS_NewInt64(ctx, result.dropped));
    JS_SetPropertyStr(ctx, obj, "discontinuities", JS_NewInt64(ctx, result.discontinuities));
    set_bool(ctx, obj, "payloadPreserved", result.payloadPreserved);
    set_bool(ctx, obj, "timestampsPreserved", result.timestampsPreserved);
    set_bool(ctx, obj, "extradataPreserved", result.remux.extradataPreserved);
    JS_SetPropertyStr(ctx, obj, "streams", JS_NewInt32(ctx, result.remux.streams));
    JS_SetPropertyStr(ctx, obj, "firstVideoPts", JS_NewInt64(ctx, result.remux.firstVideoPts));
    JS_SetPropertyStr(ctx, obj, "lastVideoPts", JS_NewInt64(ctx, result.remux.lastVideoPts));
    JS_SetPropertyStr(ctx, obj, "sourceExtradataSize", JS_NewInt32(ctx, result.remux.sourceExtradataSize));
    JS_SetPropertyStr(ctx, obj, "outputExtradataSize", JS_NewInt32(ctx, result.remux.outputExtradataSize));
    return obj;
}

JSValue make_muxer_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "transcode", JS_NewCFunction(ctx, muxer_transcode, "transcode", 1));
    JS_SetPropertyStr(ctx, ns, "remux", JS_NewCFunction(ctx, muxer_remux, "remux", 1));
    return ns;
}

JSValue make_packet_queue_namespace(JSContext* ctx) {
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "roundTripFirstPacket",
        JS_NewCFunction(ctx, packet_queue_round_trip_first_packet, "roundTripFirstPacket", 1));
    JS_SetPropertyStr(ctx, ns, "streamThroughQueue",
        JS_NewCFunction(ctx, packet_queue_stream_through, "streamThroughQueue", 1));
    return ns;
}
#endif // WL2_HAVE_QUICKJS