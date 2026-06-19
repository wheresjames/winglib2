struct SceneState;
void stop_render_thread(SceneState& scene);
struct TimelineState;

struct CameraVideoState {
    wl2::VideoBuffer reader;
    std::string name;
    std::string mode = "filmPlane";
    int64_t width = 0;
    int64_t height = 0;
    int64_t sequence = -1;
};

struct SceneState {
    int64_t width = 1280;
    int64_t height = 720;
    int64_t buffers = 3;
    wl2::VideoBuffer frameRing;
    std::string frameRingName;
    int64_t resizeGeneration = 0;
    std::mutex frameRingMutex;
    std::atomic<bool> renderThreadRunning{false};
    std::thread renderThread;

    // GPU rendering (Magnum): authorized once at publishTo via authorizeGraphics;
    // the per-scene renderer is created lazily in write_scene_frame and resized
    // when the frame size changes. Absent/ignored when the renderer is compiled
    // out or no GL context is available — write_scene_frame falls back to CPU.
    bool graphicsAuthorized = false;
    bool gpuActive = false;  // true once a frame has actually been GPU-rendered
#if WL2_3D_HAVE_MAGNUM
    std::unique_ptr<td::MagnumRenderer> gpuRenderer;
#endif

    // Engine core (renderer-independent): scene graph, camera, animators.
    td::Engine engine;
    std::map<int64_t, JSValue> tweenCallbacks;
    int64_t nextCallbackId = 1;
    JSValue onPick = JS_UNDEFINED;
    JSContext* ctx = nullptr;

    // Detections->scene consumer (§14.2): a memmsg reader feeding upserts.
    std::optional<wl2::SharedQueue> detectionQueue;
    std::optional<CameraVideoState> cameraVideo;
    std::vector<std::shared_ptr<TimelineState>> timelines;

    ~SceneState() {
        stop_render_thread(*this);
    }
};

struct SceneBox {
    std::shared_ptr<SceneState> scene;
};

void scene_finalizer(JSRuntime* rt, JSValue val) {
    auto* box = static_cast<SceneBox*>(JS_GetOpaque(val, scene_class_id));
    if (box) {
        SceneState& scene = *box->scene;
        for (auto& entry : scene.tweenCallbacks) {
            JS_FreeValueRT(rt, entry.second);
        }
        scene.tweenCallbacks.clear();
        JS_FreeValueRT(rt, scene.onPick);
        scene.onPick = JS_UNDEFINED;
    }
    delete box;
}

std::shared_ptr<SceneState> get_scene(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<SceneBox*>(JS_GetOpaque2(ctx, value, scene_class_id));
    return box ? box->scene : nullptr;
}

std::string js_string(JSContext* ctx, JSValueConst value) {
    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, value);
    if (!text) {
        return {};
    }
    std::string out(text, len);
    JS_FreeCString(ctx, text);
    return out;
}

wl2::Error make_error(std::string code, std::string message) {
    return wl2::Error(std::move(code), std::move(message));
}

JSValue js_error(JSContext* ctx, const wl2::Error& error) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "ThreeDError"));
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, error.code().c_str()));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, error.message().c_str()));
    return err;
}

JSValue throw_error(JSContext* ctx, const wl2::Error& error) {
    return JS_Throw(ctx, js_error(ctx, error));
}

JSValue rejected_promise(JSContext* ctx, const wl2::Error& error) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        return promise;
    }
    JSValue err = js_error(ctx, error);
    JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

JSValue resolved_promise(JSContext* ctx, JSValue value) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

JSValue scene_object(JSContext* ctx, std::shared_ptr<SceneState> scene) {
    auto* box = new SceneBox{std::move(scene)};
    JSValue obj = JS_NewObjectClass(ctx, scene_class_id);
    if (JS_IsException(obj)) {
        delete box;
        return obj;
    }
    JS_SetOpaque(obj, box);
    return obj;
}

