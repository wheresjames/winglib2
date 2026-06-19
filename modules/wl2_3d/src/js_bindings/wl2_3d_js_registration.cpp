void register_simple_class(JSContext* ctx, JSClassID& id, const char* name, JSClassFinalizer finalizer) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (id == 0) {
        JS_NewClassID(&id);
    }
    JSClassDef def{};
    def.class_name = name;
    def.finalizer = finalizer;
    JS_NewClass(rt, id, &def);
}

void register_camera_class(JSContext* ctx) {
    register_simple_class(ctx, camera_class_id, "ThreeDCamera", camera_finalizer);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "calibrate", JS_NewCFunction(ctx, camera_calibrate, "calibrate", 1));
    JS_SetPropertyStr(ctx, proto, "lookFrom", JS_NewCFunction(ctx, camera_look_from, "lookFrom", 2));
    JS_SetPropertyStr(ctx, proto, "unproject", JS_NewCFunction(ctx, camera_unproject, "unproject", 2));
    JS_SetPropertyStr(ctx, proto, "project", JS_NewCFunction(ctx, camera_project, "project", 1));
    JS_SetPropertyStr(ctx, proto, "videoSource", JS_NewCFunction(ctx, camera_video_source, "videoSource", 2));
    JS_SetPropertyStr(ctx, proto, "videoState", JS_NewCFunction(ctx, camera_video_state, "videoState", 0));
    JS_SetPropertyStr(ctx, proto, "filmUv", JS_NewCFunction(ctx, camera_film_uv, "filmUv", 2));
    JS_SetPropertyStr(ctx, proto, "state", JS_NewCFunction(ctx, camera_state, "state", 0));
    JS_SetClassProto(ctx, camera_class_id, proto);
}

void register_ray_class(JSContext* ctx) {
    register_simple_class(ctx, ray_class_id, "ThreeDRay", ray_finalizer);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "hitPlane", JS_NewCFunction(ctx, ray_hit_plane, "hitPlane", 1));
    JS_SetPropertyStr(ctx, proto, "hitScene", JS_NewCFunction(ctx, ray_hit_scene, "hitScene", 0));
    JS_SetClassProto(ctx, ray_class_id, proto);
}

void register_node_class(JSContext* ctx) {
    register_simple_class(ctx, node_class_id, "ThreeDNode", node_finalizer);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "set", JS_NewCFunction(ctx, node_set, "set", 1));
    JS_SetPropertyStr(ctx, proto, "faceTarget", JS_NewCFunction(ctx, node_face_target, "faceTarget", 1));
    JS_SetPropertyStr(ctx, proto, "moveLocal", JS_NewCFunction(ctx, node_move_local, "moveLocal", 1));
    JS_SetPropertyStr(ctx, proto, "attachTo", JS_NewCFunction(ctx, node_attach_to, "attachTo", 2));
    JS_SetPropertyStr(ctx, proto, "detach", JS_NewCFunction(ctx, node_detach, "detach", 1));
    JS_SetPropertyStr(ctx, proto, "bounds", JS_NewCFunction(ctx, node_bounds, "bounds", 0));
    JS_SetPropertyStr(ctx, proto, "matrix", JS_NewCFunction(ctx, node_matrix, "matrix", 0));
    JS_SetPropertyStr(ctx, proto, "mesh", JS_NewCFunction(ctx, node_mesh, "mesh", 0));
    JS_SetPropertyStr(ctx, proto, "updateMesh", JS_NewCFunction(ctx, node_update_mesh, "updateMesh", 1));
    JS_SetPropertyStr(ctx, proto, "animateTo", JS_NewCFunction(ctx, node_animate_to, "animateTo", 2));
    JS_SetPropertyStr(ctx, proto, "attention", JS_NewCFunction(ctx, node_attention, "attention", 2));
    JS_SetPropertyStr(ctx, proto, "attentionState", JS_NewCFunction(ctx, node_attention_state, "attentionState", 0));
    JS_SetPropertyStr(ctx, proto, "get", JS_NewCFunction(ctx, node_get, "get", 0));
    JS_SetPropertyStr(ctx, proto, "remove", JS_NewCFunction(ctx, node_remove, "remove", 0));
    JS_SetClassProto(ctx, node_class_id, proto);
}

