// ----- Scene-graph methods --------------------------------------------------

JSValue scene_marker(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    td::Node node;
    node.kind = "marker";
    node.radius = 0.5;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue id = JS_GetPropertyStr(ctx, argv[0], "id");
        if (JS_IsString(id)) {
            node.id = js_string(ctx, id);
        }
        JS_FreeValue(ctx, id);
        apply_node_options(ctx, node, argv[0]);
    }
    const int64_t handle = scene->engine.addNode(std::move(node));
    return node_object(ctx, scene, handle);
}

JSValue scene_set_ambient_light(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scene.setAmbientLight(color) requires a color");
    }
    td::Color color = scene->engine.ambientLight;
    if (!read_color(ctx, argv[0], color)) {
        return JS_ThrowTypeError(ctx, "scene.setAmbientLight(color) expects #rrggbb, [r,g,b], or {r,g,b}");
    }
    scene->engine.ambientLight = color;
    if (scene->frameRing.isOpen()) {
        std::lock_guard<std::mutex> lock(scene->frameRingMutex);
        auto written = write_scene_frame(*scene);
        if (!written) {
            return throw_error(ctx, written.error());
        }
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue scene_light(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "scene.light(options) requires an options object");
    }
    td::Node node;
    node.kind = "light";
    node.visible = false;
    node.radius = 0.1;
    node.boundsMin = {-0.1, -0.1, -0.1};
    node.boundsMax = {0.1, 0.1, 0.1};
    JSValue id = JS_GetPropertyStr(ctx, argv[0], "id");
    if (JS_IsString(id)) {
        node.id = js_string(ctx, id);
    }
    JS_FreeValue(ctx, id);
    apply_node_options(ctx, node, argv[0]);

    td::Light light;
    light.id = node.id;
    light.position = node.position;
    JSValue kind = JS_GetPropertyStr(ctx, argv[0], "kind");
    if (JS_IsString(kind)) {
        const std::string text = js_string(ctx, kind);
        if (text == "point") {
            light.kind = td::LightKind::Point;
        } else if (text == "directional") {
            light.kind = td::LightKind::Directional;
        } else {
            JS_FreeValue(ctx, kind);
            return throw_error(ctx, make_error("3d_invalid_argument", "Unsupported light kind: " + text));
        }
    }
    JS_FreeValue(ctx, kind);
    JSValue direction = JS_GetPropertyStr(ctx, argv[0], "direction");
    if (!JS_IsUndefined(direction)) {
        read_vec3(ctx, direction, light.direction);
    }
    JS_FreeValue(ctx, direction);
    JSValue color = JS_GetPropertyStr(ctx, argv[0], "color");
    if (!JS_IsUndefined(color)) {
        read_color(ctx, color, light.color);
    }
    JS_FreeValue(ctx, color);
    read_number_prop(ctx, argv[0], "intensity", light.intensity);
    read_number_prop(ctx, argv[0], "range", light.range);
    JSValue visible = JS_GetPropertyStr(ctx, argv[0], "visible");
    if (JS_IsBool(visible)) {
        light.visible = JS_ToBool(ctx, visible) != 0;
    }
    JS_FreeValue(ctx, visible);

    const int64_t handle = scene->engine.addNode(std::move(node));
    scene->engine.setLight(handle, std::move(light));
    if (scene->frameRing.isOpen()) {
        std::lock_guard<std::mutex> lock(scene->frameRingMutex);
        auto written = write_scene_frame(*scene);
        if (!written) {
            return throw_error(ctx, written.error());
        }
    }
    return node_object(ctx, scene, handle);
}