bool read_size(JSContext* ctx, JSValueConst options, int64_t& width, int64_t& height) {
    if (!JS_IsObject(options)) {
        return true;
    }
    JSValue size = JS_GetPropertyStr(ctx, options, "size");
    if (JS_IsUndefined(size) || JS_IsNull(size)) {
        JS_FreeValue(ctx, size);
        return true;
    }
    JSValue w = JS_GetPropertyUint32(ctx, size, 0);
    JSValue h = JS_GetPropertyUint32(ctx, size, 1);
    int64_t parsedWidth = width;
    int64_t parsedHeight = height;
    const bool ok = JS_ToInt64(ctx, &parsedWidth, w) == 0 && JS_ToInt64(ctx, &parsedHeight, h) == 0;
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, size);
    if (!ok || parsedWidth <= 0 || parsedHeight <= 0 || parsedWidth > 16384 || parsedHeight > 16384) {
        return false;
    }
    width = parsedWidth;
    height = parsedHeight;
    return true;
}

JSValue metadata(JSContext* ctx, SceneState& scene) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt64(ctx, scene.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt64(ctx, scene.height));
    JS_SetPropertyStr(ctx, obj, "buffers", JS_NewInt64(ctx, scene.buffers));
    JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, "rgba8"));
    JS_SetPropertyStr(ctx, obj, "colorSpace", JS_NewString(ctx, "srgb"));
    JS_SetPropertyStr(ctx, obj, "alpha", JS_NewString(ctx, "premultiplied"));
    JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, "top-left"));
    JS_SetPropertyStr(ctx, obj, "renderer", JS_NewString(ctx, WL2_3D_HAVE_MAGNUM ? "magnum" : "synthetic"));
    JS_SetPropertyStr(ctx, obj, "gpuActive", JS_NewBool(ctx, scene.gpuActive));
    if (scene.frameRing.isOpen()) {
        JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, scene.frameRing.sequence()));
        auto frame = scene.frameRing.frame(0);
        JS_SetPropertyStr(ctx, obj, "stride", JS_NewInt64(ctx, frame ? frame.value().scanWidth : 0));
    }
    return obj;
}

wl2::Result<void> write_scene_frame(SceneState& scene) {
    auto frame = scene.frameRing.frame(0);
    if (!frame) {
        return frame.error();
    }
    if (frame.value().scanWidth < scene.width * 4 || frame.value().size < static_cast<size_t>(frame.value().scanWidth * scene.height)) {
        return make_error("3d_invalid_frame", "FrameRing returned invalid RGBA stride or size");
    }
    // Render the live engine model into the frame. The GPU view (Magnum) is used
    // when graphics is authorized and a GL context exists; otherwise the CPU view
    // draws the same scene graph (camera, grid, shaded nodes, particles). Both are
    // views over the same engine model and honor the shared pixel contract, so the
    // ring bookkeeping (frame(0)/size checks/next(1)) below is identical for each.
    auto* const data = reinterpret_cast<unsigned char*>(frame.value().data);
    const int width = static_cast<int>(scene.width);
    const int height = static_cast<int>(scene.height);
    const int stride = static_cast<int>(frame.value().scanWidth);
    bool rendered = false;
#if WL2_3D_HAVE_MAGNUM
    if (scene.graphicsAuthorized && td::gpu_context_available()) {
        try {
            if (!scene.gpuRenderer || scene.gpuRenderer->width() != width ||
                scene.gpuRenderer->height() != height) {
                scene.gpuRenderer = std::make_unique<td::MagnumRenderer>(width, height);
            }
            scene.gpuRenderer->render(scene.engine);
            scene.gpuRenderer->readInto(data, stride);
            rendered = true;
        } catch (const std::exception& e) {
            td::gpu_log_fallback_once(e.what());
            scene.gpuRenderer.reset();
        } catch (...) {
            td::gpu_log_fallback_once("unknown GPU render error");
            scene.gpuRenderer.reset();
        }
    }
#endif
    if (!rendered) {
        render_scene_cpu(scene.engine, data, width, height, stride);
    }
    scene.gpuActive = rendered;
    scene.frameRing.next(1);
    return {};
}

