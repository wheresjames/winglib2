// ----- Node methods ---------------------------------------------------------

void apply_node_options(JSContext* ctx, td::Node& node, JSValueConst opts) {
    JSValue at = JS_GetPropertyStr(ctx, opts, "at");
    if (!JS_IsUndefined(at)) {
        read_vec3(ctx, at, node.position);
    }
    JS_FreeValue(ctx, at);
    JSValue position = JS_GetPropertyStr(ctx, opts, "position");
    if (!JS_IsUndefined(position)) {
        read_vec3(ctx, position, node.position);
    }
    JS_FreeValue(ctx, position);
    JSValue scale = JS_GetPropertyStr(ctx, opts, "scale");
    if (JS_IsNumber(scale)) {
        double s = 1.0;
        JS_ToFloat64(ctx, &s, scale);
        node.scale = {s, s, s};
    } else if (!JS_IsUndefined(scale)) {
        read_vec3(ctx, scale, node.scale);
    }
    JS_FreeValue(ctx, scale);
    JSValue rotation = JS_GetPropertyStr(ctx, opts, "rotation");
    if (!JS_IsUndefined(rotation)) {
        read_vec3(ctx, rotation, node.rotation);
    }
    JS_FreeValue(ctx, rotation);
    JSValue pivot = JS_GetPropertyStr(ctx, opts, "pivot");
    if (!JS_IsUndefined(pivot)) {
        read_vec3(ctx, pivot, node.pivot);
    }
    JS_FreeValue(ctx, pivot);
    JSValue color = JS_GetPropertyStr(ctx, opts, "color");
    if (!JS_IsUndefined(color)) {
        read_color(ctx, color, node.material);
    }
    JS_FreeValue(ctx, color);
    double opacity = node.material.a;
    if (read_number_prop(ctx, opts, "opacity", opacity)) {
        node.material.a = opacity;
    }
    const double beforeRadius = node.radius;
    read_number_prop(ctx, opts, "size", node.radius);
    read_number_prop(ctx, opts, "radius", node.radius);
    if (!node.mesh && node.radius != beforeRadius) {
        node.boundsMin = {-node.radius, -node.radius, -node.radius};
        node.boundsMax = {node.radius, node.radius, node.radius};
    }
    JSValue visible = JS_GetPropertyStr(ctx, opts, "visible");
    if (JS_IsBool(visible)) {
        node.visible = JS_ToBool(ctx, visible) != 0;
    }
    JS_FreeValue(ctx, visible);
    JSValue billboard = JS_GetPropertyStr(ctx, opts, "billboard");
    if (JS_IsBool(billboard)) {
        node.billboard = JS_ToBool(ctx, billboard) != 0;
    }
    JS_FreeValue(ctx, billboard);
    JSValue label = JS_GetPropertyStr(ctx, opts, "label");
    if (JS_IsString(label)) {
        node.label = js_string(ctx, label);
    }
    JS_FreeValue(ctx, label);
    JSValue model = JS_GetPropertyStr(ctx, opts, "model");
    if (JS_IsString(model)) {
        node.model = js_string(ctx, model);
    }
    JS_FreeValue(ctx, model);
}

td::Vec3 rotate_y(td::Vec3 v, double yaw) {
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

JSValue node_face_target(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "node.faceTarget(target) requires a target point");
    }
    td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_DupValue(ctx, thisVal);
    }
    td::Vec3 target;
    if (!read_vec3(ctx, argv[0], target)) {
        return JS_ThrowTypeError(ctx, "node.faceTarget(target) expects [x, y, z]");
    }
    const td::Vec3 world = box->scene->engine.worldPosition(box->handle);
    const td::Vec3 delta = target - world;
    node->rotation.y = std::atan2(delta.x, delta.z);
    return JS_DupValue(ctx, thisVal);
}

JSValue node_move_local(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "node.moveLocal(delta) requires a local delta");
    }
    td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_DupValue(ctx, thisVal);
    }
    td::Vec3 delta;
    if (!read_vec3(ctx, argv[0], delta)) {
        return JS_ThrowTypeError(ctx, "node.moveLocal(delta) expects [x, y, z]");
    }
    node->position = node->position + rotate_y(delta, node->rotation.y);
    return JS_DupValue(ctx, thisVal);
}