bool has_prefix(std::string_view text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool supported_asset_url(std::string_view url) {
    const auto dot = url.find_last_of('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string ext(url.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".gltf" || ext == ".glb";
}

JSValue scene_load(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return rejected_promise(ctx, make_error("3d_invalid_argument", "scene.load(url) requires a wl2:/ resource URL"));
    }
    const std::string url = js_string(ctx, argv[0]);
    if (!has_prefix(url, "wl2:/")) {
        return rejected_promise(ctx, make_error("3d_invalid_argument", "scene.load() only accepts wl2:/ resources in v1"));
    }
    if (!supported_asset_url(url)) {
        return rejected_promise(ctx, make_error("3d_unsupported_asset", "scene.load() supports .gltf and .glb resources"));
    }
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return rejected_promise(ctx, make_error("3d_invalid_argument", "scene.load() requires a runtime resource store"));
    }
    auto opened = runtime->resources().open(url);
    if (!opened) {
        return rejected_promise(ctx, opened.error());
    }

    td::Node node;
    node.kind = "asset";
    node.model = url;
    node.assetUrl = url;
    node.resourceSize = opened.value().size();
    node.radius = 1.0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue id = JS_GetPropertyStr(ctx, argv[1], "id");
        if (JS_IsString(id)) {
            node.id = js_string(ctx, id);
        }
        JS_FreeValue(ctx, id);
        apply_node_options(ctx, node, argv[1]);
    }
    const int64_t handle = scene->engine.addNode(std::move(node));
    return resolved_promise(ctx, node_object(ctx, scene, handle));
}

bool valid_primitive_kind(const std::string& kind) {
    static const char* valid[] = {"cube", "sphere", "cylinder", "cone", "arrow", "grid", "plane", "skydome"};
    for (const char* item : valid) {
        if (kind == item) {
            return true;
        }
    }
    return false;
}

void read_primitive_number(JSContext* ctx, JSValueConst opts, const char* key, td::Node& node) {
    double value = 0.0;
    if (read_number_prop(ctx, opts, key, value)) {
        node.primitiveOptions[key] = value;
    }
}

JSValue scene_primitive(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "scene.primitive(kind, options) requires a primitive kind");
    }
    const std::string kind = js_string(ctx, argv[0]);
    if (!valid_primitive_kind(kind)) {
        return throw_error(ctx, make_error("3d_unsupported_primitive", "Unsupported primitive kind: " + kind));
    }
    td::Node node;
    node.kind = "primitive";
    node.primitive = kind;
    node.radius = 0.5;
    node.material = {0.7, 0.7, 0.7, 1.0};
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue id = JS_GetPropertyStr(ctx, argv[1], "id");
        if (JS_IsString(id)) {
            node.id = js_string(ctx, id);
        }
        JS_FreeValue(ctx, id);
        apply_node_options(ctx, node, argv[1]);
        read_primitive_number(ctx, argv[1], "radius", node);
        read_primitive_number(ctx, argv[1], "height", node);
        read_primitive_number(ctx, argv[1], "segments", node);
        read_primitive_number(ctx, argv[1], "size", node);
        read_primitive_number(ctx, argv[1], "divisions", node);
    }
    const int64_t handle = scene->engine.addNode(std::move(node));
    return node_object(ctx, scene, handle);
}

JSValue scene_mesh(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "scene.mesh(options) requires an options object");
    }
    td::MeshGeometry mesh;
    std::string error;
    if (!read_mesh_geometry(ctx, argv[0], true, mesh, error)) {
        return throw_error(ctx, make_error("3d_invalid_mesh", error));
    }
    td::Node node;
    node.kind = "mesh";
    node.mesh = std::move(mesh);
    node.material = {0.7, 0.7, 0.7, 1.0};
    JSValue id = JS_GetPropertyStr(ctx, argv[0], "id");
    if (JS_IsString(id)) {
        node.id = js_string(ctx, id);
    }
    JS_FreeValue(ctx, id);
    apply_node_options(ctx, node, argv[0]);
    scene->engine.refreshBounds(node);
    const int64_t handle = scene->engine.addNode(std::move(node));
    return node_object(ctx, scene, handle);
}

