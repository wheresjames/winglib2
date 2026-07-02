#include "gstreamer/internal.h"

namespace wl2_gstreamer {

#if WL2_HAVE_QUICKJS

void register_pipeline_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (g_pipelineClassId == 0) {
        JS_NewClassID(&g_pipelineClassId);
    }
    JSClassDef def{};
    def.class_name = "Pipeline";
    def.finalizer = pipeline_finalizer;
    JS_NewClass(rt, g_pipelineClassId, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "state", JS_NewCFunction(ctx, pipeline_state, "state", 0));
    JS_SetPropertyStr(ctx, proto, "setState", JS_NewCFunction(ctx, pipeline_set_state, "setState", 1));
    JS_SetPropertyStr(ctx, proto, "play", JS_NewCFunction(ctx, pipeline_play, "play", 0));
    JS_SetPropertyStr(ctx, proto, "pause", JS_NewCFunction(ctx, pipeline_pause, "pause", 0));
    JS_SetPropertyStr(ctx, proto, "stop", JS_NewCFunction(ctx, pipeline_stop, "stop", 0));
    JS_SetPropertyStr(ctx, proto, "queryPosition", JS_NewCFunction(ctx, pipeline_query_position, "queryPosition", 0));
    JS_SetPropertyStr(ctx, proto, "queryDuration", JS_NewCFunction(ctx, pipeline_query_duration, "queryDuration", 0));
    JS_SetPropertyStr(ctx, proto, "seek", JS_NewCFunction(ctx, pipeline_seek, "seek", 1));
    JS_SetPropertyStr(ctx, proto, "busPoll", JS_NewCFunction(ctx, pipeline_bus_poll, "busPoll", 1));
    JS_SetPropertyStr(ctx, proto, "attachVideoSink", JS_NewCFunction(ctx, pipeline_attach_video_sink, "attachVideoSink", 1));
    JS_SetPropertyStr(ctx, proto, "attachAudioSink", JS_NewCFunction(ctx, pipeline_attach_audio_sink, "attachAudioSink", 1));
    JS_SetPropertyStr(ctx, proto, "attachVideoSource", JS_NewCFunction(ctx, pipeline_attach_video_source, "attachVideoSource", 1));
    JS_SetPropertyStr(ctx, proto, "attachAudioSource", JS_NewCFunction(ctx, pipeline_attach_audio_source, "attachAudioSource", 1));
    JS_SetPropertyStr(ctx, proto, "attachPacketSink", JS_NewCFunction(ctx, pipeline_attach_packet_sink, "attachPacketSink", 1));
    JS_SetPropertyStr(ctx, proto, "attachPacketSource", JS_NewCFunction(ctx, pipeline_attach_packet_source, "attachPacketSource", 1));
    JS_SetPropertyStr(ctx, proto, "pushVideoFrame", JS_NewCFunction(ctx, pipeline_push_video_frame, "pushVideoFrame", 1));
    JS_SetPropertyStr(ctx, proto, "pushAudioSamples", JS_NewCFunction(ctx, pipeline_push_audio_samples, "pushAudioSamples", 1));
    JS_SetPropertyStr(ctx, proto, "pushPacket", JS_NewCFunction(ctx, pipeline_push_packet, "pushPacket", 1));
    JS_SetPropertyStr(ctx, proto, "endOfStream", JS_NewCFunction(ctx, pipeline_end_of_stream, "endOfStream", 1));
    JS_SetPropertyStr(ctx, proto, "stats", JS_NewCFunction(ctx, pipeline_stats, "stats", 0));
    JS_SetPropertyStr(ctx, proto, "snapshot", JS_NewCFunction(ctx, pipeline_snapshot, "snapshot", 1));
    JS_SetPropertyStr(ctx, proto, "queryLatency", JS_NewCFunction(ctx, pipeline_query_latency, "queryLatency", 0));
    JS_SetPropertyStr(ctx, proto, "negotiatedCaps", JS_NewCFunction(ctx, pipeline_negotiated_caps, "negotiatedCaps", 1));
    JS_SetPropertyStr(ctx, proto, "setOverlayText", JS_NewCFunction(ctx, pipeline_set_overlay_text, "setOverlayText", 1));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, pipeline_close, "close", 0));
    JS_SetClassProto(ctx, g_pipelineClassId, proto);
}