JSValue node_attach_to(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "node.attachTo(parent, options) requires a parent node or null");
    }
    int64_t parent = 0;
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        NodeBox* parentBox = get_node_box(ctx, argv[0]);
        if (!parentBox || parentBox->scene.get() != box->scene.get()) {
            return JS_ThrowTypeError(ctx, "node.attachTo(parent) requires a node from the same scene");
        }
        parent = parentBox->handle;
    }
    bool preserveWorld = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue preserve = JS_GetPropertyStr(ctx, argv[1], "preserveWorld");
        if (JS_IsBool(preserve)) {
            preserveWorld = JS_ToBool(ctx, preserve) != 0;
        }
        JS_FreeValue(ctx, preserve);
    }
    if (!box->scene->engine.setParent(box->handle, parent, preserveWorld)) {
        return throw_error(ctx, make_error("3d_invalid_argument", "Unable to attach node"));
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue node_detach(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    bool preserveWorld = true;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue preserve = JS_GetPropertyStr(ctx, argv[0], "preserveWorld");
        if (JS_IsBool(preserve)) {
            preserveWorld = JS_ToBool(ctx, preserve) != 0;
        }
        JS_FreeValue(ctx, preserve);
    }
    if (!box->scene->engine.setParent(box->handle, 0, preserveWorld)) {
        return throw_error(ctx, make_error("3d_invalid_argument", "Unable to detach node"));
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue node_bounds(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_NULL;
    }
    const td::Vec3 world = box->scene->engine.worldPosition(box->handle);
    const td::Vec3 mn{world.x + node->boundsMin.x * node->scale.x,
                      world.y + node->boundsMin.y * node->scale.y,
                      world.z + node->boundsMin.z * node->scale.z};
    const td::Vec3 mx{world.x + node->boundsMax.x * node->scale.x,
                      world.y + node->boundsMax.y * node->scale.y,
                      world.z + node->boundsMax.z * node->scale.z};
    const td::Vec3 center{(mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, (mn.z + mx.z) * 0.5};
    const double r = node->radius * std::max({node->scale.x, node->scale.y, node->scale.z});
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "center", vec3_to_js(ctx, center));
    JS_SetPropertyStr(ctx, obj, "min", vec3_to_js(ctx, mn));
    JS_SetPropertyStr(ctx, obj, "max", vec3_to_js(ctx, mx));
    JS_SetPropertyStr(ctx, obj, "radius", JS_NewFloat64(ctx, r));
    return obj;
}

JSValue node_matrix(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_NULL;
    }
    const td::Vec3 world = box->scene->engine.worldPosition(box->handle);
    JSValue arr = JS_NewArray(ctx);
    const double m[16] = {
        node->scale.x, 0, 0, 0,
        0, node->scale.y, 0, 0,
        0, 0, node->scale.z, 0,
        world.x, world.y, world.z, 1,
    };
    for (uint32_t i = 0; i < 16; ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, m[i]));
    }
    return arr;
}

