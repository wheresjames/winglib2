# wl2:3d API

## Scene lifecycle and frame bridge

- `Scene.create({ size?: [width, height] }) -> Promise<Scene>`
- `scene.publishTo(name) -> metadata`
- `scene.metadata() -> metadata`
- `scene.close()`

`publishTo()` writes RGBA8/sRGB, premultiplied-alpha, top-left-origin frames to a
libmembus `memvid` ring. It requires the runtime shared-memory capability for the
target name (and the graphics capability when a renderer is present).

With Magnum/Corrade enabled, `publishTo()` and `scene.tick()` use the native GPU
renderer when `--allow-graphics` is present and a windowless GL context can be
created. If GPU setup or rendering fails, the scene logs the first fallback
reason once and continues with the CPU renderer. `metadata().gpuActive` reports
whether the most recent published frame was GPU-rendered.

## Engine core

The engine core is **renderer-independent** — camera math, picking, markers, and
the tween player run with the Magnum provider `off`. The renderer (when present)
is a view over this same model.

### Content loading, primitives, and textures

- `scene.load("wl2:/path/model.gltf" | "wl2:/path/model.glb", options?) -> Promise<node>`
  — validates and opens a `wl2:/` glTF resource, then returns the subtree root
  node. Host files must be exposed through the existing resource mapping policy;
  direct host paths are rejected.
- `scene.mesh({ vertices, indices, normals?, uvs?, dynamic?, ...nodeOptions }) -> node`
  — creates engine-owned indexed triangle geometry from flat arrays or typed
  arrays. `vertices` are x/y/z triples; `indices` are triangle indices.
  `node.mesh()` reports counts and flags, and
  `node.updateMesh({ vertices?, indices?, normals?, uvs?, color? })` validates
  and copies updated geometry back into the node. Bounds and picking radius are
  recomputed after creation and update.
- `scene.surfaceGrid({ columns, rows, width, height, dynamic?, ...nodeOptions }) -> node`
  — convenience helper that creates a deformable indexed triangle grid in the
  local x/y plane, centered on the node origin. `columns` and `rows` are vertex
  counts, not cell counts.
- `scene.primitive(kind, options?) -> node` — creates asset-free geometry metadata
  for `cube`, `sphere`, `cylinder`, `cone`, `arrow`, `grid`, `plane`, or
  `skydome`. Common options are `at`, `scale`, `material`/`color`, and `id`;
  shape options include `radius`, `height`, `segments`, `size`, and `divisions`.
- `scene.texture({ size: [w, h], data? }) -> texture` — creates a safe RGBA8
  texture buffer. `texture.metadata()` reports the pixel contract;
  `texture.map() -> ArrayBuffer` returns a copy; `texture.unmap(bytes)` validates
  and copies modified bytes back.
- `scene.setAmbientLight(color) -> scene` sets the CPU renderer's ambient light.
- `scene.light({ id?, kind?: "directional" | "point", at?, direction?, color?,
  intensity?, range? }) -> node` creates a renderer light. Directional lights
  use `direction`; point lights use `at`/`position` and `range`.

### Calibrated camera and 2D↔3D mapping

- `scene.camera.calibrate({ fovY? | focal?, sensor?: [w, h], aspect?, near?, far? })`
  — `fovY` in degrees, or `focal` + `sensor` (mm) for a real-lens calibration.
- `scene.camera.lookFrom([eye], [target], [up?])`
- `scene.camera.unproject(px, py) -> ray | null` — screen pixel (top-left origin)
  to a world ray.
- `scene.camera.videoSource(name, { mode?: "background" | "filmPlane" | "projectedTexture" })`
  — attach a validated RGBA `FrameRing` as the camera video source.
- `scene.camera.videoState() -> metadata | null`
- `scene.camera.filmUv(px, py) -> [u, v]`
- `scene.camera.state() -> { eye, target, up, fovY, near, far, aspect }`
- `ray.origin`, `ray.direction` — `[x, y, z]` arrays.
- `ray.hitPlane(plane) -> { point } | null` — `plane` is `{ normal, point }`,
  `{ y }`, or `scene.ground` (the `y = 0` plane).
- `ray.hitScene() -> { node, point } | null` — nearest picked node.
- `scene.project([x, y, z]) -> { x, y, depth, onScreen }` — forward projection for
  2D-anchored overlays/labels.
- `scene.projectBox(points) -> { x, y, width, height, onScreen, points }` —
  project a set of world-space points into a screen-space rectangle, useful for
  detection boxes and camera overlays.

### Scene graph, markers, picking

- `scene.marker({ at, label?, color?, size? }) -> node`
- `scene.upsert(id, { model?, at?, ... }) -> node` — create or update a node by
  id; the detection-tracking path (the same id reuses the node).
- `scene.onPick(cb)` / `scene.pick(px, py) -> node | null`
- `node.id`, `node.get()`, `node.set({ position?, scale?, rotation?, pivot?,
  color?, opacity?, visible?, label?, size? })`, `node.remove()`.
- `node.faceTarget([x, y, z])`, `node.moveLocal([x, y, z])`,
  `node.attachTo(parent, { preserveWorld? })`, `node.detach({ preserveWorld? })`
  — high-level scene-graph helpers for common 3D-UI authoring.
