// wl2_ffmpeg types and utility functions
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp

// ============================================================================
// Structures
// ============================================================================

struct DemuxerHandle {
    AVFormatContext* format = nullptr;
    std::string path;
    int videoStream = -1;

    ~DemuxerHandle() {
        if (format) {
            avformat_close_input(&format);
        }
    }
};

struct ReplaySessionHandle {
    std::string path;
    int64_t lastSeekId = 0;
    int64_t repeatCount = 0;
    int64_t dropCount = 0;
    int64_t publishedFrames = 0;
    int64_t lastPublishedPts = AV_NOPTS_VALUE;
    std::string lastResultMode = "none";
    std::string lastDirection = "forward";
    double lastSpeed = 1.0;
    std::map<std::string, wl2::VideoBuffer> videoBuffers;
    std::map<std::string, wl2::AudioBuffer> audioBuffers;
};

struct DecodedVideoFrame {
    std::vector<uint8_t> bytes;
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t ptsUs = AV_NOPTS_VALUE;
    int64_t duration = 0;
    int streamIndex = -1;
    bool keyframe = false;
    std::string resultMode = "exact";
};

struct DecodedAudioSamples {
    std::vector<uint8_t> bytes;
    int sampleRate = 0;
    int channels = 0;
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t ptsUs = AV_NOPTS_VALUE;
    int64_t samples = 0;
    int streamIndex = -1;
    AVSampleFormat sourceFormat = AV_SAMPLE_FMT_NONE;
    int sourceSampleRate = 0;
    int sourceChannels = 0;
    bool sourcePlanar = false;
};

struct FixtureOptions {
    int width = 320;
    int height = 240;
    int fps = 15;
    int durationSeconds = 10;
    int gop = 15;
    bool tone = false;
    std::string videoCodec = "rawvideo"; // "rawvideo" or "mjpeg"
    std::string format;                  // container; inferred from path if empty
    bool gaps = false;
    bool duplicateTimestamps = false;
    bool backwardPts = false;
    bool missingDurations = false;
};

struct TranscodeResult {
    std::string container;
    std::string videoCodec;
    std::string audioCodec;
    std::string videoPixelFormat;
    std::string audioSampleFormat;
    std::string preset;
    int width = 0;
    int height = 0;
    int64_t videoPackets = 0;
    int64_t audioPackets = 0;
    bool reopened = false;
    int reopenedStreams = 0;
    int reopenedWidth = 0;
    int reopenedHeight = 0;
    bool reopenedHasAudio = false;
    int reopenedSampleRate = 0;
};

struct RemuxResult {
    int streams = 0;
    int64_t packets = 0;
    int64_t firstVideoPts = AV_NOPTS_VALUE;
    int64_t lastVideoPts = AV_NOPTS_VALUE;
    bool extradataPreserved = false;
    int sourceExtradataSize = 0;
    int outputExtradataSize = 0;
    std::string container;
};

struct VideoBufferMuxResult {
    std::string container;
    std::string videoCodec;
    std::string videoPixelFormat;
    int width = 0;
    int height = 0;
    int fps = 0;
    int64_t frames = 0;
    int64_t videoPackets = 0;
    bool reopened = false;
    int reopenedStreams = 0;
    int reopenedWidth = 0;
    int reopenedHeight = 0;
};

struct QueueRemuxResult {
    int64_t packetsRead = 0;
    int64_t recordsWritten = 0;
    int64_t recordsRead = 0;
    int64_t dropped = 0;
    int64_t discontinuities = 0;
    bool payloadPreserved = true;
    bool timestampsPreserved = true;
    RemuxResult remux;
    std::string policy;
};

struct StillCodecChoice {
    AVCodecID id = AV_CODEC_ID_PNG;
    AVPixelFormat pixelFormat = AV_PIX_FMT_RGB24;
    const char* extension = "png";
};

// ============================================================================
// Enums
// ============================================================================

enum class VideoSeekMode { Fast, Accurate, Nearest, Frame };

// ============================================================================
// Constants
// ============================================================================