JSValue node_set(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    td::Node* node = box->scene->engine.node(box->handle);
    if (node && argc >= 1 && JS_IsObject(argv[0])) {
        apply_node_options(ctx, *node, argv[0]);
        if (node->kind == "light") {
            if (td::Light* light = box->scene->engine.light(box->handle)) {
                light->position = node->position;
            }
        }
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue node_mesh(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const td::Node* node = box->scene->engine.node(box->handle);
    if (!node || !node->mesh) {
        return JS_NULL;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "vertexCount", JS_NewInt64(ctx, static_cast<int64_t>(node->mesh->vertices.size())));
    JS_SetPropertyStr(ctx, obj, "indexCount", JS_NewInt64(ctx, static_cast<int64_t>(node->mesh->indices.size())));
    JS_SetPropertyStr(ctx, obj, "triangleCount", JS_NewInt64(ctx, static_cast<int64_t>(node->mesh->indices.size() / 3)));
    JS_SetPropertyStr(ctx, obj, "dynamic", JS_NewBool(ctx, node->mesh->dynamic));
    JS_SetPropertyStr(ctx, obj, "hasNormals", JS_NewBool(ctx, !node->mesh->normals.empty()));
    JS_SetPropertyStr(ctx, obj, "hasUvs", JS_NewBool(ctx, !node->mesh->uvs.empty()));
    return obj;
}

JSValue node_update_mesh(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    td::Node* node = box->scene->engine.node(box->handle);
    if (!node || !node->mesh) {
        return throw_error(ctx, make_error("3d_invalid_argument", "node.updateMesh() requires a mesh node"));
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "node.updateMesh(options) requires an options object");
    }
    std::string error;
    if (!update_mesh_geometry(ctx, argv[0], *node->mesh, error)) {
        return throw_error(ctx, make_error("3d_invalid_mesh", error));
    }
    apply_node_options(ctx, *node, argv[0]);
    box->scene->engine.refreshBounds(*node);
    if (box->scene->frameRing.isOpen()) {
        std::lock_guard<std::mutex> lock(box->scene->frameRingMutex);
        auto written = write_scene_frame(*box->scene);
        if (!written) {
            return throw_error(ctx, written.error());
        }
    }
    return JS_DupValue(ctx, thisVal);
}

JSValue node_animate_to(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    SceneState& scene = *box->scene;
    if (!scene.engine.node(box->handle)) {
        return JS_DupValue(ctx, thisVal);
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "node.animateTo(options) requires an options object");
    }
    JSValueConst opts = argv[0];
    td::Tween tween;
    tween.node = box->handle;
    td::Vec3 v;
    JSValue position = JS_GetPropertyStr(ctx, opts, "position");
    if (JS_IsUndefined(position)) {
        JS_FreeValue(ctx, position);
        position = JS_GetPropertyStr(ctx, opts, "at");
    }
    if (!JS_IsUndefined(position) && read_vec3(ctx, position, v)) {
        tween.toPosition = v;
    }
    JS_FreeValue(ctx, position);
    JSValue scale = JS_GetPropertyStr(ctx, opts, "scale");
    if (JS_IsNumber(scale)) {
        double s = 1.0;
        JS_ToFloat64(ctx, &s, scale);
        tween.toScale = td::Vec3{s, s, s};
    } else if (!JS_IsUndefined(scale) && read_vec3(ctx, scale, v)) {
        tween.toScale = v;
    }
    JS_FreeValue(ctx, scale);
    JSValue rotation = JS_GetPropertyStr(ctx, opts, "rotation");
    if (!JS_IsUndefined(rotation) && read_vec3(ctx, rotation, v)) {
        tween.toRotation = v;
    }
    JS_FreeValue(ctx, rotation);
    double opacity = 0.0;
    if (read_number_prop(ctx, opts, "opacity", opacity)) {
        tween.toOpacity = opacity;
    }
    JSValue color = JS_GetPropertyStr(ctx, opts, "color");
    if (!JS_IsUndefined(color)) {
        td::Color c;
        if (read_color(ctx, color, c)) {
            tween.toColor = c;
        }
    }
    JS_FreeValue(ctx, color);
    read_number_prop(ctx, opts, "ms", tween.durationMs);
    JSValue ease = JS_GetPropertyStr(ctx, opts, "ease");
    if (JS_IsString(ease)) {
        if (auto parsed = td::parseEase(js_string(ctx, ease))) {
            tween.ease = *parsed;
        }
    }
    JS_FreeValue(ctx, ease);

    JSValueConst cb = JS_UNDEFINED;
    if (argc >= 2 && JS_IsFunction(ctx, argv[1])) {
        cb = argv[1];
    } else {
        JSValue onComplete = JS_GetPropertyStr(ctx, opts, "onComplete");
        if (JS_IsFunction(ctx, onComplete)) {
            const int64_t id = scene.nextCallbackId++;
            scene.tweenCallbacks[id] = onComplete;  // takes ownership
            tween.callbackId = id;
        } else {
            JS_FreeValue(ctx, onComplete);
        }
    }
    if (JS_IsFunction(ctx, cb)) {
        const int64_t id = scene.nextCallbackId++;
        scene.tweenCallbacks[id] = JS_DupValue(ctx, cb);
        tween.callbackId = id;
    }
    scene.engine.enqueueTween(tween);
    return JS_DupValue(ctx, thisVal);
}

JSValue node_attention(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_DupValue(ctx, thisVal);
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "node.attention(name) requires a behavior name");
    }
    const std::string name = js_string(ctx, argv[0]);
    td::Attention attention;
    attention.kind = attention_kind(name);
    if (argc >= 2 && JS_IsObject(argv[1])) {
        read_number_prop(ctx, argv[1], "hz", attention.hz);
        JSValue color = JS_GetPropertyStr(ctx, argv[1], "color");
        if (!JS_IsUndefined(color)) {
            attention.hasColor = read_color(ctx, color, attention.color);
        }
        JS_FreeValue(ctx, color);
    }
    node->attention = attention;
    return JS_DupValue(ctx, thisVal);
}