- `node.bounds() -> { center, min, max, radius }` and `node.matrix() -> number[]`
  — inspection and raw transform escape hatches.

### 2D overlays

`wl2_3d` owns projection/depth/visibility and emits overlay layout data; Slint
owns text rendering, layout, styling, and interaction.

- `scene.overlay(node | [x, y, z], { id?, label?, offset?, leaderLine? })`
  creates an overlay anchored to a node or world point and returns its current
  projected state.
- `scene.overlayState(handle) -> { handle, id, label, world, screen, visible,
  leaderLine }` updates the projected overlay data after `scene.tick()` or node
  movement. `screen` has the same shape as `scene.project(...)`.

### Attention cues and animation

Both are advanced by `scene.tick(ms)`, which runs on the JS thread; tween
completion callbacks are invoked there (marshalled to the JS thread, never from a
render thread).

- `node.attention("rotate" | "pulse" | "glow" | "ping" | "bounce", { color?, hz? })`
- `node.attentionState() -> { kind, hz, phase, intensity, spin, lift, ring }`
- `node.animateTo({ position?, scale?, rotation?, opacity?, color?, ms?, ease? },
  onDone?)` — `scale` accepts a number (uniform) or `[x, y, z]`; `ease` is one of
  `linear`, `inQuad`, `outQuad`, `inOutQuad`, `inCubic`, `outCubic`, `inOutCubic`.
  Tweens queued on the same node run in sequence.
- `scene.tick(ms) -> number` — count of tweens completed this tick.
- `scene.count() -> number` — live node count.
- `scene.timeline(name?)` creates a deterministic timeline advanced by
  `scene.tick(ms)`. `timeline.animate(node | scene.camera | overlay, options)`
  supports node properties, overlay `offset`, and camera `eye`, `target`,
  `fovY`, `near`, and `far`; timeline controls are `pause()`, `resume()`,
  `cancel()`, and `state()`. Options include `ms`, `ease`, `loop`, `yoyo`, and
  `onComplete`.

### UI-on-3D surfaces

Bind a UI `FrameRing` (produced by `wl2:slint`'s off-screen renderer) to a planar
quad and map picks back to UI pixels — the §4.2 input-command flow.

- `scene.surface({ name, origin?, uAxis?, vAxis?, pixels: [w, h], id? }) -> { id, name, handle }`
  — `origin` is the world position of uv (0,0) (the UI top-left); `uAxis`/`vAxis`
  span the quad to uv (1,0)/(0,1), with `vAxis` pointing along the UI's downward
  direction; `pixels` is the bound UI's pixel size.
- `scene.pickSurface(px, py) -> { surface, name, uv: [u, v], pixel: [px, py], point } | null`
  — unprojects the screen pixel, intersects the bound quad, and returns the UV
  and the top-left UI pixel to inject into the off-screen UI
  (`instance.injectPointer(...)`).

Sampling the UI ring as an actual GL texture on the surface (Topology B) is the
renderer-side view, gated behind the Magnum provider; the geometry/round-trip
above is renderer-independent.

### Detections → scene (§14.2)

Consume image-space detections from a libmembus `memmsg` queue and place/track
world objects on the ground plane.

- `encodeDetection(record) -> ArrayBuffer` / `decodeDetection(bytes) -> record | null`
  — the versioned, bounds-checked wire format. `record` is
  `{ cameraId, id, sourceFrameSeq?, ts, px, py, imageWidth, imageHeight,
  confidence, coord, class, bbox? }`; `coord` is `"pixel-top-left"` or
  `"normalized-top-left"`. `decodeDetection` returns `null` for any malformed
  payload (the fuzz contract).
- `scene.detectionSource(name, { size? })` — attach a read-only queue
  (shared-memory gated).
- `scene.pollDetections({ model?, attention?, minConfidence?, max? }) -> { read, placed, missed, invalid, filtered }`
  — decode pending records, unproject each onto the ground plane via the
  calibrated camera, and `upsert` a tracked node keyed by `cameraId:id`. Rays
  that escape the ground count as `missed`; bad records as `invalid`;
  low-confidence as `filtered`.

### Effects: particles (§13.2)

CPU particle simulation; GPU rendering (instanced quads, additive/alpha blend,
glow/outline post pass — all WebGL2-safe) is the renderer's view, gated behind
the Magnum provider. Advanced by `scene.tick(ms)`.

- `scene.particles({ at, velocity?, velocityJitter?, gravity?, rate?, lifetime?,
  colorStart?, colorEnd?, sizeStart?, sizeEnd?, blend?, max?, seed?, emitting? }) -> { handle }`
- `scene.particleCount() -> number` — total live particles.
- `scene.emitterState(handle) -> { count, blend, sample? }` — `sample` is the
  oldest particle's interpolated `{ life, size, position, color }`.

### Dynamic resize

- `scene.resize([width, height]) -> metadata` — renegotiate the viewport. With a
  ring open it recreates the `memvid` ring under the same name, bumps
  `metadata.generation`, and updates the camera aspect; with no ring it just sets
  the size for the next `publishTo`.