void stop_render_thread(SceneState& scene) {
    scene.renderThreadRunning.store(false);
    if (scene.renderThread.joinable()) {
        scene.renderThread.join();
    }
}

void start_render_thread(const std::shared_ptr<SceneState>& scene) {
    // The CPU renderer reads the engine model, which JavaScript mutates on its
    // own thread (add nodes, tick animations). To stay race-free without locking
    // every engine mutation, rendering is driven from the JS thread instead of a
    // background thread: publishTo() renders the first frame and scene.tick()
    // re-renders as the scene animates. (A future GPU renderer that owns its own
    // GL context would reintroduce a worker thread with marshalled hand-off.)
    (void)scene;
}

// ----- Engine-core bindings: camera, ray, node, scene-graph ----------------

struct CameraBox {
    std::shared_ptr<SceneState> scene;
};
struct RayBox {
    std::shared_ptr<SceneState> scene;
    td::Ray ray;
};
struct NodeBox {
    std::shared_ptr<SceneState> scene;
    int64_t handle = 0;
};
struct TextureState {
    int64_t width = 0;
    int64_t height = 0;
    int64_t stride = 0;
    std::vector<uint8_t> rgba;
};
struct TextureBox {
    std::shared_ptr<TextureState> texture;
};
struct TimelineItem {
    int64_t node = 0;
    int64_t overlay = 0;
    bool camera = false;
    td::Ease ease = td::Ease::OutCubic;
    double durationMs = 200.0;
    double elapsedMs = 0.0;
    bool loop = false;
    bool yoyo = false;
    bool reverse = false;
    bool started = false;
    bool done = false;
    std::optional<td::Vec3> toPosition;
    std::optional<td::Vec3> toScale;
    std::optional<td::Vec3> toRotation;
    std::optional<double> toOpacity;
    std::optional<td::Color> toColor;
    std::optional<td::Vec3> toEye;
    std::optional<td::Vec3> toTarget;
    std::optional<double> toFovYRadians;
    std::optional<double> toNear;
    std::optional<double> toFar;
    std::optional<td::Vec3> toOverlayOffset;
    td::Vec3 fromPosition;
    td::Vec3 fromScale;
    td::Vec3 fromRotation;
    double fromOpacity = 1.0;
    td::Color fromColor;
    td::Vec3 fromEye;
    td::Vec3 fromTarget;
    double fromFovYRadians = 0.0;
    double fromNear = 0.0;
    double fromFar = 0.0;
    td::Vec3 fromOverlayOffset;
    int64_t callbackId = 0;
};
struct TimelineState {
    std::weak_ptr<SceneState> scene;
    std::string name;
    std::vector<TimelineItem> items;
    bool paused = false;
    bool canceled = false;
};
struct TimelineBox {
    std::shared_ptr<TimelineState> timeline;
};

void camera_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<CameraBox*>(JS_GetOpaque(val, camera_class_id));
}
void ray_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<RayBox*>(JS_GetOpaque(val, ray_class_id));
}
void node_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NodeBox*>(JS_GetOpaque(val, node_class_id));
}
void texture_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<TextureBox*>(JS_GetOpaque(val, texture_class_id));
}
void timeline_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<TimelineBox*>(JS_GetOpaque(val, timeline_class_id));
}

CameraBox* get_camera(JSContext* ctx, JSValueConst value) {
    return static_cast<CameraBox*>(JS_GetOpaque2(ctx, value, camera_class_id));
}
RayBox* get_ray(JSContext* ctx, JSValueConst value) {
    return static_cast<RayBox*>(JS_GetOpaque2(ctx, value, ray_class_id));
}
NodeBox* get_node_box(JSContext* ctx, JSValueConst value) {
    return static_cast<NodeBox*>(JS_GetOpaque2(ctx, value, node_class_id));
}
TextureBox* get_texture_box(JSContext* ctx, JSValueConst value) {
    return static_cast<TextureBox*>(JS_GetOpaque2(ctx, value, texture_class_id));
}
TimelineBox* get_timeline_box(JSContext* ctx, JSValueConst value) {
    return static_cast<TimelineBox*>(JS_GetOpaque2(ctx, value, timeline_class_id));
}