void register_texture_class(JSContext* ctx) {
    register_simple_class(ctx, texture_class_id, "ThreeDTexture", texture_finalizer);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "metadata", JS_NewCFunction(ctx, texture_metadata, "metadata", 0));
    JS_SetPropertyStr(ctx, proto, "map", JS_NewCFunction(ctx, texture_map, "map", 0));
    JS_SetPropertyStr(ctx, proto, "unmap", JS_NewCFunction(ctx, texture_unmap, "unmap", 1));
    JS_SetClassProto(ctx, texture_class_id, proto);
}

void register_timeline_class(JSContext* ctx) {
    register_simple_class(ctx, timeline_class_id, "ThreeDTimeline", timeline_finalizer);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "animate", JS_NewCFunction(ctx, timeline_animate, "animate", 2));
    JS_SetPropertyStr(ctx, proto, "pause", JS_NewCFunction(ctx, timeline_pause, "pause", 0));
    JS_SetPropertyStr(ctx, proto, "resume", JS_NewCFunction(ctx, timeline_resume, "resume", 0));
    JS_SetPropertyStr(ctx, proto, "cancel", JS_NewCFunction(ctx, timeline_cancel, "cancel", 0));
    JS_SetPropertyStr(ctx, proto, "state", JS_NewCFunction(ctx, timeline_state, "state", 0));
    JS_SetClassProto(ctx, timeline_class_id, proto);
}

void register_scene_class(JSContext* ctx) {
    register_simple_class(ctx, scene_class_id, "ThreeDScene", scene_finalizer);
    register_camera_class(ctx);
    register_ray_class(ctx);
    register_node_class(ctx);
    register_texture_class(ctx);
    register_timeline_class(ctx);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "publishTo", JS_NewCFunction(ctx, scene_publish_to, "publishTo", 1));
    JS_SetPropertyStr(ctx, proto, "resize", JS_NewCFunction(ctx, scene_resize, "resize", 1));
    JS_SetPropertyStr(ctx, proto, "metadata", JS_NewCFunction(ctx, scene_metadata, "metadata", 0));
    JS_SetPropertyStr(ctx, proto, "load", JS_NewCFunction(ctx, scene_load, "load", 2));
    JS_SetPropertyStr(ctx, proto, "setAmbientLight", JS_NewCFunction(ctx, scene_set_ambient_light, "setAmbientLight", 1));
    JS_SetPropertyStr(ctx, proto, "light", JS_NewCFunction(ctx, scene_light, "light", 1));
    JS_SetPropertyStr(ctx, proto, "mesh", JS_NewCFunction(ctx, scene_mesh, "mesh", 1));
    JS_SetPropertyStr(ctx, proto, "surfaceGrid", JS_NewCFunction(ctx, scene_surface_grid, "surfaceGrid", 1));
    JS_SetPropertyStr(ctx, proto, "primitive", JS_NewCFunction(ctx, scene_primitive, "primitive", 2));
    JS_SetPropertyStr(ctx, proto, "texture", JS_NewCFunction(ctx, scene_texture, "texture", 1));
    JS_SetPropertyStr(ctx, proto, "marker", JS_NewCFunction(ctx, scene_marker, "marker", 1));
    JS_SetPropertyStr(ctx, proto, "upsert", JS_NewCFunction(ctx, scene_upsert, "upsert", 2));
    JS_SetPropertyStr(ctx, proto, "project", JS_NewCFunction(ctx, scene_project, "project", 1));
    JS_SetPropertyStr(ctx, proto, "projectBox", JS_NewCFunction(ctx, scene_project_box, "projectBox", 1));
    JS_SetPropertyStr(ctx, proto, "overlay", JS_NewCFunction(ctx, scene_overlay, "overlay", 2));
    JS_SetPropertyStr(ctx, proto, "overlayState", JS_NewCFunction(ctx, scene_overlay_state, "overlayState", 1));
    JS_SetPropertyStr(ctx, proto, "timeline", JS_NewCFunction(ctx, scene_timeline, "timeline", 0));
    JS_SetPropertyStr(ctx, proto, "onPick", JS_NewCFunction(ctx, scene_on_pick, "onPick", 1));
    JS_SetPropertyStr(ctx, proto, "pick", JS_NewCFunction(ctx, scene_pick, "pick", 2));
    JS_SetPropertyStr(ctx, proto, "tick", JS_NewCFunction(ctx, scene_tick, "tick", 1));
    JS_SetPropertyStr(ctx, proto, "count", JS_NewCFunction(ctx, scene_count, "count", 0));
    JS_SetPropertyStr(ctx, proto, "surface", JS_NewCFunction(ctx, scene_surface, "surface", 1));
    JS_SetPropertyStr(ctx, proto, "pickSurface", JS_NewCFunction(ctx, scene_pick_surface, "pickSurface", 2));
    JS_SetPropertyStr(ctx, proto, "detectionSource", JS_NewCFunction(ctx, scene_detection_source, "detectionSource", 2));
    JS_SetPropertyStr(ctx, proto, "pollDetections", JS_NewCFunction(ctx, scene_poll_detections, "pollDetections", 1));
    JS_SetPropertyStr(ctx, proto, "particles", JS_NewCFunction(ctx, scene_particles, "particles", 1));
    JS_SetPropertyStr(ctx, proto, "particleCount", JS_NewCFunction(ctx, scene_particle_count, "particleCount", 0));
    JS_SetPropertyStr(ctx, proto, "emitterState", JS_NewCFunction(ctx, scene_emitter_state, "emitterState", 1));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, scene_close, "close", 0));
    JS_SetClassProto(ctx, scene_class_id, proto);
}