constexpr const char* FfmpegApi = R"(Exports JavaScript module wl2:ffmpeg.

Functions:
  version()                  -> FFmpeg component versions and build policy.
  listCodecs(options)        -> codec summary, filterable by type/name.
  listFormats(options)       -> muxer/demuxer summary.
  capabilities()             -> current module feature flags.
  generateSyntheticFixture(options)
                             -> deterministic fixture metadata, or an AVI file
                                when options.path is provided.
  Demuxer.open(path)         -> stream metadata and compressed packet reads.
  ReplaySession.open(path)   -> seek/extract/publish frames and A/V playback plan.
  AudioDecoder.decode(opts)  -> decode audio with planar/interleaved metadata.
  AudioConverter.convert(opts)
                             -> libswresample sample/rate/layout conversion.
  Muxer.transcode(opts)      -> decode/encode/mux round-trip with named presets.
  Muxer.remux(opts)          -> compressed remux preserving timestamps/extradata.
  Muxer.writeVideoBuffer(opts)
                             -> encode/mux frames from an existing VideoBuffer.
  PacketQueue.streamThroughQueue(opts)
                             -> producer/consumer remux with backpressure policy.
  VideoEncoder.presets()/AudioEncoder.presets()
                             -> named low-latency and archival presets.
  analyzeTimestamps(opts)    -> non-monotonic/gap/duplicate/missing-duration scan.
  Evidence.exportStill/extractClip/keyframeWindow
                             -> still export, packet-copy/transcode clips, windows.
  FilterGraph.apply/info     -> optional libavfilter chain (when compiled in).
  hardware()                 -> hardware-acceleration discovery, software fallback.
  profilePacketQueue(opts)   -> packet-queue copy-cost profiling.

The module provides synchronous demux/decode primitives, generated and
irregular fixtures, MJPEG input, fast/accurate/nearest/frame seek modes,
reverse window playback and a paced schedule, raw VideoBuffer/AudioBuffer
publication, compressed PacketQueue bridges with backpressure, encode/mux,
remux, evidence workflows, timestamp diagnostics, optional filter graphs, and
hardware-acceleration discovery. A runtime async trick-play scheduler is the
remaining follow-on.)";

struct NamedValue {
    const char* name;
    int value;
};

constexpr NamedValue RequiredPixelFormats[] = {
    {"gray8", AV_PIX_FMT_GRAY8},
    {"yuyv422", AV_PIX_FMT_YUYV422},
    {"uyvy422", AV_PIX_FMT_UYVY422},
    {"yuv420p", AV_PIX_FMT_YUV420P},
    {"nv12", AV_PIX_FMT_NV12},
    {"rgb24", AV_PIX_FMT_RGB24},
    {"bgr24", AV_PIX_FMT_BGR24},
    {"rgba", AV_PIX_FMT_RGBA},
    {"bgra", AV_PIX_FMT_BGRA},
};

// ============================================================================
// Utility Functions
// ============================================================================

const char* media_type_name(AVMediaType type) {
    const char* name = av_get_media_type_string(type);
    return name ? name : "unknown";
}

std::string version_string(unsigned version) {
    return std::to_string((version >> 16) & 0xff) + "."
        + std::to_string((version >> 8) & 0xff) + "."
        + std::to_string(version & 0xff);
}

int64_t timestamp_to_us(int64_t timestamp, AVRational timeBase) {
    if (timestamp == AV_NOPTS_VALUE) {
        return AV_NOPTS_VALUE;
    }
    return av_rescale_q(timestamp, timeBase, AVRational{1, 1000000});
}

AVPixelFormat pixel_format_from_name(std::string_view name) {
    if (name == "rgb24") return AV_PIX_FMT_RGB24;
    if (name == "bgr24") return AV_PIX_FMT_BGR24;
    if (name == "rgba") return AV_PIX_FMT_RGBA;
    if (name == "bgra") return AV_PIX_FMT_BGRA;
    if (name == "gray8") return AV_PIX_FMT_GRAY8;
    if (name == "yuv420p") return AV_PIX_FMT_YUV420P;
    if (name == "nv12") return AV_PIX_FMT_NV12;
    if (name == "yuyv422") return AV_PIX_FMT_YUYV422;
    if (name == "uyvy422") return AV_PIX_FMT_UYVY422;
    return AV_PIX_FMT_BGRA;
}