JSValue scene_surface_grid(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "scene.surfaceGrid(options) requires an options object");
    }
    double columnsD = 0.0, rowsD = 0.0;
    if (!read_number_prop(ctx, argv[0], "columns", columnsD) ||
        !read_number_prop(ctx, argv[0], "rows", rowsD)) {
        return throw_error(ctx, make_error("3d_invalid_mesh", "surfaceGrid requires columns and rows"));
    }
    const int columns = static_cast<int>(columnsD);
    const int rows = static_cast<int>(rowsD);
    if (columns < 2 || rows < 2 || columnsD != columns || rowsD != rows ||
        static_cast<size_t>(columns) * static_cast<size_t>(rows) > kMaxMeshVertices) {
        return throw_error(ctx, make_error("3d_invalid_mesh", "surfaceGrid columns and rows must be integer counts >= 2 within mesh limits"));
    }
    double width = 1.0;
    double height = 1.0;
    read_number_prop(ctx, argv[0], "width", width);
    read_number_prop(ctx, argv[0], "height", height);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
        return throw_error(ctx, make_error("3d_invalid_mesh", "surfaceGrid width and height must be positive finite numbers"));
    }

    td::MeshGeometry mesh;
    mesh.vertices.reserve(static_cast<size_t>(columns) * static_cast<size_t>(rows));
    mesh.indices.reserve(static_cast<size_t>(columns - 1) * static_cast<size_t>(rows - 1) * 6);
    const double halfW = width * 0.5;
    const double halfH = height * 0.5;
    for (int y = 0; y < rows; ++y) {
        const double fy = rows > 1 ? static_cast<double>(y) / (rows - 1) : 0.0;
        for (int x = 0; x < columns; ++x) {
            const double fx = columns > 1 ? static_cast<double>(x) / (columns - 1) : 0.0;
            mesh.vertices.push_back({fx * width - halfW, fy * height - halfH, 0.0});
            mesh.uvs.push_back(fx);
            mesh.uvs.push_back(fy);
        }
    }
    for (int y = 0; y < rows - 1; ++y) {
        for (int x = 0; x < columns - 1; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * columns + x);
            const uint32_t b = static_cast<uint32_t>(y * columns + x + 1);
            const uint32_t c = static_cast<uint32_t>((y + 1) * columns + x + 1);
            const uint32_t d = static_cast<uint32_t>((y + 1) * columns + x);
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }
    }
    JSValue dynamic = JS_GetPropertyStr(ctx, argv[0], "dynamic");
    if (JS_IsBool(dynamic)) {
        mesh.dynamic = JS_ToBool(ctx, dynamic) != 0;
    }
    JS_FreeValue(ctx, dynamic);

    td::Node node;
    node.kind = "mesh";
    node.mesh = std::move(mesh);
    node.material = {0.7, 0.7, 0.7, 1.0};
    JSValue id = JS_GetPropertyStr(ctx, argv[0], "id");
    if (JS_IsString(id)) {
        node.id = js_string(ctx, id);
    }
    JS_FreeValue(ctx, id);
    apply_node_options(ctx, node, argv[0]);
    scene->engine.refreshBounds(node);
    const int64_t handle = scene->engine.addNode(std::move(node));
    return node_object(ctx, scene, handle);
}

JSValue scene_upsert(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scene.upsert(id, options) requires an id");
    }
    const std::string id = js_string(ctx, argv[0]);
    int64_t handle = scene->engine.findById(id);
    if (handle == 0) {
        td::Node node;
        node.id = id;
        node.radius = 0.5;
        if (argc >= 2 && JS_IsObject(argv[1])) {
            apply_node_options(ctx, node, argv[1]);
        }
        handle = scene->engine.addNode(std::move(node));
    } else if (argc >= 2 && JS_IsObject(argv[1])) {
        if (td::Node* node = scene->engine.node(handle)) {
            apply_node_options(ctx, *node, argv[1]);
        }
    }
    return node_object(ctx, scene, handle);
}

JSValue scene_project(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    td::Vec3 world;
    if (argc < 1 || !read_vec3(ctx, argv[0], world)) {
        return JS_ThrowTypeError(ctx, "scene.project(world) expects [x, y, z]");
    }
    return screen_point_object(ctx, td::project(scene->engine.camera, world,
                                                static_cast<double>(scene->width),
                                                static_cast<double>(scene->height)));
}

