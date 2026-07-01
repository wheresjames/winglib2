// wl2_ffmpeg module exports - public registration functions
// Part of wl2_ffmpeg module - included from wl2_ffmpeg.cpp OUTSIDE anonymous namespace
// These functions must have external linkage to be visible to the runtime loader.

// ============================================================================
// Module Registration
// ============================================================================

wl2::ModuleInfo wl2_ffmpeg_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:ffmpeg", wl2_ffmpeg_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:ffmpeg",
        .version = av_version_info(),
        .build = WL2_BUILD,
        .stableId = "5f03b3d4-2e2f-4c20-9ac5-fb2dd7ad0f6f",
        .summary = "FFmpeg-backed media discovery and archive replay module.",
        .api = FfmpegApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_ffmpeg_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_ffmpeg_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "version");
    JS_AddModuleExport(ctx, module, "listCodecs");
    JS_AddModuleExport(ctx, module, "listFormats");
    JS_AddModuleExport(ctx, module, "capabilities");
    JS_AddModuleExport(ctx, module, "analyzeTimestamps");
    JS_AddModuleExport(ctx, module, "generateSyntheticFixture");
    JS_AddModuleExport(ctx, module, "Demuxer");
    JS_AddModuleExport(ctx, module, "ReplaySession");
    JS_AddModuleExport(ctx, module, "PacketQueue");
    JS_AddModuleExport(ctx, module, "Muxer");
    JS_AddModuleExport(ctx, module, "AudioConverter");
    JS_AddModuleExport(ctx, module, "AudioDecoder");
    JS_AddModuleExport(ctx, module, "VideoEncoder");
    JS_AddModuleExport(ctx, module, "AudioEncoder");
    JS_AddModuleExport(ctx, module, "Evidence");
    JS_AddModuleExport(ctx, module, "FilterGraph");
    JS_AddModuleExport(ctx, module, "hardware");
    JS_AddModuleExport(ctx, module, "filters");
    JS_AddModuleExport(ctx, module, "profilePacketQueue");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_FFMPEG_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:ffmpeg";
    out->version = av_version_info();
    out->build = WL2_BUILD;
    out->stable_id = "5f03b3d4-2e2f-4c20-9ac5-fb2dd7ad0f6f";
    out->summary = "FFmpeg-backed media discovery and archive replay module.";
    out->api = FfmpegApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    if (!host || !host->register_quickjs_module) {
        return 1;
    }
    host->register_quickjs_module(host->host, "wl2:ffmpeg", wl2_ffmpeg_quickjs_module_factory);
    return 0;
}
#endif