std::string pixel_format_name(AVPixelFormat fmt) {
    const char* name = av_get_pix_fmt_name(fmt);
    return name ? name : "unknown";
}

AVSampleFormat sample_format_from_name(std::string_view name) {
    if (name == "u8") return AV_SAMPLE_FMT_U8;
    if (name == "s16" || name == "s16le") return AV_SAMPLE_FMT_S16;
    if (name == "s32" || name == "s32le") return AV_SAMPLE_FMT_S32;
    if (name == "flt" || name == "f32") return AV_SAMPLE_FMT_FLT;
    if (name == "dbl" || name == "f64") return AV_SAMPLE_FMT_DBL;
    return AV_SAMPLE_FMT_S16;
}

std::string sample_format_name(AVSampleFormat fmt) {
    const char* name = av_get_sample_fmt_name(fmt);
    return name ? name : "unknown";
}

int bytes_per_sample(AVSampleFormat fmt) {
    return av_get_bytes_per_sample(fmt);
}

AVPixelFormat pixel_format_for_membus(std::string_view name) {
    if (name == "rgb24" || name.empty()) return AV_PIX_FMT_RGB24;
    if (name == "bgr24") return AV_PIX_FMT_BGR24;
    if (name == "rgba32" || name == "rgba") return AV_PIX_FMT_RGBA;
    if (name == "bgra32" || name == "bgra") return AV_PIX_FMT_BGRA;
    if (name == "gray8") return AV_PIX_FMT_GRAY8;
    if (name == "yuyv422") return AV_PIX_FMT_YUYV422;
    if (name == "uyvy422") return AV_PIX_FMT_UYVY422;
    return AV_PIX_FMT_RGB24;
}

wl2::VideoPixelFormat video_buffer_format_from_pixel_format(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_BGR24: return wl2::VideoPixelFormat::Bgr24;
        case AV_PIX_FMT_RGBA: return wl2::VideoPixelFormat::Rgba32;
        case AV_PIX_FMT_BGRA: return wl2::VideoPixelFormat::Bgra32;
        case AV_PIX_FMT_GRAY8: return wl2::VideoPixelFormat::Gray8;
        case AV_PIX_FMT_YUYV422: return wl2::VideoPixelFormat::Yuyv422;
        case AV_PIX_FMT_UYVY422: return wl2::VideoPixelFormat::Uyvy422;
        case AV_PIX_FMT_RGB24:
        default:
            return wl2::VideoPixelFormat::Rgb24;
    }
}

VideoSeekMode video_seek_mode_from_name(std::string_view name) {
    if (name == "fast") return VideoSeekMode::Fast;
    if (name == "nearest") return VideoSeekMode::Nearest;
    if (name == "frame") return VideoSeekMode::Frame;
    return VideoSeekMode::Accurate;
}

bool still_codec_from_name(std::string_view name, StillCodecChoice& out) {
    if (name == "png") {
        out = {AV_CODEC_ID_PNG, AV_PIX_FMT_RGB24, "png"};
        return true;
    }
    if (name == "jpeg" || name == "jpg" || name == "mjpeg") {
        out = {AV_CODEC_ID_MJPEG, AV_PIX_FMT_YUVJ420P, "jpg"};
        return true;
    }
    if (name == "bmp") {
        out = {AV_CODEC_ID_BMP, AV_PIX_FMT_BGR24, "bmp"};
        return true;
    }
    return false;
}

// ============================================================================
// Packet Record Serialization
// ============================================================================

void append_u32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

void append_i64(std::string& out, int64_t value) {
    const auto u = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((u >> (i * 8)) & 0xff));
    }
}

uint32_t read_u32(const char* data) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << (i * 8);
    }
    return value;
}

int64_t read_i64(const char* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<unsigned char>(data[i])) << (i * 8);
    }
    return static_cast<int64_t>(value);
}

