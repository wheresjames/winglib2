#pragma once

#include "wl2/membus.h"
#include "wl2/runtime.h"
#include "wl2_gstreamer/wl2_gstreamer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gst/gst.h>

#if WL2_GSTREAMER_HAVE_APP
#include <gst/app/app.h>
#endif

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif

namespace wl2_gstreamer {

#ifdef WL2_GSTREAMER_HAVE_APP
inline constexpr bool kHaveApp = true;
#else
inline constexpr bool kHaveApp = false;
#endif
#ifdef WL2_GSTREAMER_HAVE_VIDEO
inline constexpr bool kHaveVideo = true;
#else
inline constexpr bool kHaveVideo = false;
#endif
#ifdef WL2_GSTREAMER_HAVE_AUDIO
inline constexpr bool kHaveAudio = true;
#else
inline constexpr bool kHaveAudio = false;
#endif
#ifdef WL2_GSTREAMER_HAVE_PBUTILS
inline constexpr bool kHavePbutils = true;
#else
inline constexpr bool kHavePbutils = false;
#endif

extern const char* const GstreamerApi;

bool ensure_gst_init();

#if WL2_HAVE_QUICKJS

JSValue throw_gst_error(JSContext* ctx, const char* code, const char* operation,
    const std::string& message, const std::string& debug = {}, const std::string& element = {});
JSValue throw_gst_init_error(JSContext* ctx, const char* operation);
wl2::Runtime* current_runtime(JSContext* ctx);

bool option_int(JSContext* ctx, JSValueConst options, const char* key, int64_t& out);
bool option_bool(JSContext* ctx, JSValueConst options, const char* key, bool fallback);
bool option_string(JSContext* ctx, JSValueConst options, const char* key, std::string& out);

JSValue gst_version_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_capabilities_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_list_plugins_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_list_elements_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_element_info_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_has_property_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_uri_handlers_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_parse_launch_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_test_pattern_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_file_playback_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_record_video_buffer_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_record_packet_buffer_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_discover_media_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_capture_device_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_device_monitor_create_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_send_udp_packets_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_receive_udp_packets_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_send_rtp_packets_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_receive_rtp_packets_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_send_tcp_packets_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_receive_tcp_packets_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_stream_video_udp_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_stream_video_tcp_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_rtsp_playback_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_required_elements_for_uri_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_can_decode_uri_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_build_hls_output_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_build_dash_output_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_build_srt_output_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

std::optional<wl2::VideoPixelFormat> gst_format_to_pixel(const std::string& format);
const char* pixel_to_gst_format(wl2::VideoPixelFormat format);
std::optional<wl2::AudioSampleFormat> gst_format_to_sample(const std::string& format);
const char* sample_to_gst_format(wl2::AudioSampleFormat format);

extern JSClassID g_pipelineClassId;

struct Bridge {
    std::string name;
    std::string kind;
    std::string bufferName;
    virtual ~Bridge() = default;
    virtual JSValue stats(JSContext* ctx) = 0;
    // Video sink bridges retain the most recent sample so snapshot() can encode a
    // still without racing the streaming thread. Returns a new ref (caller
    // unrefs) or null for bridges that do not hold samples.
    virtual GstSample* acquireLastSample() { return nullptr; }
};

// Shared state behind Pipeline.watchBus(). The GstBus sync handler runs on
// whatever thread posts a bus message, so it may only read the atomics and
// forward ref'd messages to the JS thread; the JSValue callback slots are
// touched exclusively on the JS thread. The slot survives watch/unwatch cycles
// because a GstBus sync handler cannot be replaced while installed — it is
// installed once and only removed in close_pipeline() after the pipeline has
// fully reached NULL (no streaming thread can still be inside gst_bus_post).
struct BusWatchSlot {
    wl2::Runtime* runtime = nullptr;
    JSContext* ctx = nullptr;
    JSValue onMessage = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    JSValue onWarning = JS_UNDEFINED;
    JSValue onEos = JS_UNDEFINED;
    std::atomic<bool> active{false};
    // Bumped on every watch/unwatch so completions posted for an older watch
    // are dropped instead of invoking the new watch's callbacks.
    std::atomic<uint64_t> generation{0};
};

struct PipelineBox {
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::vector<std::unique_ptr<Bridge>> bridges;
    std::shared_ptr<BusWatchSlot> busWatch;
    bool closed = false;
};

void close_pipeline(PipelineBox* box);
void pipeline_finalizer(JSRuntime* rt, JSValue value);
PipelineBox* live_pipeline(JSContext* ctx, JSValueConst thisVal, const char* operation);
JSValue new_pipeline_object(JSContext* ctx, GstElement* pipeline);

JSValue pipeline_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_set_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_play(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_pause(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_stop(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_query_position(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_query_duration(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_seek(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_bus_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_watch_bus(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_unwatch_bus(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

// Serialize one bus message into a JS object (does not unref the message).
JSValue message_to_js(JSContext* ctx, GstMessage* message);

JSValue pipeline_attach_video_sink(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_attach_audio_sink(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_attach_video_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_attach_audio_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_attach_packet_sink(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_attach_packet_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_push_video_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_push_audio_samples(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_push_packet(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_end_of_stream(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_stats(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

// --- Advanced pipeline features ---------------------------------------------
JSValue pipeline_snapshot(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_query_latency(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_negotiated_caps(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue pipeline_set_overlay_text(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_caps_parse_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_tee_video_buffer_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue gst_overlay_video_buffer_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

// Serialize a GstCaps into a JS object: { text, structureCount, structures }.
JSValue caps_to_js(JSContext* ctx, GstCaps* caps);

#if WL2_GSTREAMER_HAVE_APP
JSValue attach_video_sink_core(JSContext* ctx, PipelineBox* box, const char* operation,
    const std::string& elementName, const std::string& bufferName, bool create,
    int64_t width, int64_t height, int64_t fps, int64_t buffers, wl2::VideoPixelFormat pixel);
#endif

#endif // WL2_HAVE_QUICKJS

} // namespace wl2_gstreamer