JSValue scene_project_box(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || JS_IsArray(ctx, argv[0]) <= 0) {
        return JS_ThrowTypeError(ctx, "scene.projectBox(points) requires an array of world points");
    }
    JSValue out = JS_NewObject(ctx);
    JSValue points = JS_NewArray(ctx);
    uint32_t count = 0;
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    bool any = false;
    bool allOnScreen = true;
    uint32_t len = 0;
    JSValue lengthValue = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &len, lengthValue);
    JS_FreeValue(ctx, lengthValue);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[0], i);
        td::Vec3 world;
        const bool ok = read_vec3(ctx, item, world);
        JS_FreeValue(ctx, item);
        if (!ok) {
            continue;
        }
        const auto screen = td::project(scene->engine.camera, world,
                                        static_cast<double>(scene->width),
                                        static_cast<double>(scene->height));
        JS_SetPropertyUint32(ctx, points, count++, screen_point_object(ctx, screen));
        if (!any) {
            minX = maxX = screen.x;
            minY = maxY = screen.y;
            any = true;
        } else {
            minX = std::min(minX, screen.x);
            minY = std::min(minY, screen.y);
            maxX = std::max(maxX, screen.x);
            maxY = std::max(maxY, screen.y);
        }
        allOnScreen = allOnScreen && screen.onScreen;
    }
    JS_SetPropertyStr(ctx, out, "points", points);
    JS_SetPropertyStr(ctx, out, "x", JS_NewFloat64(ctx, minX));
    JS_SetPropertyStr(ctx, out, "y", JS_NewFloat64(ctx, minY));
    JS_SetPropertyStr(ctx, out, "width", JS_NewFloat64(ctx, any ? maxX - minX : 0.0));
    JS_SetPropertyStr(ctx, out, "height", JS_NewFloat64(ctx, any ? maxY - minY : 0.0));
    JS_SetPropertyStr(ctx, out, "onScreen", JS_NewBool(ctx, any && allOnScreen));
    return out;
}

JSValue scene_on_pick(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, scene->onPick);
    scene->onPick = (argc >= 1 && JS_IsFunction(ctx, argv[0])) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

JSValue scene_pick(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "scene.pick(px, py) requires two coordinates");
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
    const int64_t handle = scene->engine.pick(*ray);
    if (handle == 0) {
        return JS_NULL;
    }
    JSValue node = node_object(ctx, scene, handle);
    if (JS_IsFunction(ctx, scene->onPick)) {
        JSValue arg = JS_DupValue(ctx, node);
        JSValue ret = JS_Call(ctx, scene->onPick, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(ret)) {
            JS_FreeValue(ctx, node);
            return ret;
        }
        JS_FreeValue(ctx, ret);
    }
    return node;
}

void capture_timeline_start(td::Node& node, TimelineItem& item) {
    item.fromPosition = node.position;
    item.fromScale = node.scale;
    item.fromRotation = node.rotation;
    item.fromOpacity = node.material.a;
    item.fromColor = node.material;
    item.started = true;
}

void capture_timeline_camera_start(const td::Camera& camera, TimelineItem& item) {
    item.fromEye = camera.eye;
    item.fromTarget = camera.target;
    item.fromFovYRadians = camera.fovYRadians;
    item.fromNear = camera.near;
    item.fromFar = camera.far;
    item.started = true;
}

void capture_timeline_overlay_start(const td::Overlay& overlay, TimelineItem& item) {
    item.fromOverlayOffset = overlay.offset;
    item.started = true;
}

td::Vec3 lerp_vec3(td::Vec3 a, td::Vec3 b, double t) {
    return a + (b - a) * t;
}

double lerp_number(double a, double b, double t) {
    return a + (b - a) * t;
}