std::string make_packet_record(const AVPacket* packet) {
    std::string out;
    out.reserve(56 + static_cast<size_t>(packet->size));
    out.append("WL2FQP1", 7);
    out.push_back('\0');
    append_u32(out, 1);
    append_u32(out, static_cast<uint32_t>(packet->stream_index));
    append_i64(out, packet->pts);
    append_i64(out, packet->dts);
    append_i64(out, packet->duration);
    append_u32(out, static_cast<uint32_t>(packet->flags));
    append_u32(out, static_cast<uint32_t>(packet->size));
    out.append(reinterpret_cast<const char*>(packet->data), static_cast<size_t>(packet->size));
    return out;
}

std::string make_sequenced_packet_record(const AVPacket* packet, uint64_t sequence, bool discontinuity) {
    std::string out;
    out.reserve(64 + static_cast<size_t>(packet->size));
    out.append("WL2FQP2", 7);
    out.push_back('\0');
    append_u32(out, 2);
    append_u32(out, static_cast<uint32_t>(packet->stream_index));
    append_i64(out, packet->pts);
    append_i64(out, packet->dts);
    append_i64(out, packet->duration);
    append_u32(out, static_cast<uint32_t>(packet->flags));
    append_u32(out, static_cast<uint32_t>(packet->size));
    append_i64(out, static_cast<int64_t>(sequence));
    append_u32(out, discontinuity ? 1u : 0u);
    out.append(reinterpret_cast<const char*>(packet->data), static_cast<size_t>(packet->size));
    return out;
}

std::string make_stream_config_record(AVFormatContext* format) {
    std::string out;
    out.append("WL2FSC1", 7);
    out.push_back('\0');
    append_u32(out, 1);
    append_u32(out, format->nb_streams);
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVCodecParameters* par = format->streams[i]->codecpar;
        append_u32(out, static_cast<uint32_t>(par->codec_type));
        append_u32(out, static_cast<uint32_t>(par->codec_id));
        append_u32(out, static_cast<uint32_t>(par->format));
        append_u32(out, static_cast<uint32_t>(par->width));
        append_u32(out, static_cast<uint32_t>(par->height));
        append_u32(out, static_cast<uint32_t>(par->sample_rate));
        append_u32(out, static_cast<uint32_t>(par->ch_layout.nb_channels));
        append_u32(out, static_cast<uint32_t>(format->streams[i]->time_base.num));
        append_u32(out, static_cast<uint32_t>(format->streams[i]->time_base.den));
        append_u32(out, static_cast<uint32_t>(par->extradata_size));
        if (par->extradata_size > 0 && par->extradata) {
            out.append(reinterpret_cast<const char*>(par->extradata), static_cast<size_t>(par->extradata_size));
        }
    }
    return out;
}

// ============================================================================
// Synthetic Frame Fillers
// ============================================================================

void fill_bgr_frame(std::vector<uint8_t>& frame, int width, int height, int index, int fps) {
    frame.resize(static_cast<size_t>(width) * height * 3);
    const int seconds = index / std::max(1, fps);
    const int frameInSecond = index % std::max(1, fps);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t off = (static_cast<size_t>(y) * width + x) * 3;
            frame[off + 0] = static_cast<uint8_t>((x + index * 7) & 0xff);
            frame[off + 1] = static_cast<uint8_t>((y + seconds * 31) & 0xff);
            frame[off + 2] = static_cast<uint8_t>(((x / 8 + y / 8 + frameInSecond) % 2) ? 0xe0 : 0x30);
        }
    }
}

void fill_s16_tone(std::vector<uint8_t>& audio, int sampleRate, int channels, int startSample, int sampleCount) {
    audio.resize(static_cast<size_t>(sampleCount) * channels * sizeof(int16_t));
    auto* out = reinterpret_cast<int16_t*>(audio.data());
    constexpr double frequency = 440.0;
    constexpr double amplitude = 12000.0;
    for (int i = 0; i < sampleCount; ++i) {
        const double t = static_cast<double>(startSample + i) / static_cast<double>(sampleRate);
        const auto value = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979323846 * frequency * t) * amplitude);
        for (int ch = 0; ch < channels; ++ch) {
            out[static_cast<size_t>(i) * channels + ch] = value;
        }
    }
}

