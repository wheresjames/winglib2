# wl2_3d Design

This module starts with the cross-module frame contract before introducing a
renderer. The foundation publishes a deterministic synthetic frame into a
`FrameRing` backed by libmembus `memvid`, using the same pixel contract planned
for rendered frames: RGBA8 bytes ordered R, G, B, A; sRGB boundary; premultiplied
alpha; explicit stride; top-left canonical origin.

## Engine core (model/view split)

The engine core — calibrated camera, 2D↔3D mapping, scene graph, attention
animators, and the tween player — is **renderer-independent CPU state**, kept in
two header-only units with no GPU dependency:

- `src/wl2_3d_math.h`: vectors, a column-major (OpenGL-layout) 4×4 matrix, the
  calibrated pinhole `Camera` (FOV or focal+sensor, pose, near/far), and the
  `project` / `unproject` / ray–plane / ray–sphere primitives. Distortion is a
  documented follow-up (pinhole only for now).
- `src/wl2_3d_engine.h`: the `Engine` — nodes (transform, parenting, material,
  visibility, bounding sphere), markers, attention behaviors
  (`rotate`/`pulse`/`glow`/`ping`) advanced as derived state, a sequenced tween
  player with easing and completion ids, and ray picking.

This is the *model*; the renderer is a *view* over it. Because the
core has no GPU dependency, it builds and is fully unit-tested with the Magnum
provider `off`, satisfying the optional/capability-gated requirement.

Time advances only on `scene.tick(ms)`, called on the JS thread. Tween completion
callbacks are therefore invoked on the JS thread — never from a render thread —
matching the threading rule for the bridge. The JS bindings hold the `Camera`,
`Ray`, and `Node` wrappers; each keeps a `shared_ptr` to the scene so handles
stay valid, and node handles are validated against the engine on every call so a
removed node degrades gracefully instead of dangling.

## UI-on-3D surfaces

A `Surface` binds a UI `FrameRing` to a planar quad: an `origin` (uv 0,0) plus
`uAxis`/`vAxis` spanning it. `pickSurface()` unprojects a screen pixel, intersects
the quad's plane, solves the uv on the parallelogram (Cramer's rule on the
edge-dot matrix, so non-rectangular quads work), and maps uv to a top-left UI
pixel. That pixel is the input command the caller injects into the off-screen
`wl2:slint` UI, closing the surface-pick → UV → UI-pixel → injected-event →
re-render → ring-update round-trip. All of this is CPU geometry over the same
engine core; the only renderer-dependent step is uploading the UI ring as a GL
texture on the surface (Topology B), gated behind the Magnum provider.

## Native GPU renderer

When Magnum/Corrade are available (`WL2_DEPS_MAGNUM=download`, `local`, or an
equivalent global dependency setting), the native renderer can draw the scene
model through a windowless EGL/OpenGL context. JavaScript must still authorize
graphics (`--allow-graphics`); without authorization, without a working GL
context, or after a renderer error, `write_scene_frame()` falls back to the CPU
renderer and keeps publishing the same RGBA8/sRGB/top-left `FrameRing` contract.
The first GPU fallback reason is logged once so headless systems do not spam
stderr every tick.

The headless CI lane is `ctest -L gpu_render`. It runs under
`EGL_PLATFORM=surfaceless` and `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`, renders a
known scene with the GPU and CPU views, writes `gpu.ppm`, `gpu_moved.ppm`, and
`cpu.ppm`, checks that the GPU frame is non-blank and camera-dependent, bounds
the mean RGB error against the CPU frame, and records average render/readback
time.

Anti-aliasing is controlled at configure time with `WL2_3D_GPU_SAMPLES`
(default `4`; use `1` to disable MSAA). The renderer clamps the requested value
to the driver-reported maximum.

The shared-GL path is intentionally kept as a clean extension point rather than
mixed into the headless path. The renderer object is isolated from EGL setup,
takes an already-current context, renders to an explicit offscreen framebuffer,
and copies into caller-owned memory. A future shared-GL integration can replace
the readback target with a caller-supplied framebuffer or texture without
changing the engine model or JavaScript scene API.

## Detections, effects, resize

These extend the same model/view split. The **detection** decoder
(`wl2_3d_detection.h`) is a versioned, fully bounds-checked binary reader over an
untrusted `memmsg` payload; `pollDetections` unprojects each hit through the
calibrated camera onto the ground plane and upserts a node tracked by
`cameraId:id`. The **particle system** (`wl2_3d_particles.h`) is a deterministic
CPU simulation (seeded xorshift for reproducible jitter) advanced by `tick`;
emitters integrate position/velocity and interpolate color/size over life, and
the renderer draws them as instanced quads (WebGL2-safe — no compute/geometry
shaders). **Resize** recreates the `memvid` ring under the same authorized name
(old handle closed first so the new shared memory survives) and bumps a
generation so consumers re-attach. Particle GPU rendering and the glow/outline
post pass are the renderer view, gated behind the Magnum provider.

## WASM readiness (§15)

`wl2_3d_platform.h` declares the two seams a browser port swaps: a
`GraphicsBackend` (native EGL vs web WebGL2, selected by the renderer) and a
`FrameTransport` (native libmembus `memvid` vs a web SharedArrayBuffer / in-memory
backend). The portable `InMemoryFrameTransport` is the single-threaded web
fallback and carries no native dependency. The whole renderer-independent core —
math, scene graph, animators, particles, detections, and the transport — compiles
to **wasm32** via Emscripten in a CTest lane (`three_d.wl2_3d_wasm32_compile`,
also run under node when present). That lane is the regression guard against
native-only or non-WebGL2-safe code drifting into the core; a full Emscripten
runtime port (libmembus/QuickJS/Slint on the web) remains a later, dedicated
effort.

## Native process isolation

The native renderer can live in a separate process without changing the
`FrameRing` contract: the renderer owns the writable `memvid` endpoint and the UI
process opens the same ring as the reader. The UI process treats the ring as an
untrusted producer boundary, validates metadata before copying or uploading, and
continues to own all UI state.

Crash isolation is validated by `three_d.wl2_3d_process_isolation`: a child
process publishes RGBA frames into a `memvid` ring, the parent opens it as the UI
reader, then kills the child and verifies that the parent can still read the last
published frame and continue engine-side UI work. This path is native-only and is
not built for wasm32.
