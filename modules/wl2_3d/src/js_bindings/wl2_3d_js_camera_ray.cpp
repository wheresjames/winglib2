// ----- Camera methods -------------------------------------------------------

JSValue camera_calibrate(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    td::Camera& cam = box->scene->engine.camera;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        double focal = 0.0;
        if (read_number_prop(ctx, opts, "focal", focal)) {
            JSValue sensor = JS_GetPropertyStr(ctx, opts, "sensor");
            td::Vec3 s{36.0, 24.0, 0.0};
            read_vec3(ctx, sensor, s);
            JS_FreeValue(ctx, sensor);
            cam.setLens(focal, s.x, s.y);
        }
        double fovY = 0.0;
        if (read_number_prop(ctx, opts, "fovY", fovY)) {
            cam.fovYRadians = td::radians(fovY);
        }
        read_number_prop(ctx, opts, "aspect", cam.aspect);
        read_number_prop(ctx, opts, "near", cam.near);
        read_number_prop(ctx, opts, "far", cam.far);
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue camera_look_from(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    td::Camera& cam = box->scene->engine.camera;
    if (argc >= 1) {
        read_vec3(ctx, argv[0], cam.eye);
    }
    if (argc >= 2) {
        read_vec3(ctx, argv[1], cam.target);
    }
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        read_vec3(ctx, argv[2], cam.up);
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue camera_unproject(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "camera.unproject(px, py) requires two coordinates");
    }
    double px = 0.0, py = 0.0;
    if (JS_ToFloat64(ctx, &px, argv[0]) != 0 || JS_ToFloat64(ctx, &py, argv[1]) != 0) {
        return JS_EXCEPTION;
    }
    SceneState& scene = *box->scene;
    auto ray = td::unproject(scene.engine.camera, px, py, static_cast<double>(scene.width),
                             static_cast<double>(scene.height));
    if (!ray) {
        return JS_NULL;
    }
    return ray_object(ctx, box->scene, *ray);
}

JSValue camera_project(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "camera.project(world) requires a world point");
    }
    td::Vec3 world;
    if (!read_vec3(ctx, argv[0], world)) {
        return JS_ThrowTypeError(ctx, "camera.project(world) expects [x, y, z]");
    }
    SceneState& scene = *box->scene;
    return screen_point_object(ctx, td::project(scene.engine.camera, world,
                                                static_cast<double>(scene.width),
                                                static_cast<double>(scene.height)));
}

JSValue camera_video_state_object(JSContext* ctx, const CameraVideoState& video) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, video.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewString(ctx, video.mode.c_str()));
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt64(ctx, video.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt64(ctx, video.height));
    JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, video.reader.sequence()));
    JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, "rgba8"));
    JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, "top-left"));
    JS_SetPropertyStr(ctx, obj, "alpha", JS_NewString(ctx, "premultiplied"));
    JS_SetPropertyStr(ctx, obj, "colorSpace", JS_NewString(ctx, "srgb"));
    return obj;
}