// ============================================================================
// Encoder Helpers
// ============================================================================

AVPixelFormat encoder_first_pixel_format(const AVCodec* codec, AVPixelFormat fallback) {
    const enum AVPixelFormat* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
            reinterpret_cast<const void**>(&formats), &count) >= 0 && formats && count > 0) {
        return formats[0];
    }
    return fallback;
}

AVSampleFormat encoder_first_sample_format(const AVCodec* codec, AVSampleFormat fallback) {
    const enum AVSampleFormat* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
            reinterpret_cast<const void**>(&formats), &count) >= 0 && formats && count > 0) {
        return formats[0];
    }
    return fallback;
}

int encoder_pick_sample_rate(const AVCodec* codec, int preferred) {
    const int* rates = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0,
            reinterpret_cast<const void**>(&rates), &count) >= 0 && rates && count > 0) {
        for (int i = 0; i < count; ++i) {
            if (rates[i] == preferred) {
                return preferred;
            }
        }
        return rates[0];
    }
    return preferred;
}

const AVCodec* find_encoder_preference(const std::vector<std::string>& names) {
    for (const auto& name : names) {
        if (name.empty()) {
            continue;
        }
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (codec && av_codec_is_encoder(codec)) {
            return codec;
        }
    }
    return nullptr;
}

std::vector<std::string> video_encoder_candidates(const std::string& requested) {
    if (!requested.empty()) {
        return {requested};
    }
    return {"mpeg4", "mjpeg", "ffv1", "mpeg2video"};
}

std::vector<std::string> audio_encoder_candidates(const std::string& requested) {
    if (!requested.empty()) {
        return {requested};
    }
    return {"mp2", "ac3", "aac", "pcm_s16le"};
}

void apply_video_preset(AVCodecContext* enc, const std::string& preset) {
    if (preset == "archival") {
        enc->gop_size = 12;
        enc->max_b_frames = 0;
        enc->bit_rate = 2'000'000;
    } else {
        // Default to a low-latency preset suitable for security/live replay:
        // intra-only GOP and no reordering so every packet decodes immediately.
        enc->gop_size = 1;
        enc->max_b_frames = 0;
        enc->bit_rate = 800'000;
    }
}

bool codec_matches(const AVCodec* codec, const std::string& typeFilter, const std::string& nameFilter) {
    if (!typeFilter.empty() && typeFilter != media_type_name(codec->type)) {
        return false;
    }
    if (!nameFilter.empty()) {
        std::string name = codec->name ? codec->name : "";
        std::string longName = codec->long_name ? codec->long_name : "";
        if (name.find(nameFilter) == std::string::npos && longName.find(nameFilter) == std::string::npos) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// QuickJS Helpers (conditional compilation)
// ============================================================================

#if WL2_HAVE_QUICKJS
JSClassID demuxer_class_id = 0;
JSClassID replay_session_class_id = 0;

void demuxer_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<DemuxerHandle*>(JS_GetOpaque(val, demuxer_class_id));
}

void replay_session_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<ReplaySessionHandle*>(JS_GetOpaque(val, replay_session_class_id));
}

std::string js_string(JSContext* ctx, JSValueConst value) {
    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, value);
    if (!text) {
        return {};
    }
    std::string out(text, len);
    JS_FreeCString(ctx, text);
    return out;
}

JSValue throw_ffmpeg_error(JSContext* ctx, const char* operation, int code, const std::string& message = {}) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, errbuf, sizeof(errbuf));
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "FfmpegError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_ffmpeg"));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, "ffmpeg_error"));
    JS_SetPropertyStr(ctx, error, "ffmpegCode", JS_NewInt32(ctx, code));
    JS_SetPropertyStr(ctx, error, "ffmpegMessage", JS_NewString(ctx, errbuf));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.empty() ? errbuf : message.c_str()));
    return JS_Throw(ctx, error);
}