// Capability/diagnostic probe: reports whether the GPU renderer is compiled in
// and, when graphics is authorized, whether a GL context can be created (plus the
// device/GL-version strings). Drives the `wl2 graphics` CLI command. Creating a
// context needs the graphics capability, so this reports authorized=false rather
// than throwing when it is absent — a diagnostic should describe, not fail.
JSValue js_query_graphics(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    (void)argc;
    (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "renderer", JS_NewString(ctx, WL2_3D_HAVE_MAGNUM ? "magnum" : "synthetic"));
    bool authorized = false;
    bool available = false;
    std::string device;
    std::string glVersion;
#if WL2_3D_HAVE_MAGNUM
    if (auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx))) {
        authorized = static_cast<bool>(runtime->authorizeGraphics());
    }
    if (authorized && td::gpu_context_available()) {
        available = true;
        const td::GpuDeviceInfo info = td::gpu_device_info();
        device = info.device;
        glVersion = info.glVersion;
    }
#endif
    JS_SetPropertyStr(ctx, obj, "authorized", JS_NewBool(ctx, authorized));
    JS_SetPropertyStr(ctx, obj, "available", JS_NewBool(ctx, available));
    JS_SetPropertyStr(ctx, obj, "device", JS_NewString(ctx, device.c_str()));
    JS_SetPropertyStr(ctx, obj, "glVersion", JS_NewString(ctx, glVersion.c_str()));
    return obj;
}

int init_3d_module(JSContext* ctx, JSModuleDef* module) {
    register_scene_class(ctx);
    JSValue scene = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, scene, "create", JS_NewCFunction(ctx, scene_create, "create", 1));
    JS_SetModuleExport(ctx, module, "Scene", scene);
    JS_SetModuleExport(ctx, module, "hasRenderer", JS_NewBool(ctx, WL2_3D_HAVE_MAGNUM != 0));
    JS_SetModuleExport(ctx, module, "queryGraphics", JS_NewCFunction(ctx, js_query_graphics, "queryGraphics", 0));
    JS_SetModuleExport(ctx, module, "encodeDetection", JS_NewCFunction(ctx, js_encode_detection, "encodeDetection", 1));
    JS_SetModuleExport(ctx, module, "decodeDetection", JS_NewCFunction(ctx, js_decode_detection, "decodeDetection", 1));
    return 0;
}