bool read_vec3(JSContext* ctx, JSValueConst value, td::Vec3& out) {
    if (JS_IsArray(ctx, value) > 0) {
        JSValue x = JS_GetPropertyUint32(ctx, value, 0);
        JSValue y = JS_GetPropertyUint32(ctx, value, 1);
        JSValue z = JS_GetPropertyUint32(ctx, value, 2);
        double dx = out.x, dy = out.y, dz = out.z;
        const bool ok = JS_ToFloat64(ctx, &dx, x) == 0 && JS_ToFloat64(ctx, &dy, y) == 0 &&
                        JS_ToFloat64(ctx, &dz, z) == 0;
        JS_FreeValue(ctx, x);
        JS_FreeValue(ctx, y);
        JS_FreeValue(ctx, z);
        if (ok) {
            out = {dx, dy, dz};
        }
        return ok;
    }
    if (JS_IsObject(value)) {
        td::Vec3 parsed = out;
        JSValue x = JS_GetPropertyStr(ctx, value, "x");
        JSValue y = JS_GetPropertyStr(ctx, value, "y");
        JSValue z = JS_GetPropertyStr(ctx, value, "z");
        const bool ok = JS_ToFloat64(ctx, &parsed.x, x) == 0 && JS_ToFloat64(ctx, &parsed.y, y) == 0 &&
                        JS_ToFloat64(ctx, &parsed.z, z) == 0;
        JS_FreeValue(ctx, x);
        JS_FreeValue(ctx, y);
        JS_FreeValue(ctx, z);
        if (ok) {
            out = parsed;
        }
        return ok;
    }
    return false;
}

JSValue vec3_to_js(JSContext* ctx, td::Vec3 v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, v.z));
    return arr;
}

bool read_number_prop(JSContext* ctx, JSValueConst obj, const char* key, double& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return false;
    }
    double parsed = out;
    const bool ok = JS_ToFloat64(ctx, &parsed, v) == 0;
    JS_FreeValue(ctx, v);
    if (ok) {
        out = parsed;
    }
    return ok;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Parse a color from "#rrggbb", [r,g,b(,a)] (0..1), or {r,g,b,a}. Alpha is left
// untouched when the source has none.
bool read_color(JSContext* ctx, JSValueConst value, td::Color& out) {
    if (JS_IsString(value)) {
        std::string text = js_string(ctx, value);
        size_t i = (!text.empty() && text[0] == '#') ? 1 : 0;
        if (text.size() - i >= 6) {
            int r1 = hex_digit(text[i]), r2 = hex_digit(text[i + 1]);
            int g1 = hex_digit(text[i + 2]), g2 = hex_digit(text[i + 3]);
            int b1 = hex_digit(text[i + 4]), b2 = hex_digit(text[i + 5]);
            if ((r1 | r2 | g1 | g2 | b1 | b2) >= 0) {
                out.r = (r1 * 16 + r2) / 255.0;
                out.g = (g1 * 16 + g2) / 255.0;
                out.b = (b1 * 16 + b2) / 255.0;
                return true;
            }
        }
        return false;
    }
    if (JS_IsArray(ctx, value) > 0) {
        td::Vec3 rgb{out.r, out.g, out.b};
        if (read_vec3(ctx, value, rgb)) {
            out.r = rgb.x;
            out.g = rgb.y;
            out.b = rgb.z;
            return true;
        }
        return false;
    }
    if (JS_IsObject(value)) {
        read_number_prop(ctx, value, "r", out.r);
        read_number_prop(ctx, value, "g", out.g);
        read_number_prop(ctx, value, "b", out.b);
        read_number_prop(ctx, value, "a", out.a);
        return true;
    }
    return false;
}