JSValue throw_module_error(JSContext* ctx, const char* operation, const char* code, const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "FfmpegError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_ffmpeg"));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}

bool get_string_prop(JSContext* ctx, JSValueConst obj, const char* name, std::string& out) {
    if (!JS_IsObject(obj)) {
        return false;
    }
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    out = js_string(ctx, value);
    JS_FreeValue(ctx, value);
    return true;
}

bool get_int_prop(JSContext* ctx, JSValueConst obj, const char* name, int32_t& out) {
    if (!JS_IsObject(obj)) {
        return false;
    }
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    const bool ok = JS_ToInt32(ctx, &out, value) == 0;
    JS_FreeValue(ctx, value);
    return ok;
}

bool get_bool_prop(JSContext* ctx, JSValueConst obj, const char* name, bool& out) {
    if (!JS_IsObject(obj)) {
        return false;
    }
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    out = JS_ToBool(ctx, value) != 0;
    JS_FreeValue(ctx, value);
    return true;
}

void set_string(JSContext* ctx, JSValue obj, const char* name, std::string_view value) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewStringLen(ctx, value.data(), value.size()));
}

void set_bool(JSContext* ctx, JSValue obj, const char* name, bool value) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewBool(ctx, value));
}

JSValue make_string_array(JSContext* ctx, const std::vector<std::string>& values) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& value : values) {
        JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, value.c_str()));
    }
    return array;
}

JSValue make_wl2_buffer(JSContext* ctx, const uint8_t* bytes, size_t size) {
    JSValue arrayBuffer = JS_NewArrayBufferCopy(ctx, bytes, size);
    if (JS_IsException(arrayBuffer)) {
        return arrayBuffer;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    JS_FreeValue(ctx, global);
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2 host API is unavailable");
    }

    JSValue bufferNamespace = JS_GetPropertyStr(ctx, wl2, "buffer");
    JS_FreeValue(ctx, wl2);
    if (!JS_IsObject(bufferNamespace)) {
        JS_FreeValue(ctx, bufferNamespace);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2.buffer host API is unavailable");
    }

    JSValue fromArrayBuffer = JS_GetPropertyStr(ctx, bufferNamespace, "fromArrayBuffer");
    if (!JS_IsFunction(ctx, fromArrayBuffer)) {
        JS_FreeValue(ctx, fromArrayBuffer);
        JS_FreeValue(ctx, bufferNamespace);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2.buffer.fromArrayBuffer is unavailable");
    }

    JSValue args[] = {arrayBuffer};
    JSValue result = JS_Call(ctx, fromArrayBuffer, bufferNamespace, 1, args);
    JS_FreeValue(ctx, fromArrayBuffer);
    JS_FreeValue(ctx, bufferNamespace);
    JS_FreeValue(ctx, arrayBuffer);
    return result;
}

bool read_js_bytes(JSContext* ctx, JSValueConst value, std::vector<uint8_t>& out) {
    auto clear_pending = [ctx]() {
        JSValue pending = JS_GetException(ctx);
        JS_FreeValue(ctx, pending);
    };
    if (JS_IsObject(value)) {
        JSValue accessor = JS_GetPropertyStr(ctx, value, "arrayBuffer");
        if (JS_IsFunction(ctx, accessor)) {
            JSValue arrayBuffer = JS_Call(ctx, accessor, value, 0, nullptr);
            JS_FreeValue(ctx, accessor);
            if (JS_IsException(arrayBuffer)) {
                JS_FreeValue(ctx, arrayBuffer);
                clear_pending();
            } else {
                size_t len = 0;
                uint8_t* bytes = JS_GetArrayBuffer(ctx, &len, arrayBuffer);
                if (bytes) {
                    out.assign(bytes, bytes + len);
                    JS_FreeValue(ctx, arrayBuffer);
                    return true;
                }
                JS_FreeValue(ctx, arrayBuffer);
                clear_pending();
            }
        } else {
            JS_FreeValue(ctx, accessor);
        }
    }
    size_t len = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &len, value);
    if (bytes) {
        out.assign(bytes, bytes + len);
        return true;
    }
    clear_pending();
    size_t offset = 0;
    size_t typedLength = 0;
    size_t bytesPerElement = 0;
    JSValue arrayBuffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &typedLength, &bytesPerElement);
    if (!JS_IsException(arrayBuffer)) {
        size_t bufferLength = 0;
        uint8_t* typedBytes = JS_GetArrayBuffer(ctx, &bufferLength, arrayBuffer);
        if (typedBytes && offset <= bufferLength) {
            typedLength = std::min(typedLength, bufferLength - offset);
            out.assign(typedBytes + offset, typedBytes + offset + typedLength);
            JS_FreeValue(ctx, arrayBuffer);
            return true;
        }
    }
    JS_FreeValue(ctx, arrayBuffer);
    clear_pending();
    return false;
}

