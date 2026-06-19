#include "wl2_3d/wl2_3d.h"

#include "wl2_3d_detection.h"
#include "wl2_3d_engine.h"
#include "wl2_3d_gpu_context.h"  // no-op unless WL2_3D_HAVE_MAGNUM
#include "wl2_3d_gpu_render.h"   // no-op unless WL2_3D_HAVE_MAGNUM
#include "wl2_3d_math.h"
#include "wl2_3d_render.h"

#include "wl2/membus.h"
#include "wl2/resources.h"
#include "wl2/runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif
#ifndef WL2_3D_HAVE_MAGNUM
#define WL2_3D_HAVE_MAGNUM 0
#endif

namespace {

constexpr const char* ThreeDApi = R"(Exports JavaScript module wl2:3d.

Scene lifecycle / frame bridge:
  Scene.create({ size: [width, height] }) -> Promise<Scene>
  scene.publishTo(name) -> metadata   (requires the shared-memory capability)
  scene.metadata() -> metadata
  scene.close()

Calibrated camera and 2D<->3D mapping:
  scene.camera.calibrate({ fovY | focal, sensor:[w,h], aspect, near, far })
  scene.camera.lookFrom([eye], [target], [up])
  scene.camera.unproject(px, py) -> ray
  ray.hitPlane(plane) -> { point } | null
  ray.hitScene() -> { node, point } | null
  scene.project([x, y, z]) -> { x, y, depth, onScreen }

Scene graph, markers, picking:
  scene.load("wl2:/model.gltf", opts) -> Promise<node>
  scene.setAmbientLight(color); scene.light({kind, at, direction, color, intensity, range})
  scene.mesh({vertices, indices, dynamic, ...}) -> node
  scene.surfaceGrid({columns, rows, width, height, dynamic, ...}) -> node
  scene.primitive("cube"|"sphere"|..., opts) -> node
  scene.texture({size:[w,h], data?}) -> texture; texture.map()/unmap(bytes)
  scene.overlay(node|world, opts) -> overlay; scene.overlayState(handle)
  scene.projectBox(points) -> {x,y,width,height,onScreen,points}
  scene.timeline(name?) -> timeline; timeline.animate(node|camera|overlay, opts).pause().resume().cancel()
  scene.marker({ at, label, color, size }) -> node
  scene.upsert(id, { model, at }) -> node   (detection tracking by id)
  scene.onPick(cb); scene.pick(px, py) -> node | null
  node.set({ ... }); node.mesh(); node.updateMesh({vertices?, indices?})
  node.faceTarget(); node.moveLocal(); node.attachTo(); node.detach()
  node.bounds(); node.matrix(); node.get(); node.remove()

Attention cues and animation (advanced by scene.tick(ms)):
  node.attention("rotate"|"pulse"|"glow"|"ping"|"bounce", { color, hz })
  node.animateTo({ position, scale, rotation, opacity, color, ms, ease }, onDone)
  scene.tick(ms) -> completed-tween count (fires completion callbacks)

UI-on-3D surfaces, detections, effects, resize:
  scene.camera.videoSource(name, {mode}) -> metadata; camera.videoState(); camera.filmUv(px,py)
  scene.surface({ name, origin, uAxis, vAxis, pixels }); scene.pickSurface(px, py)
  encodeDetection(record); decodeDetection(bytes)
  scene.detectionSource(name); scene.pollDetections({ model, attention })
  scene.particles({ at, rate, lifetime, ... }); scene.particleCount()
  scene.resize([w, h])   (recreates the ring; bumps metadata.generation)

publishTo() writes RGBA8/sRGB, premultiplied-alpha frames to a libmembus memvid
ring. The engine core (camera, picking, markers, tweens) is renderer-independent
and runs with the Magnum provider off.)";

#if WL2_HAVE_QUICKJS

JSClassID scene_class_id = 0;
JSClassID camera_class_id = 0;
JSClassID ray_class_id = 0;
JSClassID node_class_id = 0;
JSClassID texture_class_id = 0;
JSClassID timeline_class_id = 0;

namespace td = wl2::three_d;

#include "js_bindings/wl2_3d_js_common.cpp"
#include "js_bindings/wl2_3d_js_camera_ray.cpp"
#include "js_bindings/wl2_3d_js_node.cpp"
#include "js_bindings/wl2_3d_js_scene_graph.cpp"
#include "js_bindings/wl2_3d_js_surfaces.cpp"
#include "js_bindings/wl2_3d_js_particles.cpp"
#include "js_bindings/wl2_3d_js_textures.cpp"
#include "js_bindings/wl2_3d_js_timelines.cpp"
#include "js_bindings/wl2_3d_js_detections.cpp"
#include "js_bindings/wl2_3d_js_scene_lifecycle.cpp"
#include "js_bindings/wl2_3d_js_registration.cpp"
#endif

} // namespace

wl2::ModuleInfo wl2_3d_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:3d", wl2_3d_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:3d",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "8cc3d260-02f4-4466-a8d1-3029fbbbf41c",
        .summary = "3D UI engine and shared frame bridge foundation.",
        .api = ThreeDApi,
        .unloadSafe = true,
        .dependencies = {wl2::ModuleDependencyRequirement{
            .name = "wl2:membus",
            .kind = wl2::ModuleDependencyKind::Required,
        }},
    };
}

#if WL2_HAVE_QUICKJS
extern "C" void* wl2_3d_quickjs_module_factory(void* context, const char* moduleName) {
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_3d_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "Scene");
    JS_AddModuleExport(ctx, module, "hasRenderer");
    JS_AddModuleExport(ctx, module, "queryGraphics");
    JS_AddModuleExport(ctx, module, "encodeDetection");
    JS_AddModuleExport(ctx, module, "decodeDetection");
    return module;
}
#endif

#if !WL2_3D_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:3d";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "8cc3d260-02f4-4466-a8d1-3029fbbbf41c";
    out->summary = "3D UI engine and shared frame bridge foundation.";
    out->api = ThreeDApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