JSValue camera_video_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "camera.videoSource(name, options) requires a FrameRing name");
    }
    const std::string name = js_string(ctx, argv[0]);
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return throw_error(ctx, make_error("shared_memory_denied", "Shared-memory access is not permitted without a runtime policy"));
    }
    if (auto allowed = runtime->authorizeSharedMemory(name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto opened = wl2::VideoBuffer::openExisting(name);
    if (!opened) {
        return throw_error(ctx, opened.error());
    }
    wl2::VideoBuffer reader = std::move(opened.value());
    if (reader.width() <= 0 || reader.height() <= 0 || reader.bytesPerPixel() != 4 ||
        reader.formatName() != "RGBA32") {
        return throw_error(ctx, make_error("3d_invalid_frame", "camera.videoSource() requires an RGBA32 FrameRing"));
    }
    auto frame = reader.frame(0);
    if (!frame || !frame.value().data || frame.value().scanWidth < frame.value().width * 4 ||
        frame.value().size < static_cast<size_t>(frame.value().scanWidth * frame.value().height)) {
        return throw_error(ctx, make_error("3d_invalid_frame", "camera.videoSource() rejected invalid frame metadata"));
    }
    CameraVideoState video;
    video.reader = std::move(reader);
    video.name = name;
    video.width = video.reader.width();
    video.height = video.reader.height();
    video.sequence = video.reader.sequence();
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue mode = JS_GetPropertyStr(ctx, argv[1], "mode");
        if (JS_IsString(mode)) {
            video.mode = js_string(ctx, mode);
        }
        JS_FreeValue(ctx, mode);
    }
    if (video.mode != "background" && video.mode != "filmPlane" && video.mode != "projectedTexture") {
        return throw_error(ctx, make_error("3d_invalid_argument", "camera video mode must be background, filmPlane, or projectedTexture"));
    }
    box->scene->cameraVideo = std::move(video);
    return camera_video_state_object(ctx, *box->scene->cameraVideo);
}

JSValue camera_video_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (!box->scene->cameraVideo) {
        return JS_NULL;
    }
    return camera_video_state_object(ctx, *box->scene->cameraVideo);
}

JSValue camera_film_uv(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "camera.filmUv(px, py) requires two coordinates");
    }
    double px = 0.0, py = 0.0;
    if (JS_ToFloat64(ctx, &px, argv[0]) != 0 || JS_ToFloat64(ctx, &py, argv[1]) != 0) {
        return JS_EXCEPTION;
    }
    const double w = box->scene->cameraVideo ? static_cast<double>(box->scene->cameraVideo->width)
                                             : static_cast<double>(box->scene->width);
    const double h = box->scene->cameraVideo ? static_cast<double>(box->scene->cameraVideo->height)
                                             : static_cast<double>(box->scene->height);
    JSValue uv = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, uv, 0, JS_NewFloat64(ctx, w > 0.0 ? px / w : 0.0));
    JS_SetPropertyUint32(ctx, uv, 1, JS_NewFloat64(ctx, h > 0.0 ? py / h : 0.0));
    return uv;
}

JSValue camera_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    CameraBox* box = get_camera(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const td::Camera& cam = box->scene->engine.camera;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "eye", vec3_to_js(ctx, cam.eye));
    JS_SetPropertyStr(ctx, obj, "target", vec3_to_js(ctx, cam.target));
    JS_SetPropertyStr(ctx, obj, "up", vec3_to_js(ctx, cam.up));
    JS_SetPropertyStr(ctx, obj, "fovY", JS_NewFloat64(ctx, cam.fovYRadians * 180.0 / td::kPi));
    JS_SetPropertyStr(ctx, obj, "near", JS_NewFloat64(ctx, cam.near));
    JS_SetPropertyStr(ctx, obj, "far", JS_NewFloat64(ctx, cam.far));
    JS_SetPropertyStr(ctx, obj, "aspect", JS_NewFloat64(ctx, cam.aspect));
    return obj;
}

// ----- Ray methods ----------------------------------------------------------

JSValue ray_hit_plane(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    RayBox* box = get_ray(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    td::Plane plane = (argc >= 1) ? read_plane(ctx, argv[0]) : td::Plane{};
    auto hit = td::intersect(box->ray, plane);
    if (!hit) {
        return JS_NULL;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "point", vec3_to_js(ctx, *hit));
    return obj;
}

JSValue ray_hit_scene(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    RayBox* box = get_ray(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const int64_t handle = box->scene->engine.pick(box->ray);
    if (handle == 0) {
        return JS_NULL;
    }
    JSValue node = node_object(ctx, box->scene, handle);
    const td::Vec3 world = box->scene->engine.worldPosition(handle);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "node", node);
    JS_SetPropertyStr(ctx, obj, "point", vec3_to_js(ctx, world));
    return obj;
}