int init_gstreamer_module(JSContext* ctx, JSModuleDef* module) {
    register_pipeline_class(ctx);
    JS_SetModuleExport(ctx, module, "version", JS_NewCFunction(ctx, gst_version_fn, "version", 0));
    JS_SetModuleExport(ctx, module, "capabilities", JS_NewCFunction(ctx, gst_capabilities_fn, "capabilities", 0));
    JS_SetModuleExport(ctx, module, "listPlugins", JS_NewCFunction(ctx, gst_list_plugins_fn, "listPlugins", 1));
    JS_SetModuleExport(ctx, module, "listElements", JS_NewCFunction(ctx, gst_list_elements_fn, "listElements", 1));
    JS_SetModuleExport(ctx, module, "parseLaunch", JS_NewCFunction(ctx, gst_parse_launch_fn, "parseLaunch", 2));
    JS_SetModuleExport(ctx, module, "testPattern", JS_NewCFunction(ctx, gst_test_pattern_fn, "testPattern", 1));
    JS_SetModuleExport(ctx, module, "filePlayback", JS_NewCFunction(ctx, gst_file_playback_fn, "filePlayback", 1));
    JS_SetModuleExport(ctx, module, "recordVideoBuffer", JS_NewCFunction(ctx, gst_record_video_buffer_fn, "recordVideoBuffer", 1));
    JS_SetModuleExport(ctx, module, "recordPacketBuffer", JS_NewCFunction(ctx, gst_record_packet_buffer_fn, "recordPacketBuffer", 1));
    JS_SetModuleExport(ctx, module, "discoverMedia", JS_NewCFunction(ctx, gst_discover_media_fn, "discoverMedia", 1));
    JS_SetModuleExport(ctx, module, "captureDevice", JS_NewCFunction(ctx, gst_capture_device_fn, "captureDevice", 1));
    JS_SetModuleExport(ctx, module, "sendUdpPackets", JS_NewCFunction(ctx, gst_send_udp_packets_fn, "sendUdpPackets", 1));
    JS_SetModuleExport(ctx, module, "receiveUdpPackets", JS_NewCFunction(ctx, gst_receive_udp_packets_fn, "receiveUdpPackets", 1));
    JS_SetModuleExport(ctx, module, "sendRtpPackets", JS_NewCFunction(ctx, gst_send_rtp_packets_fn, "sendRtpPackets", 1));
    JS_SetModuleExport(ctx, module, "receiveRtpPackets", JS_NewCFunction(ctx, gst_receive_rtp_packets_fn, "receiveRtpPackets", 1));
    JS_SetModuleExport(ctx, module, "sendTcpPackets", JS_NewCFunction(ctx, gst_send_tcp_packets_fn, "sendTcpPackets", 1));
    JS_SetModuleExport(ctx, module, "receiveTcpPackets", JS_NewCFunction(ctx, gst_receive_tcp_packets_fn, "receiveTcpPackets", 1));
    JS_SetModuleExport(ctx, module, "streamVideoUdp", JS_NewCFunction(ctx, gst_stream_video_udp_fn, "streamVideoUdp", 1));
    JS_SetModuleExport(ctx, module, "streamVideoTcp", JS_NewCFunction(ctx, gst_stream_video_tcp_fn, "streamVideoTcp", 1));
    JS_SetModuleExport(ctx, module, "rtspPlayback", JS_NewCFunction(ctx, gst_rtsp_playback_fn, "rtspPlayback", 1));
    JS_SetModuleExport(ctx, module, "teeVideoBuffer", JS_NewCFunction(ctx, gst_tee_video_buffer_fn, "teeVideoBuffer", 1));
    JS_SetModuleExport(ctx, module, "overlayVideoBuffer", JS_NewCFunction(ctx, gst_overlay_video_buffer_fn, "overlayVideoBuffer", 1));
    JSValue deviceMonitor = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, deviceMonitor, "create", JS_NewCFunction(ctx, gst_device_monitor_create_fn, "create", 1));
    JS_SetModuleExport(ctx, module, "DeviceMonitor", deviceMonitor);
    JSValue caps = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, caps, "parse", JS_NewCFunction(ctx, gst_caps_parse_fn, "parse", 1));
    JS_SetModuleExport(ctx, module, "Caps", caps);
    return 0;
}

constexpr const char* kExportNames[] = {
    "version", "capabilities", "listPlugins", "listElements", "parseLaunch", "testPattern",
    "filePlayback", "recordVideoBuffer", "recordPacketBuffer", "discoverMedia", "captureDevice", "DeviceMonitor",
    "sendUdpPackets", "receiveUdpPackets", "sendRtpPackets", "receiveRtpPackets",
    "sendTcpPackets", "receiveTcpPackets", "streamVideoUdp", "streamVideoTcp", "rtspPlayback",
    "teeVideoBuffer", "overlayVideoBuffer", "Caps",
};

#endif // WL2_HAVE_QUICKJS

} // namespace wl2_gstreamer

wl2::ModuleInfo wl2_gstreamer_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:gstreamer", wl2_gstreamer_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:gstreamer",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "bee7b81d-9fb6-48bf-81c9-6e519b85583c",
        .summary = "GStreamer pipeline runtime: build, run, and inspect pipelines from JavaScript.",
        .api = wl2_gstreamer::GstreamerApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_gstreamer_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, wl2_gstreamer::init_gstreamer_module);
    if (!module) {
        return nullptr;
    }
    for (const char* name : wl2_gstreamer::kExportNames) {
        JS_AddModuleExport(ctx, module, name);
    }
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_GSTREAMER_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:gstreamer";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "bee7b81d-9fb6-48bf-81c9-6e519b85583c";
    out->summary = "GStreamer pipeline runtime: build, run, and inspect pipelines from JavaScript.";
    out->api = wl2_gstreamer::GstreamerApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