constexpr size_t kMaxMeshVertices = 1'000'000;
constexpr size_t kMaxMeshIndices = 3'000'000;

bool read_array_length(JSContext* ctx, JSValueConst value, uint32_t& len) {
    JSValue lengthValue = JS_GetPropertyStr(ctx, value, "length");
    const bool ok = JS_ToUint32(ctx, &len, lengthValue) == 0;
    JS_FreeValue(ctx, lengthValue);
    return ok;
}

bool read_numeric_array(JSContext* ctx, JSValueConst value, std::vector<double>& out) {
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buffer)) {
        size_t size = 0;
        uint8_t* data = JS_GetArrayBuffer(ctx, &size, buffer);
        const bool ok = data && byteOffset <= size && byteLength <= size - byteOffset &&
                        (bytesPerElement == 4 || bytesPerElement == 8) &&
                        byteLength % bytesPerElement == 0;
        if (ok) {
            const uint8_t* p = data + byteOffset;
            const size_t count = byteLength / bytesPerElement;
            out.resize(count);
            for (size_t i = 0; i < count; ++i) {
                if (bytesPerElement == 4) {
                    float f = 0.0f;
                    std::memcpy(&f, p + i * 4, 4);
                    out[i] = static_cast<double>(f);
                } else {
                    double d = 0.0;
                    std::memcpy(&d, p + i * 8, 8);
                    out[i] = d;
                }
                if (!std::isfinite(out[i])) {
                    JS_FreeValue(ctx, buffer);
                    return false;
                }
            }
        }
        JS_FreeValue(ctx, buffer);
        return ok;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));

    if (JS_IsArray(ctx, value) <= 0) {
        return false;
    }
    uint32_t len = 0;
    if (!read_array_length(ctx, value, len)) {
        return false;
    }
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, value, i);
        double n = 0.0;
        const bool ok = JS_ToFloat64(ctx, &n, item) == 0 && std::isfinite(n);
        JS_FreeValue(ctx, item);
        if (!ok) {
            return false;
        }
        out.push_back(n);
    }
    return true;
}

bool read_index_array(JSContext* ctx, JSValueConst value, std::vector<uint32_t>& out) {
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buffer)) {
        size_t size = 0;
        uint8_t* data = JS_GetArrayBuffer(ctx, &size, buffer);
        const bool ok = data && byteOffset <= size && byteLength <= size - byteOffset &&
                        (bytesPerElement == 2 || bytesPerElement == 4) &&
                        byteLength % bytesPerElement == 0;
        if (ok) {
            const uint8_t* p = data + byteOffset;
            const size_t count = byteLength / bytesPerElement;
            out.resize(count);
            for (size_t i = 0; i < count; ++i) {
                if (bytesPerElement == 2) {
                    uint16_t v = 0;
                    std::memcpy(&v, p + i * 2, 2);
                    out[i] = v;
                } else {
                    uint32_t v = 0;
                    std::memcpy(&v, p + i * 4, 4);
                    out[i] = v;
                }
            }
        }
        JS_FreeValue(ctx, buffer);
        return ok;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));

    if (JS_IsArray(ctx, value) <= 0) {
        return false;
    }
    uint32_t len = 0;
    if (!read_array_length(ctx, value, len)) {
        return false;
    }
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, value, i);
        int64_t n = 0;
        const bool ok = JS_ToInt64(ctx, &n, item) == 0 && n >= 0 &&
                        n <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
        JS_FreeValue(ctx, item);
        if (!ok) {
            return false;
        }
        out.push_back(static_cast<uint32_t>(n));
    }
    return true;
}

