JSValue scene_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto scene = std::make_shared<SceneState>();
    if (argc > 0 && !read_size(ctx, argv[0], scene->width, scene->height)) {
        return rejected_promise(ctx, make_error("3d_invalid_argument", "Scene.create({ size }) requires positive width and height"));
    }
    scene->ctx = ctx;
    scene->engine.camera.aspect = static_cast<double>(scene->width) / static_cast<double>(scene->height);
    JSValue obj = scene_object(ctx, scene);
    if (JS_IsException(obj)) {
        return rejected_promise(ctx, make_error("3d_internal", "Unable to allocate Scene object"));
    }
    JS_SetPropertyStr(ctx, obj, "camera", camera_object(ctx, scene));
    JS_SetPropertyStr(ctx, obj, "ground", make_plane_object(ctx, scene->engine.ground));
    return resolved_promise(ctx, obj);
}

JSValue scene_publish_to(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Scene.publishTo(name) requires a shared-memory name");
    }
    if (!wl2::libmembusHasV12Surface()) {
        return throw_error(ctx, make_error("3d_unsupported", "wl2:3d FrameRing requires libmembus v1.2 video format support"));
    }
    auto name = js_string(ctx, argv[0]);
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return throw_error(ctx, make_error("shared_memory_denied", "Shared-memory access is not permitted without a runtime policy"));
    }
    if (auto allowed = runtime->authorizeSharedMemory(name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
#if WL2_3D_HAVE_MAGNUM
    if (auto allowed = runtime->authorizeGraphics(); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    scene->graphicsAuthorized = true;  // GPU writer is now eligible in write_scene_frame
#endif
    auto created = wl2::VideoBuffer::create(name, scene->width, scene->height, wl2::VideoPixelFormat::Rgba32, 30, scene->buffers);
    if (!created) {
        return throw_error(ctx, created.error());
    }
    stop_render_thread(*scene);
    scene->frameRing = std::move(created.value());
    scene->frameRingName = name;
    {
        std::lock_guard<std::mutex> lock(scene->frameRingMutex);
        if (auto written = write_scene_frame(*scene); !written) {
            scene->frameRing.close();
            return throw_error(ctx, written.error());
        }
    }
    start_render_thread(scene);
    std::lock_guard<std::mutex> lock(scene->frameRingMutex);
    return metadata(ctx, *scene);
}

// Dynamic resize (lifts the v1 fixed-viewport limit): recreate the ring at a new
// size under the same name and bump the resize generation so a consumer that
// re-opens by name picks up the new dimensions. The control-channel handshake
// (scene.controlChannel) carries the generation so consumers know to re-attach.
JSValue scene_resize(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    int64_t width = scene->width;
    int64_t height = scene->height;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) <= 0) {
        return JS_ThrowTypeError(ctx, "scene.resize([width, height]) requires a size array");
    }
    JSValue w = JS_GetPropertyUint32(ctx, argv[0], 0);
    JSValue h = JS_GetPropertyUint32(ctx, argv[0], 1);
    const bool ok = JS_ToInt64(ctx, &width, w) == 0 && JS_ToInt64(ctx, &height, h) == 0;
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, h);
    if (!ok || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return throw_error(ctx, make_error("3d_invalid_argument", "resize size must be positive and within bounds"));
    }
    if (!scene->frameRing.isOpen()) {
        // No ring yet: just update the negotiated size for the next publishTo.
        scene->width = width;
        scene->height = height;
        scene->engine.camera.aspect = static_cast<double>(width) / static_cast<double>(height);
        std::lock_guard<std::mutex> lock(scene->frameRingMutex);
        return metadata(ctx, *scene);
    }
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (runtime) {
        if (auto allowed = runtime->authorizeSharedMemory(scene->frameRingName); !allowed) {
            return throw_error(ctx, allowed.error());
        }
    }
    // Close/unlink the old ring before recreating under the same name, so the
    // new ring's shared memory is not unlinked by the old handle's close.
    stop_render_thread(*scene);
    {
        std::lock_guard<std::mutex> lock(scene->frameRingMutex);
        scene->frameRing.close();
        auto created = wl2::VideoBuffer::create(scene->frameRingName, width, height,
                                                wl2::VideoPixelFormat::Rgba32, 30, scene->buffers);
        if (!created) {
            return throw_error(ctx, created.error());
        }
        scene->frameRing = std::move(created.value());
        scene->width = width;
        scene->height = height;
        scene->engine.camera.aspect = static_cast<double>(width) / static_cast<double>(height);
        ++scene->resizeGeneration;
        if (auto written = write_scene_frame(*scene); !written) {
            scene->frameRing.close();
            return throw_error(ctx, written.error());
        }
    }
    start_render_thread(scene);
    std::lock_guard<std::mutex> lock(scene->frameRingMutex);
    JSValue meta = metadata(ctx, *scene);
    JS_SetPropertyStr(ctx, meta, "generation", JS_NewInt64(ctx, scene->resizeGeneration));
    return meta;
}

JSValue scene_metadata(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    std::lock_guard<std::mutex> lock(scene->frameRingMutex);
    return metadata(ctx, *scene);
}

JSValue scene_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    stop_render_thread(*scene);
    for (auto& entry : scene->tweenCallbacks) {
        JS_FreeValue(ctx, entry.second);
    }
    scene->tweenCallbacks.clear();
    JS_FreeValue(ctx, scene->onPick);
    scene->onPick = JS_UNDEFINED;
    std::lock_guard<std::mutex> lock(scene->frameRingMutex);
    scene->frameRing.close();
    if (scene->cameraVideo) {
        scene->cameraVideo->reader.close();
        scene->cameraVideo.reset();
    }
    scene->timelines.clear();
    return JS_UNDEFINED;
}