void apply_timeline_item(td::Node& node, TimelineItem& item, double t) {
    if (item.reverse) {
        t = 1.0 - t;
    }
    t = td::applyEase(item.ease, t);
    if (item.toPosition) node.position = lerp_vec3(item.fromPosition, *item.toPosition, t);
    if (item.toScale) node.scale = lerp_vec3(item.fromScale, *item.toScale, t);
    if (item.toRotation) node.rotation = lerp_vec3(item.fromRotation, *item.toRotation, t);
    if (item.toOpacity) node.material.a = lerp_number(item.fromOpacity, *item.toOpacity, t);
    if (item.toColor) {
        node.material.r = lerp_number(item.fromColor.r, item.toColor->r, t);
        node.material.g = lerp_number(item.fromColor.g, item.toColor->g, t);
        node.material.b = lerp_number(item.fromColor.b, item.toColor->b, t);
    }
}

void apply_timeline_camera(td::Camera& camera, TimelineItem& item, double t) {
    if (item.reverse) {
        t = 1.0 - t;
    }
    t = td::applyEase(item.ease, t);
    if (item.toEye) camera.eye = lerp_vec3(item.fromEye, *item.toEye, t);
    if (item.toTarget) camera.target = lerp_vec3(item.fromTarget, *item.toTarget, t);
    if (item.toFovYRadians) camera.fovYRadians = lerp_number(item.fromFovYRadians, *item.toFovYRadians, t);
    if (item.toNear) camera.near = lerp_number(item.fromNear, *item.toNear, t);
    if (item.toFar) camera.far = lerp_number(item.fromFar, *item.toFar, t);
}

void apply_timeline_overlay(td::Overlay& overlay, TimelineItem& item, double t) {
    if (item.reverse) {
        t = 1.0 - t;
    }
    t = td::applyEase(item.ease, t);
    if (item.toOverlayOffset) overlay.offset = lerp_vec3(item.fromOverlayOffset, *item.toOverlayOffset, t);
}

std::vector<int64_t> advance_timelines(SceneState& scene, double ms) {
    std::vector<int64_t> completed;
    for (const auto& timeline : scene.timelines) {
        if (!timeline || timeline->paused || timeline->canceled) {
            continue;
        }
        for (TimelineItem& item : timeline->items) {
            if (item.done) {
                continue;
            }
            td::Node* node = item.camera ? nullptr : scene.engine.node(item.node);
            td::Overlay* overlay = item.overlay != 0 ? scene.engine.overlay(item.overlay) : nullptr;
            if ((!item.camera && item.overlay == 0 && !node) || (item.overlay != 0 && !overlay)) {
                item.done = true;
                continue;
            }
            if (!item.started) {
                if (item.camera) {
                    capture_timeline_camera_start(scene.engine.camera, item);
                } else if (overlay) {
                    capture_timeline_overlay_start(*overlay, item);
                } else {
                    capture_timeline_start(*node, item);
                }
            }
            item.elapsedMs += ms;
            const double raw = item.durationMs > 0.0 ? std::min(1.0, item.elapsedMs / item.durationMs) : 1.0;
            if (item.camera) {
                apply_timeline_camera(scene.engine.camera, item, raw);
            } else if (overlay) {
                apply_timeline_overlay(*overlay, item, raw);
            } else {
                apply_timeline_item(*node, item, raw);
            }
            if (item.elapsedMs >= item.durationMs) {
                if (item.loop) {
                    item.elapsedMs = std::fmod(item.elapsedMs, std::max(1.0, item.durationMs));
                    if (item.yoyo) {
                        item.reverse = !item.reverse;
                    }
                } else {
                    item.done = true;
                    if (item.callbackId != 0) {
                        completed.push_back(item.callbackId);
                    }
                }
            }
        }
    }
    return completed;
}

