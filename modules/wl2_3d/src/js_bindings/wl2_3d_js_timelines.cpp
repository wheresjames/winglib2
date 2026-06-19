// --- Timelines --------------------------------------------------------------

JSValue timeline_object(JSContext* ctx, std::shared_ptr<TimelineState> timeline) {
    JSValue obj = JS_NewObjectClass(ctx, timeline_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new TimelineBox{std::move(timeline)});
    return obj;
}

void read_timeline_common(JSContext* ctx, SceneState& scene, JSValueConst opts, TimelineItem& item) {
    read_number_prop(ctx, opts, "ms", item.durationMs);
    JSValue ease = JS_GetPropertyStr(ctx, opts, "ease");
    if (JS_IsString(ease)) {
        if (auto parsed = td::parseEase(js_string(ctx, ease))) {
            item.ease = *parsed;
        }
    }
    JS_FreeValue(ctx, ease);
    JSValue loop = JS_GetPropertyStr(ctx, opts, "loop");
    if (JS_IsBool(loop)) item.loop = JS_ToBool(ctx, loop) != 0;
    JS_FreeValue(ctx, loop);
    JSValue yoyo = JS_GetPropertyStr(ctx, opts, "yoyo");
    if (JS_IsBool(yoyo)) item.yoyo = JS_ToBool(ctx, yoyo) != 0;
    JS_FreeValue(ctx, yoyo);
    JSValue onComplete = JS_GetPropertyStr(ctx, opts, "onComplete");
    if (JS_IsFunction(ctx, onComplete)) {
        const int64_t id = scene.nextCallbackId++;
        scene.tweenCallbacks[id] = onComplete;  // takes ownership
        item.callbackId = id;
    } else {
        JS_FreeValue(ctx, onComplete);
    }
}

TimelineItem read_timeline_item(JSContext* ctx, SceneState& scene, int64_t nodeHandle, JSValueConst opts) {
    TimelineItem item;
    item.node = nodeHandle;
    td::Vec3 v;
    JSValue position = JS_GetPropertyStr(ctx, opts, "position");
    if (JS_IsUndefined(position)) {
        JS_FreeValue(ctx, position);
        position = JS_GetPropertyStr(ctx, opts, "at");
    }
    if (!JS_IsUndefined(position) && read_vec3(ctx, position, v)) item.toPosition = v;
    JS_FreeValue(ctx, position);
    JSValue scale = JS_GetPropertyStr(ctx, opts, "scale");
    if (JS_IsNumber(scale)) {
        double s = 1.0;
        JS_ToFloat64(ctx, &s, scale);
        item.toScale = td::Vec3{s, s, s};
    } else if (!JS_IsUndefined(scale) && read_vec3(ctx, scale, v)) {
        item.toScale = v;
    }
    JS_FreeValue(ctx, scale);
    JSValue rotation = JS_GetPropertyStr(ctx, opts, "rotation");
    if (!JS_IsUndefined(rotation) && read_vec3(ctx, rotation, v)) item.toRotation = v;
    JS_FreeValue(ctx, rotation);
    double opacity = 0.0;
    if (read_number_prop(ctx, opts, "opacity", opacity)) item.toOpacity = opacity;
    JSValue color = JS_GetPropertyStr(ctx, opts, "color");
    if (!JS_IsUndefined(color)) {
        td::Color c;
        if (read_color(ctx, color, c)) item.toColor = c;
    }
    JS_FreeValue(ctx, color);
    read_timeline_common(ctx, scene, opts, item);
    return item;
}

TimelineItem read_camera_timeline_item(JSContext* ctx, SceneState& scene, JSValueConst opts) {
    TimelineItem item;
    item.camera = true;
    td::Vec3 v;
    JSValue eye = JS_GetPropertyStr(ctx, opts, "eye");
    if (!JS_IsUndefined(eye) && read_vec3(ctx, eye, v)) item.toEye = v;
    JS_FreeValue(ctx, eye);
    JSValue target = JS_GetPropertyStr(ctx, opts, "target");
    if (!JS_IsUndefined(target) && read_vec3(ctx, target, v)) item.toTarget = v;
    JS_FreeValue(ctx, target);
    double fovY = 0.0;
    if (read_number_prop(ctx, opts, "fovY", fovY)) item.toFovYRadians = td::radians(fovY);
    double nearPlane = 0.0;
    if (read_number_prop(ctx, opts, "near", nearPlane)) item.toNear = nearPlane;
    double farPlane = 0.0;
    if (read_number_prop(ctx, opts, "far", farPlane)) item.toFar = farPlane;
    read_timeline_common(ctx, scene, opts, item);
    return item;
}