JSValue node_attention_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_NULL;
    }
    const td::Attention& a = node->attention;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, attention_name(a.kind)));
    JS_SetPropertyStr(ctx, obj, "hz", JS_NewFloat64(ctx, a.hz));
    JS_SetPropertyStr(ctx, obj, "phase", JS_NewFloat64(ctx, a.phase));
    JS_SetPropertyStr(ctx, obj, "intensity", JS_NewFloat64(ctx, a.intensity));
    JS_SetPropertyStr(ctx, obj, "spin", JS_NewFloat64(ctx, a.spin));
    JS_SetPropertyStr(ctx, obj, "lift", JS_NewFloat64(ctx, a.lift));
    JS_SetPropertyStr(ctx, obj, "ring", JS_NewFloat64(ctx, a.ring));
    return obj;
}

JSValue node_get(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    const td::Node* node = box->scene->engine.node(box->handle);
    if (!node) {
        return JS_NULL;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, node->kind.c_str()));
    JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, node->id.c_str()));
    JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, node->label.c_str()));
    JS_SetPropertyStr(ctx, obj, "model", JS_NewString(ctx, node->model.c_str()));
    JS_SetPropertyStr(ctx, obj, "assetUrl", JS_NewString(ctx, node->assetUrl.c_str()));
    JS_SetPropertyStr(ctx, obj, "resourceSize", JS_NewInt64(ctx, static_cast<int64_t>(node->resourceSize)));
    JS_SetPropertyStr(ctx, obj, "primitive", JS_NewString(ctx, node->primitive.c_str()));
    JSValue primitiveOptions = JS_NewObject(ctx);
    for (const auto& [key, value] : node->primitiveOptions) {
        JS_SetPropertyStr(ctx, primitiveOptions, key.c_str(), JS_NewFloat64(ctx, value));
    }
    JS_SetPropertyStr(ctx, obj, "primitiveOptions", primitiveOptions);
    if (node->mesh) {
        JSValue mesh = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mesh, "vertexCount", JS_NewInt64(ctx, static_cast<int64_t>(node->mesh->vertices.size())));
        JS_SetPropertyStr(ctx, mesh, "indexCount", JS_NewInt64(ctx, static_cast<int64_t>(node->mesh->indices.size())));
        JS_SetPropertyStr(ctx, mesh, "dynamic", JS_NewBool(ctx, node->mesh->dynamic));
        JS_SetPropertyStr(ctx, obj, "mesh", mesh);
    }
    JS_SetPropertyStr(ctx, obj, "position", vec3_to_js(ctx, node->position));
    JS_SetPropertyStr(ctx, obj, "world", vec3_to_js(ctx, box->scene->engine.worldPosition(box->handle)));
    JS_SetPropertyStr(ctx, obj, "pivot", vec3_to_js(ctx, node->pivot));
    JS_SetPropertyStr(ctx, obj, "scale", vec3_to_js(ctx, node->scale));
    JS_SetPropertyStr(ctx, obj, "rotation", vec3_to_js(ctx, node->rotation));
    JS_SetPropertyStr(ctx, obj, "parent", JS_NewInt64(ctx, node->parent));
    JS_SetPropertyStr(ctx, obj, "visible", JS_NewBool(ctx, node->visible));
    JS_SetPropertyStr(ctx, obj, "opacity", JS_NewFloat64(ctx, node->material.a));
    JSValue color = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, color, 0, JS_NewFloat64(ctx, node->material.r));
    JS_SetPropertyUint32(ctx, color, 1, JS_NewFloat64(ctx, node->material.g));
    JS_SetPropertyUint32(ctx, color, 2, JS_NewFloat64(ctx, node->material.b));
    JS_SetPropertyUint32(ctx, color, 3, JS_NewFloat64(ctx, node->material.a));
    JS_SetPropertyStr(ctx, obj, "color", color);
    return obj;
}

JSValue node_remove(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    NodeBox* box = get_node_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    box->scene->engine.removeNode(box->handle);
    return JS_UNDEFINED;
}
