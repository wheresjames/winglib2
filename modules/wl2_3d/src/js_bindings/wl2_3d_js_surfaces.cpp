// --- UI-on-3D surfaces ------------------------------------------------------

bool read_pixels(JSContext* ctx, JSValueConst obj, int64_t& width, int64_t& height) {
    JSValue pixels = JS_GetPropertyStr(ctx, obj, "pixels");
    bool ok = false;
    if (JS_IsArray(ctx, pixels) > 0) {
        JSValue w = JS_GetPropertyUint32(ctx, pixels, 0);
        JSValue h = JS_GetPropertyUint32(ctx, pixels, 1);
        ok = JS_ToInt64(ctx, &width, w) == 0 && JS_ToInt64(ctx, &height, h) == 0;
        JS_FreeValue(ctx, w);
        JS_FreeValue(ctx, h);
    }
    JS_FreeValue(ctx, pixels);
    return ok && width > 0 && height > 0;
}

// Bind a UI FrameRing to a planar quad in the scene. Picking the surface yields
// the UV and the top-left UI pixel, which the caller injects into the off-screen
// Slint UI (the §4.2 input-command flow).
JSValue scene_surface(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "scene.surface(options) requires an options object");
    }
    JSValueConst opts = argv[0];
    td::Surface surface;
    JSValue name = JS_GetPropertyStr(ctx, opts, "name");
    if (JS_IsString(name)) {
        surface.ring = js_string(ctx, name);
    }
    JS_FreeValue(ctx, name);
    JSValue id = JS_GetPropertyStr(ctx, opts, "id");
    if (JS_IsString(id)) {
        surface.id = js_string(ctx, id);
    }
    JS_FreeValue(ctx, id);
    JSValue origin = JS_GetPropertyStr(ctx, opts, "origin");
    if (JS_IsUndefined(origin)) {
        JS_FreeValue(ctx, origin);
        origin = JS_GetPropertyStr(ctx, opts, "at");
    }
    if (!JS_IsUndefined(origin)) {
        read_vec3(ctx, origin, surface.origin);
    }
    JS_FreeValue(ctx, origin);
    JSValue uAxis = JS_GetPropertyStr(ctx, opts, "uAxis");
    if (!JS_IsUndefined(uAxis)) {
        read_vec3(ctx, uAxis, surface.uAxis);
    }
    JS_FreeValue(ctx, uAxis);
    JSValue vAxis = JS_GetPropertyStr(ctx, opts, "vAxis");
    if (!JS_IsUndefined(vAxis)) {
        read_vec3(ctx, vAxis, surface.vAxis);
    }
    JS_FreeValue(ctx, vAxis);
    if (!read_pixels(ctx, opts, surface.pixelWidth, surface.pixelHeight)) {
        return JS_ThrowTypeError(ctx, "scene.surface({ pixels: [width, height] }) is required");
    }

    const int64_t handle = scene->engine.addSurface(surface);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, surface.id.c_str()));
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, surface.ring.c_str()));
    JS_SetPropertyStr(ctx, obj, "handle", JS_NewInt64(ctx, handle));
    return obj;
}

JSValue scene_pick_surface(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "scene.pickSurface(px, py) requires two coordinates");
    }
    double px = 0.0, py = 0.0;
    if (JS_ToFloat64(ctx, &px, argv[0]) != 0 || JS_ToFloat64(ctx, &py, argv[1]) != 0) {
        return JS_EXCEPTION;
    }
    auto ray = td::unproject(scene->engine.camera, px, py, static_cast<double>(scene->width),
                             static_cast<double>(scene->height));
    if (!ray) {
        return JS_NULL;
    }
    auto hit = scene->engine.pickSurface(*ray);
    if (!hit) {
        return JS_NULL;
    }
    const td::Surface* surface = scene->engine.surface(hit->handle);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "surface", JS_NewString(ctx, surface ? surface->id.c_str() : ""));
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, surface ? surface->ring.c_str() : ""));
    JSValue uv = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, uv, 0, JS_NewFloat64(ctx, hit->u));
    JS_SetPropertyUint32(ctx, uv, 1, JS_NewFloat64(ctx, hit->v));
    JS_SetPropertyStr(ctx, obj, "uv", uv);
    JSValue pixel = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pixel, 0, JS_NewFloat64(ctx, hit->pixelX));
    JS_SetPropertyUint32(ctx, pixel, 1, JS_NewFloat64(ctx, hit->pixelY));
    JS_SetPropertyStr(ctx, obj, "pixel", pixel);
    JS_SetPropertyStr(ctx, obj, "point", vec3_to_js(ctx, hit->point));
    return obj;
}