TimelineItem read_overlay_timeline_item(JSContext* ctx, SceneState& scene, int64_t handle, JSValueConst opts) {
    TimelineItem item;
    item.overlay = handle;
    td::Vec3 v;
    JSValue offset = JS_GetPropertyStr(ctx, opts, "offset");
    if (!JS_IsUndefined(offset) && read_vec3(ctx, offset, v)) item.toOverlayOffset = v;
    JS_FreeValue(ctx, offset);
    read_timeline_common(ctx, scene, opts, item);
    return item;
}

JSValue timeline_animate(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    TimelineBox* box = get_timeline_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "timeline.animate(target, options) requires a target and options");
    }
    auto scene = box->timeline->scene.lock();
    if (!scene) {
        return throw_error(ctx, make_error("3d_invalid_argument", "timeline scene is closed"));
    }
    if (auto* camera = static_cast<CameraBox*>(JS_GetOpaque(argv[0], camera_class_id))) {
        if (camera->scene.get() != scene.get()) {
            return JS_ThrowTypeError(ctx, "timeline.animate(target, options) requires a target from the same scene");
        }
        box->timeline->items.push_back(read_camera_timeline_item(ctx, *scene, argv[1]));
        return JS_DupValue(ctx, thisVal);
    }
    if (auto* node = static_cast<NodeBox*>(JS_GetOpaque(argv[0], node_class_id))) {
        if (node->scene.get() != scene.get()) {
            return JS_ThrowTypeError(ctx, "timeline.animate(target, options) requires a target from the same scene");
        }
        box->timeline->items.push_back(read_timeline_item(ctx, *scene, node->handle, argv[1]));
        return JS_DupValue(ctx, thisVal);
    }
    if (JS_IsObject(argv[0])) {
        JSValue handleValue = JS_GetPropertyStr(ctx, argv[0], "handle");
        int64_t handle = 0;
        const int ok = JS_IsNumber(handleValue) ? JS_ToInt64(ctx, &handle, handleValue) : -1;
        JS_FreeValue(ctx, handleValue);
        if (ok == 0 && handle != 0 && scene->engine.overlay(handle)) {
            box->timeline->items.push_back(read_overlay_timeline_item(ctx, *scene, handle, argv[1]));
            return JS_DupValue(ctx, thisVal);
        }
    }
    return JS_ThrowTypeError(ctx, "timeline.animate(target, options) requires a node, camera, or overlay from the same scene");
}

JSValue timeline_pause(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    TimelineBox* box = get_timeline_box(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    box->timeline->paused = true;
    return JS_DupValue(ctx, thisVal);
}

JSValue timeline_resume(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    TimelineBox* box = get_timeline_box(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    box->timeline->paused = false;
    return JS_DupValue(ctx, thisVal);
}

JSValue timeline_cancel(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    TimelineBox* box = get_timeline_box(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    box->timeline->canceled = true;
    auto scene = box->timeline->scene.lock();
    for (auto& item : box->timeline->items) {
        item.done = true;
        if (scene && item.callbackId != 0) {
            auto it = scene->tweenCallbacks.find(item.callbackId);
            if (it != scene->tweenCallbacks.end()) {
                JS_FreeValue(ctx, it->second);
                scene->tweenCallbacks.erase(it);
            }
            item.callbackId = 0;
        }
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue timeline_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    TimelineBox* box = get_timeline_box(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    int64_t active = 0;
    int64_t done = 0;
    for (const auto& item : box->timeline->items) {
        if (item.done) ++done;
        else ++active;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "active", JS_NewInt64(ctx, active));
    JS_SetPropertyStr(ctx, obj, "done", JS_NewInt64(ctx, done));
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, box->timeline->name.c_str()));
    JS_SetPropertyStr(ctx, obj, "paused", JS_NewBool(ctx, box->timeline->paused));
    JS_SetPropertyStr(ctx, obj, "canceled", JS_NewBool(ctx, box->timeline->canceled));
    return obj;
}

JSValue scene_timeline(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    auto timeline = std::make_shared<TimelineState>();
    if (argc >= 1 && JS_IsString(argv[0])) {
        timeline->name = js_string(ctx, argv[0]);
    }
    timeline->scene = scene;
    scene->timelines.push_back(timeline);
    return timeline_object(ctx, std::move(timeline));
}