bool read_mesh_geometry(JSContext* ctx, JSValueConst opts, bool requireAll, td::MeshGeometry& out,
                        std::string& error) {
    td::MeshGeometry mesh = out;
    JSValue vertices = JS_GetPropertyStr(ctx, opts, "vertices");
    if (!JS_IsUndefined(vertices)) {
        std::vector<double> raw;
        if (!read_numeric_array(ctx, vertices, raw) || raw.size() % 3 != 0 || raw.empty()) {
            JS_FreeValue(ctx, vertices);
            error = "mesh vertices must be a non-empty numeric array or Float32Array with x/y/z triples";
            return false;
        }
        if (raw.size() / 3 > kMaxMeshVertices) {
            JS_FreeValue(ctx, vertices);
            error = "mesh vertex count exceeds the configured limit";
            return false;
        }
        mesh.vertices.clear();
        mesh.vertices.reserve(raw.size() / 3);
        for (size_t i = 0; i < raw.size(); i += 3) {
            mesh.vertices.push_back({raw[i], raw[i + 1], raw[i + 2]});
        }
    } else if (requireAll) {
        JS_FreeValue(ctx, vertices);
        error = "scene.mesh() requires vertices";
        return false;
    }
    JS_FreeValue(ctx, vertices);

    JSValue indices = JS_GetPropertyStr(ctx, opts, "indices");
    if (!JS_IsUndefined(indices)) {
        if (!read_index_array(ctx, indices, mesh.indices) || mesh.indices.size() % 3 != 0 || mesh.indices.empty()) {
            JS_FreeValue(ctx, indices);
            error = "mesh indices must be a non-empty integer array, Uint16Array, or Uint32Array of triangles";
            return false;
        }
        if (mesh.indices.size() > kMaxMeshIndices) {
            JS_FreeValue(ctx, indices);
            error = "mesh index count exceeds the configured limit";
            return false;
        }
    } else if (requireAll) {
        JS_FreeValue(ctx, indices);
        error = "scene.mesh() requires indices";
        return false;
    }
    JS_FreeValue(ctx, indices);

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "mesh updates must leave vertices and indices populated";
        return false;
    }
    for (uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            error = "mesh index references a missing vertex";
            return false;
        }
    }

    JSValue normals = JS_GetPropertyStr(ctx, opts, "normals");
    if (!JS_IsUndefined(normals)) {
        std::vector<double> raw;
        if (!read_numeric_array(ctx, normals, raw) || raw.size() != mesh.vertices.size() * 3) {
            JS_FreeValue(ctx, normals);
            error = "mesh normals must match the vertex count";
            return false;
        }
        mesh.normals.clear();
        mesh.normals.reserve(mesh.vertices.size());
        for (size_t i = 0; i < raw.size(); i += 3) {
            mesh.normals.push_back({raw[i], raw[i + 1], raw[i + 2]});
        }
    }
    JS_FreeValue(ctx, normals);

    // No author-supplied normals: derive smooth normals so the renderer shades
    // the surface per pixel instead of faceting each triangle.
    if (mesh.normals.size() != mesh.vertices.size()) {
        td::recomputeSmoothNormals(mesh);
    }

    JSValue uvs = JS_GetPropertyStr(ctx, opts, "uvs");
    if (!JS_IsUndefined(uvs)) {
        if (!read_numeric_array(ctx, uvs, mesh.uvs) || mesh.uvs.size() != mesh.vertices.size() * 2) {
            JS_FreeValue(ctx, uvs);
            error = "mesh uvs must match the vertex count";
            return false;
        }
    }
    JS_FreeValue(ctx, uvs);

    JSValue dynamic = JS_GetPropertyStr(ctx, opts, "dynamic");
    if (JS_IsBool(dynamic)) {
        mesh.dynamic = JS_ToBool(ctx, dynamic) != 0;
    }
    JS_FreeValue(ctx, dynamic);

    out = std::move(mesh);
    return true;
}

void assign_mesh_vertices(std::vector<td::Vec3>& vertices, const std::vector<double>& raw) {
    vertices.resize(raw.size() / 3);
    for (size_t i = 0, out = 0; i < raw.size(); i += 3, ++out) {
        vertices[out] = {raw[i], raw[i + 1], raw[i + 2]};
    }
}

void assign_mesh_normals(std::vector<td::Vec3>& normals, const std::vector<double>& raw) {
    normals.resize(raw.size() / 3);
    for (size_t i = 0, out = 0; i < raw.size(); i += 3, ++out) {
        normals[out] = {raw[i], raw[i + 1], raw[i + 2]};
    }
}

