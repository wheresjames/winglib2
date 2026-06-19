// UI-on-3D full round-trip across wl2:3d and wl2:slint:
//   surface pick -> barycentric/UV -> UI pixel -> input command -> injected
//   event -> UI re-renders -> ring (texture) updates.
// All CPU: the UI is software-rendered to a FrameRing and the surface geometry
// is solved on the host. (Sampling that ring as a GL texture is the renderer's
// view, gated behind Magnum.)
import { Scene } from "wl2:3d";
import { compile, useOffscreenRendering } from "wl2:slint";
import { VideoBuffer, hasV12Surface } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function centerPixel(name, x, y) {
  const video = VideoBuffer.openExisting(name);
  try {
    const frame = video.frame(0);
    const bytes = frame.data.uint8Array();
    const idx = y * frame.scanWidth + x * 4;
    return { r: bytes[idx], g: bytes[idx + 1], b: bytes[idx + 2] };
  } finally {
    video.close();
  }
}

if (!hasV12Surface) {
  console.log("wl2 UI-on-3D test skipped: libmembus v1.2 surface unavailable");
} else {
  const W = 64;
  const H = 64;

  // --- UI side: an off-screen Slint panel that toggles color on click -------
  useOffscreenRendering();
  const ui = (await compile(`
export component Panel inherits Window {
    width: 64px;
    height: 64px;
    in-out property <bool> on: false;
    background: root.on ? #00ff00 : #ff0000;
    TouchArea { clicked => { root.on = !root.on; } }
}`)).create();

  const ring = `/wl2_3d_test_uion3d_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
  ui.renderOffscreenTo(ring, { size: [W, H] });
  assert(ui.get("on") === false, "UI should start off");
  const before = centerPixel(ring, W / 2, H / 2);
  assert(before.r > 120 && before.g < 90, `UI should start red, got ${before.r},${before.g}`);

  // --- 3D side: a calibrated scene with the UI bound to a quad ---------------
  const scene = await Scene.create({ size: [1280, 720] });
  scene.camera.calibrate({ fovY: 50, near: 0.1, far: 1000 });
  scene.camera.lookFrom([0, 0, 6], [0, 0, 0]);

  // A 4x4 quad centered at the origin in the z=0 plane, mapped to the UI ring.
  // origin = top-left corner (uv 0,0); vAxis points down so uv maps to UI y.
  scene.surface({
    id: "kiosk-screen",
    name: ring,
    origin: [-2, 2, 0],
    uAxis: [4, 0, 0],
    vAxis: [0, -4, 0],
    pixels: [W, H],
  });

  // Pick the screen-center ray; it should land on the quad center -> UI center.
  const hit = scene.pickSurface(640, 360);
  assert(hit !== null, "screen-center ray should hit the surface");
  assert(hit.surface === "kiosk-screen", "hit should report the surface id");
  assert(Math.abs(hit.uv[0] - 0.5) < 0.02 && Math.abs(hit.uv[1] - 0.5) < 0.02,
    `expected uv ~ (0.5,0.5), got ${hit.uv}`);

  const px = Math.round(hit.pixel[0]);
  const py = Math.round(hit.pixel[1]);
  assert(Math.abs(px - W / 2) <= 1 && Math.abs(py - H / 2) <= 1,
    `expected UI pixel ~ center, got ${px},${py}`);

  // --- Round-trip: inject the mapped pixel into the UI ----------------------
  ui.injectPointer(px, py, "click");
  assert(ui.get("on") === true, "surface click should toggle the UI property");

  const after = centerPixel(ring, W / 2, H / 2);
  assert(after.g > before.g + 40, `UI texture should turn green (${before.g} -> ${after.g})`);
  assert(after.r < before.r - 40, `UI texture red should drop (${before.r} -> ${after.r})`);

  // A pick that misses the quad injects nothing.
  const miss = scene.pickSurface(5, 5);
  assert(miss === null, "off-quad pick should miss");

  scene.close();
  console.log("wl2 UI-on-3D round-trip ok");
}
