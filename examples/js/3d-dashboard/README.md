# 3D-UI dashboard (wl2:3d × wl2:slint)

A cross-module example that puts a live **wl2:3d** viewport inside a **wl2:slint**
window. The 3D engine renders into a libmembus `FrameRing`; the UI copies the
latest frame into an `image` property every tick (**3D-into-UI**). Projected 3D
labels are drawn over the viewport, and the control panel turns button clicks
into engine operations.

## What it demonstrates

- **Calibrated camera** + orbit/tilt/zoom and a `scene.project`/`unproject`
  round trip, with overlay labels projected to viewport pixels.
- **Procedural primitives** — grid floor, skydome, cube/sphere/cylinder/cone/
  arrow/plane — and **glTF content loading** from a `wl2:/` resource.
- **Markers, attention cues** (`rotate`/`pulse`/`glow`/`ping`/`bounce`), and the
  **tween + timeline player** (camera and node animation, yoyo).
- **Particles** (burst emitter) and a safe **CPU texture** `map()/unmap()`.
- **Picking** — clicking the viewport resolves the node under the cursor.
- **Detections → scene** — a synthetic detector publishes image-space hits on a
  `memmsg` queue that get unprojected onto the ground plane and tracked by id.
- **The 3D → UI frame bridge** via `instance.setImageFromFrameRing(...)`.

## Run in-tree (no build)

```sh
wl2 run --allow-ui --allow-graphics \
        --allow-shared-memory --shared-memory-allow /wl2_3d_dashboard_ \
        --map-resource examples/js/3d-dashboard:wl2:/3d-dashboard \
        wl2:/3d-dashboard/scripts/main.js
```

## Build and run the executable

Enable examples (`-DWL2_BUILD_EXAMPLES=ON`) and build `wl2_3d_dashboard_example`.

- `wl2_3d_dashboard_example` — interactive dashboard window.
- `wl2_3d_dashboard_example --compile-only` — headless feature demo with
  assertions; opens no window (the CI smoke test, `examples.js.3d_dashboard_compile`).
- `wl2_3d_dashboard_example --selftest` — windowed self-driving run (needs a
  display; registered only when `WL2_SLINT_DISPLAY_TESTS=ON`).

## Notes

- Capabilities are scoped: shared-memory access is limited to the
  `/wl2_3d_dashboard_` name prefix, and `--allow-graphics` covers the renderer's
  graphics capability.
- With Magnum/Corrade enabled, the dashboard uses the native GPU renderer for
  mesh/grid frames when graphics is authorized. If GL is unavailable, it falls
  back to the CPU rasterizer and keeps publishing the same `FrameRing` pixels.
  `scene.tick()` re-renders the published frame each step, so the viewport tracks
  the live scene either way.