bool update_mesh_geometry(JSContext* ctx, JSValueConst opts, td::MeshGeometry& mesh, std::string& error) {
    std::optional<std::vector<double>> rawVertices;
    std::optional<std::vector<uint32_t>> indices;
    std::optional<std::vector<double>> rawNormals;
    std::optional<std::vector<double>> uvs;
    std::optional<bool> dynamic;

    JSValue vertices = JS_GetPropertyStr(ctx, opts, "vertices");
    if (!JS_IsUndefined(vertices)) {
        std::vector<double> raw;
        if (!read_numeric_array(ctx, vertices, raw) || raw.size() % 3 != 0 || raw.empty()) {
            JS_FreeValue(ctx, vertices);
            error = "mesh vertices must be a non-empty numeric array or Float32Array with x/y/z triples";
            return false;
        }
        if (raw.size() / 3 > kMaxMeshVertices) {
            JS_FreeValue(ctx, vertices);
            error = "mesh vertex count exceeds the configured limit";
            return false;
        }
        rawVertices = std::move(raw);
    }
    JS_FreeValue(ctx, vertices);

    JSValue indexValue = JS_GetPropertyStr(ctx, opts, "indices");
    if (!JS_IsUndefined(indexValue)) {
        std::vector<uint32_t> parsed;
        if (!read_index_array(ctx, indexValue, parsed) || parsed.size() % 3 != 0 || parsed.empty()) {
            JS_FreeValue(ctx, indexValue);
            error = "mesh indices must be a non-empty integer array, Uint16Array, or Uint32Array of triangles";
            return false;
        }
        if (parsed.size() > kMaxMeshIndices) {
            JS_FreeValue(ctx, indexValue);
            error = "mesh index count exceeds the configured limit";
            return false;
        }
        indices = std::move(parsed);
    }
    JS_FreeValue(ctx, indexValue);

    const size_t vertexCount = rawVertices ? rawVertices->size() / 3 : mesh.vertices.size();
    const auto& activeIndices = indices ? *indices : mesh.indices;
    if (vertexCount == 0 || activeIndices.empty()) {
        error = "mesh updates must leave vertices and indices populated";
        return false;
    }
    for (uint32_t index : activeIndices) {
        if (index >= vertexCount) {
            error = "mesh index references a missing vertex";
            return false;
        }
    }

    JSValue normals = JS_GetPropertyStr(ctx, opts, "normals");
    if (!JS_IsUndefined(normals)) {
        std::vector<double> raw;
        if (!read_numeric_array(ctx, normals, raw) || raw.size() != vertexCount * 3) {
            JS_FreeValue(ctx, normals);
            error = "mesh normals must match the vertex count";
            return false;
        }
        rawNormals = std::move(raw);
    }
    JS_FreeValue(ctx, normals);

    JSValue uvValue = JS_GetPropertyStr(ctx, opts, "uvs");
    if (!JS_IsUndefined(uvValue)) {
        std::vector<double> parsed;
        if (!read_numeric_array(ctx, uvValue, parsed) || parsed.size() != vertexCount * 2) {
            JS_FreeValue(ctx, uvValue);
            error = "mesh uvs must match the vertex count";
            return false;
        }
        uvs = std::move(parsed);
    }
    JS_FreeValue(ctx, uvValue);

    JSValue dynamicValue = JS_GetPropertyStr(ctx, opts, "dynamic");
    if (JS_IsBool(dynamicValue)) {
        dynamic = JS_ToBool(ctx, dynamicValue) != 0;
    }
    JS_FreeValue(ctx, dynamicValue);

    if (rawVertices) {
        assign_mesh_vertices(mesh.vertices, *rawVertices);
    }
    if (indices) {
        mesh.indices = std::move(*indices);
    }
    if (rawNormals) {
        assign_mesh_normals(mesh.normals, *rawNormals);
    } else if (rawVertices || indices || mesh.normals.size() != vertexCount) {
        // Geometry moved (or normals were never authored): regenerate smooth
        // normals from the current vertices so a morphing surface stays shaded
        // smoothly without the author resupplying normals each frame.
        td::recomputeSmoothNormals(mesh);
    }
    if (uvs) {
        mesh.uvs = std::move(*uvs);
    } else if (mesh.uvs.size() != vertexCount * 2) {
        mesh.uvs.clear();
    }
    if (dynamic) {
        mesh.dynamic = *dynamic;
    }
    return true;
}