JSValue scene_tick(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    double ms = 16.0;
    if (argc >= 1) {
        JS_ToFloat64(ctx, &ms, argv[0]);
    }
    auto timelineCompleted = advance_timelines(*scene, ms);
    auto completed = scene->engine.tick(ms);
    completed.insert(completed.end(), timelineCompleted.begin(), timelineCompleted.end());
    JSValue pending = JS_UNDEFINED;
    for (int64_t id : completed) {
        auto it = scene->tweenCallbacks.find(id);
        if (it == scene->tweenCallbacks.end()) {
            continue;
        }
        JSValue cb = it->second;
        scene->tweenCallbacks.erase(it);
        if (JS_IsUndefined(pending)) {
            JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(ret)) {
                pending = ret;
            } else {
                JS_FreeValue(ctx, ret);
            }
        }
        JS_FreeValue(ctx, cb);
    }
    if (JS_IsException(pending)) {
        return pending;
    }
    // Re-render the published frame so the viewport reflects the advanced scene.
    if (scene->frameRing.isOpen()) {
        std::lock_guard<std::mutex> lock(scene->frameRingMutex);
        (void)write_scene_frame(*scene);
    }
    return JS_NewInt64(ctx, static_cast<int64_t>(completed.size()));
}

JSValue scene_count(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    return JS_NewInt64(ctx, static_cast<int64_t>(scene->engine.nodes().size()));
}

JSValue overlay_state_to_js(JSContext* ctx, const td::OverlayState& state) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "handle", JS_NewInt64(ctx, state.handle));
    JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, state.id.c_str()));
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, state.label.c_str()));
    JS_SetPropertyStr(ctx, obj, "world", vec3_to_js(ctx, state.world));
    JS_SetPropertyStr(ctx, obj, "screen", screen_point_object(ctx, state.screen));
    JS_SetPropertyStr(ctx, obj, "visible", JS_NewBool(ctx, state.visible));
    JS_SetPropertyStr(ctx, obj, "leaderLine", JS_NewBool(ctx, state.leaderLine));
    return obj;
}

JSValue scene_overlay(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scene.overlay(anchor, options) requires a node or world point anchor");
    }
    td::Overlay overlay;
    if (auto* anchor = static_cast<NodeBox*>(JS_GetOpaque(argv[0], node_class_id))) {
        if (anchor->scene.get() != scene.get()) {
            return JS_ThrowTypeError(ctx, "scene.overlay(anchor) requires a node from the same scene");
        }
        overlay.anchorNode = anchor->handle;
    } else if (!read_vec3(ctx, argv[0], overlay.anchorWorld)) {
        return JS_ThrowTypeError(ctx, "scene.overlay(anchor) expects a node or [x, y, z]");
    }
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue id = JS_GetPropertyStr(ctx, argv[1], "id");
        if (JS_IsString(id)) overlay.id = js_string(ctx, id);
        JS_FreeValue(ctx, id);
        JSValue label = JS_GetPropertyStr(ctx, argv[1], "label");
        if (JS_IsString(label)) overlay.label = js_string(ctx, label);
        JS_FreeValue(ctx, label);
        JSValue offset = JS_GetPropertyStr(ctx, argv[1], "offset");
        if (!JS_IsUndefined(offset)) read_vec3(ctx, offset, overlay.offset);
        JS_FreeValue(ctx, offset);
        JSValue leader = JS_GetPropertyStr(ctx, argv[1], "leaderLine");
        if (JS_IsBool(leader)) overlay.leaderLine = JS_ToBool(ctx, leader) != 0;
        JS_FreeValue(ctx, leader);
    }
    const int64_t handle = scene->engine.addOverlay(std::move(overlay));
    auto state = scene->engine.overlayState(handle, static_cast<double>(scene->width), static_cast<double>(scene->height));
    return state ? overlay_state_to_js(ctx, *state) : JS_NULL;
}

JSValue scene_overlay_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scene.overlayState(handle) requires an overlay handle");
    }
    int64_t handle = 0;
    JS_ToInt64(ctx, &handle, argv[0]);
    auto state = scene->engine.overlayState(handle, static_cast<double>(scene->width), static_cast<double>(scene->height));
    return state ? overlay_state_to_js(ctx, *state) : JS_NULL;
}
