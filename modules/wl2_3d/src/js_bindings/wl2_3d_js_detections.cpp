// --- Detections -> scene (§14.2) --------------------------------------------

bool js_to_bytes(JSContext* ctx, JSValueConst value, std::string& out) {
    if (JS_IsString(value)) {
        out = js_string(ctx, value);
        return true;
    }
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buffer)) {
        size_t size = 0;
        uint8_t* data = JS_GetArrayBuffer(ctx, &size, buffer);
        const bool ok = data && byteOffset + byteLength <= size;
        if (ok) {
            out.assign(reinterpret_cast<char*>(data) + byteOffset, byteLength);
        }
        JS_FreeValue(ctx, buffer);
        return ok;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t size = 0;
    uint8_t* data = JS_GetArrayBuffer(ctx, &size, value);
    if (data) {
        out.assign(reinterpret_cast<char*>(data), size);
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
}

td::Detection js_to_detection(JSContext* ctx, JSValueConst obj) {
    td::Detection d;
    double n = 0.0;
    if (read_number_prop(ctx, obj, "cameraId", n)) d.cameraId = static_cast<int32_t>(n);
    if (read_number_prop(ctx, obj, "id", n)) d.id = static_cast<int32_t>(n);
    if (read_number_prop(ctx, obj, "sourceFrameSeq", n)) d.sourceFrameSeq = static_cast<int64_t>(n);
    read_number_prop(ctx, obj, "ts", d.ts);
    read_number_prop(ctx, obj, "px", d.px);
    read_number_prop(ctx, obj, "py", d.py);
    if (read_number_prop(ctx, obj, "imageWidth", n)) d.imageWidth = static_cast<int32_t>(n);
    if (read_number_prop(ctx, obj, "imageHeight", n)) d.imageHeight = static_cast<int32_t>(n);
    double conf = 0.0;
    if (read_number_prop(ctx, obj, "confidence", conf)) d.confidence = static_cast<float>(conf);
    JSValue coord = JS_GetPropertyStr(ctx, obj, "coord");
    if (JS_IsString(coord)) {
        d.coord = js_string(ctx, coord) == "normalized-top-left" ? td::DetectionCoord::NormalizedTopLeft
                                                                  : td::DetectionCoord::PixelTopLeft;
    }
    JS_FreeValue(ctx, coord);
    JSValue klass = JS_GetPropertyStr(ctx, obj, "class");
    if (JS_IsString(klass)) {
        d.klass = js_string(ctx, klass);
    }
    JS_FreeValue(ctx, klass);
    JSValue bbox = JS_GetPropertyStr(ctx, obj, "bbox");
    if (JS_IsArray(ctx, bbox) > 0) {
        d.hasBbox = true;
        for (int i = 0; i < 4; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, bbox, static_cast<uint32_t>(i));
            double b = 0.0;
            JS_ToFloat64(ctx, &b, e);
            d.bbox[i] = static_cast<float>(b);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, bbox);
    return d;
}

JSValue detection_to_js(JSContext* ctx, const td::Detection& d) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "cameraId", JS_NewInt32(ctx, d.cameraId));
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, d.id));
    JS_SetPropertyStr(ctx, obj, "sourceFrameSeq", JS_NewInt64(ctx, d.sourceFrameSeq));
    JS_SetPropertyStr(ctx, obj, "ts", JS_NewFloat64(ctx, d.ts));
    JS_SetPropertyStr(ctx, obj, "px", JS_NewFloat64(ctx, d.px));
    JS_SetPropertyStr(ctx, obj, "py", JS_NewFloat64(ctx, d.py));
    JS_SetPropertyStr(ctx, obj, "imageWidth", JS_NewInt32(ctx, d.imageWidth));
    JS_SetPropertyStr(ctx, obj, "imageHeight", JS_NewInt32(ctx, d.imageHeight));
    JS_SetPropertyStr(ctx, obj, "confidence", JS_NewFloat64(ctx, d.confidence));
    JS_SetPropertyStr(ctx, obj, "coord",
        JS_NewString(ctx, d.coord == td::DetectionCoord::NormalizedTopLeft ? "normalized-top-left" : "pixel-top-left"));
    JS_SetPropertyStr(ctx, obj, "class", JS_NewString(ctx, d.klass.c_str()));
    if (d.hasBbox) {
        JSValue bbox = JS_NewArray(ctx);
        for (int i = 0; i < 4; ++i) {
            JS_SetPropertyUint32(ctx, bbox, static_cast<uint32_t>(i), JS_NewFloat64(ctx, d.bbox[i]));
        }
        JS_SetPropertyStr(ctx, obj, "bbox", bbox);
    }
    return obj;
}