std::string path_from_first_arg(JSContext* ctx, int argc, JSValueConst* argv, const char* operation) {
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "%s requires a path", operation);
        return {};
    }
    std::string path;
    if (JS_IsObject(argv[0])) {
        get_string_prop(ctx, argv[0], "path", path);
    } else {
        path = js_string(ctx, argv[0]);
    }
    if (path.empty()) {
        JS_ThrowTypeError(ctx, "%s requires a non-empty path", operation);
    }
    return path;
}

JSValue make_stream_info(JSContext* ctx, const AVStream* stream) {
    JSValue obj = JS_NewObject(ctx);
    const AVCodecParameters* par = stream->codecpar;
    JS_SetPropertyStr(ctx, obj, "index", JS_NewInt32(ctx, stream->index));
    set_string(ctx, obj, "type", media_type_name(par->codec_type));
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    set_string(ctx, obj, "codec", desc && desc->name ? desc->name : avcodec_get_name(par->codec_id));
    set_string(ctx, obj, "codecLongName", desc && desc->long_name ? desc->long_name : "");
    JS_SetPropertyStr(ctx, obj, "codecId", JS_NewInt32(ctx, par->codec_id));
    JS_SetPropertyStr(ctx, obj, "timeBaseNum", JS_NewInt32(ctx, stream->time_base.num));
    JS_SetPropertyStr(ctx, obj, "timeBaseDen", JS_NewInt32(ctx, stream->time_base.den));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, stream->duration));
    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, par->width));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, par->height));
        set_string(ctx, obj, "pixelFormat", pixel_format_name(static_cast<AVPixelFormat>(par->format)));
        AVRational rate = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
        JS_SetPropertyStr(ctx, obj, "fpsNum", JS_NewInt32(ctx, rate.num));
        JS_SetPropertyStr(ctx, obj, "fpsDen", JS_NewInt32(ctx, rate.den ? rate.den : 1));
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, par->sample_rate));
        JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, par->ch_layout.nb_channels));
    }
    if (par->extradata && par->extradata_size > 0) {
        JS_SetPropertyStr(ctx, obj, "extradata", make_wl2_buffer(ctx, par->extradata, par->extradata_size));
    }
    return obj;
}

DemuxerHandle* get_demuxer(JSContext* ctx, JSValueConst thisVal) {
    auto* handle = static_cast<DemuxerHandle*>(JS_GetOpaque2(ctx, thisVal, demuxer_class_id));
    return handle;
}

ReplaySessionHandle* get_replay_session(JSContext* ctx, JSValueConst thisVal) {
    auto* handle = static_cast<ReplaySessionHandle*>(JS_GetOpaque2(ctx, thisVal, replay_session_class_id));
    return handle;
}

JSValue new_demuxer(JSContext* ctx, std::unique_ptr<DemuxerHandle> handle) {
    JSValue obj = JS_NewObjectClass(ctx, demuxer_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, handle.release());
    return obj;
}

JSValue new_replay_session(JSContext* ctx, std::unique_ptr<ReplaySessionHandle> handle) {
    JSValue obj = JS_NewObjectClass(ctx, replay_session_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, handle.release());
    return obj;
}
#endif // WL2_HAVE_QUICKJS