const char* attention_name(td::AttentionKind kind) {
    switch (kind) {
        case td::AttentionKind::Rotate: return "rotate";
        case td::AttentionKind::Pulse: return "pulse";
        case td::AttentionKind::Glow: return "glow";
        case td::AttentionKind::Ping: return "ping";
        case td::AttentionKind::Bounce: return "bounce";
        case td::AttentionKind::None: break;
    }
    return "none";
}

td::AttentionKind attention_kind(const std::string& name) {
    if (name == "rotate") return td::AttentionKind::Rotate;
    if (name == "pulse") return td::AttentionKind::Pulse;
    if (name == "glow") return td::AttentionKind::Glow;
    if (name == "ping") return td::AttentionKind::Ping;
    if (name == "bounce") return td::AttentionKind::Bounce;
    return td::AttentionKind::None;
}

// Read a plane from {normal, point}, {y}, or default to the ground (y = 0).
td::Plane read_plane(JSContext* ctx, JSValueConst value) {
    td::Plane plane;
    if (!JS_IsObject(value)) {
        return plane;
    }
    JSValue normal = JS_GetPropertyStr(ctx, value, "normal");
    if (!JS_IsUndefined(normal)) {
        read_vec3(ctx, normal, plane.normal);
    }
    JS_FreeValue(ctx, normal);
    JSValue point = JS_GetPropertyStr(ctx, value, "point");
    if (!JS_IsUndefined(point)) {
        read_vec3(ctx, point, plane.point);
    }
    JS_FreeValue(ctx, point);
    double y = 0.0;
    if (read_number_prop(ctx, value, "y", y)) {
        plane.point = {0.0, y, 0.0};
    }
    return plane;
}

JSValue make_plane_object(JSContext* ctx, td::Plane plane) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "normal", vec3_to_js(ctx, plane.normal));
    JS_SetPropertyStr(ctx, obj, "point", vec3_to_js(ctx, plane.point));
    return obj;
}

JSValue node_object(JSContext* ctx, std::shared_ptr<SceneState> scene, int64_t handle) {
    JSValue obj = JS_NewObjectClass(ctx, node_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    auto* box = new NodeBox{std::move(scene), handle};
    JS_SetOpaque(obj, box);
    if (const td::Node* n = box->scene->engine.node(handle)) {
        JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, n->id.c_str()));
    }
    return obj;
}

JSValue ray_object(JSContext* ctx, std::shared_ptr<SceneState> scene, td::Ray ray) {
    JSValue obj = JS_NewObjectClass(ctx, ray_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new RayBox{std::move(scene), ray});
    JS_SetPropertyStr(ctx, obj, "origin", vec3_to_js(ctx, ray.origin));
    JS_SetPropertyStr(ctx, obj, "direction", vec3_to_js(ctx, ray.direction));
    return obj;
}

JSValue camera_object(JSContext* ctx, std::shared_ptr<SceneState> scene) {
    JSValue obj = JS_NewObjectClass(ctx, camera_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new CameraBox{std::move(scene)});
    return obj;
}

JSValue screen_point_object(JSContext* ctx, td::ScreenPoint screen) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, screen.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, screen.y));
    JS_SetPropertyStr(ctx, obj, "depth", JS_NewFloat64(ctx, screen.depth));
    JS_SetPropertyStr(ctx, obj, "onScreen", JS_NewBool(ctx, screen.onScreen));
    return obj;
}