JSValue js_encode_detection(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "encodeDetection(record) requires a record object");
    }
    const std::string bytes = td::encodeDetection(js_to_detection(ctx, argv[0]));
    return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

JSValue js_decode_detection(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    std::string bytes;
    if (argc < 1 || !js_to_bytes(ctx, argv[0], bytes)) {
        return JS_ThrowTypeError(ctx, "decodeDetection(bytes) requires a string or ArrayBuffer");
    }
    auto decoded = td::decodeDetection(bytes);
    if (!decoded) {
        return JS_NULL;
    }
    return detection_to_js(ctx, *decoded);
}

JSValue scene_detection_source(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scene.detectionSource(name[, options]) requires a queue name");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 65536;
    double sizeD = 65536.0;
    if (argc >= 2 && JS_IsObject(argv[1]) && read_number_prop(ctx, argv[1], "size", sizeD)) {
        size = static_cast<int64_t>(sizeD);
    }
    if (size <= 0 || size > (1 << 26)) {
        return throw_error(ctx, make_error("3d_invalid_argument", "detectionSource size out of range"));
    }
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return throw_error(ctx, make_error("shared_memory_denied", "Shared-memory access is not permitted without a runtime policy"));
    }
    if (auto allowed = runtime->authorizeSharedMemory(name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto attached = wl2::SharedQueue::attach(name, static_cast<size_t>(size), false);
    if (!attached) {
        return throw_error(ctx, attached.error());
    }
    scene->detectionQueue.emplace(std::move(attached.value()));
    return JS_UNDEFINED;
}

JSValue scene_poll_detections(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (!scene->detectionQueue) {
        return throw_error(ctx, make_error("3d_invalid_argument", "call scene.detectionSource(name) before pollDetections()"));
    }
    std::string model;
    std::string attention;
    double minConfidence = 0.0;
    int64_t maxRecords = 1024;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[0], "model");
        if (JS_IsString(m)) model = js_string(ctx, m);
        JS_FreeValue(ctx, m);
        JSValue a = JS_GetPropertyStr(ctx, argv[0], "attention");
        if (JS_IsString(a)) attention = js_string(ctx, a);
        JS_FreeValue(ctx, a);
        read_number_prop(ctx, argv[0], "minConfidence", minConfidence);
        double maxD = 1024.0;
        if (read_number_prop(ctx, argv[0], "max", maxD)) maxRecords = static_cast<int64_t>(maxD);
    }

    int64_t read = 0, placed = 0, missed = 0, invalid = 0, filtered = 0;
    while (read < maxRecords && scene->detectionQueue->poll()) {
        auto message = scene->detectionQueue->read(std::chrono::milliseconds{0});
        if (!message) {
            break;
        }
        if (message.value().empty()) {
            continue;
        }
        ++read;
        auto detection = td::decodeDetection(message.value());
        if (!detection) {
            ++invalid;
            continue;
        }
        if (detection->confidence < minConfidence) {
            ++filtered;
            continue;
        }
        double vx = 0.0, vy = 0.0;
        td::detectionToViewport(*detection, static_cast<double>(scene->width),
                                static_cast<double>(scene->height), vx, vy);
        auto ray = td::unproject(scene->engine.camera, vx, vy, static_cast<double>(scene->width),
                                 static_cast<double>(scene->height));
        if (!ray) {
            ++missed;
            continue;
        }
        auto hit = td::intersect(*ray, scene->engine.ground);
        if (!hit) {
            ++missed;
            continue;
        }
        const std::string id = std::to_string(detection->cameraId) + ":" + std::to_string(detection->id);
        int64_t handle = scene->engine.findById(id);
        if (handle == 0) {
            td::Node node;
            node.id = id;
            node.model = model;
            node.position = *hit;
            node.radius = 0.5;
            if (!attention.empty()) {
                node.attention.kind = attention_kind(attention);
            }
            scene->engine.addNode(std::move(node));
        } else if (td::Node* node = scene->engine.node(handle)) {
            node->position = *hit;
            if (!model.empty()) node->model = model;
        }
        ++placed;
    }

    JSValue summary = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, summary, "read", JS_NewInt64(ctx, read));
    JS_SetPropertyStr(ctx, summary, "placed", JS_NewInt64(ctx, placed));
    JS_SetPropertyStr(ctx, summary, "missed", JS_NewInt64(ctx, missed));
    JS_SetPropertyStr(ctx, summary, "invalid", JS_NewInt64(ctx, invalid));
    JS_SetPropertyStr(ctx, summary, "filtered", JS_NewInt64(ctx, filtered));
    return summary;
}

