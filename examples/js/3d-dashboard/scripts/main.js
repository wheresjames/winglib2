// wl2:3d × wl2:slint — comprehensive 3D-UI dashboard.
//
// Builds a calibrated 3D scene, exercises the breadth of the wl2:3d engine, and
// presents it inside a Slint window: the engine renders into a FrameRing that
// the UI shows as a live image (3D-into-UI), projected 3D labels are drawn over
// the viewport, and the control panel drives the engine.
//
// Run in-tree without building the example executable:
//   wl2 run --allow-ui --allow-graphics \
//           --allow-shared-memory --shared-memory-allow /wl2_3d_dashboard_ \
//           --map-resource examples/js/3d-dashboard:wl2:/3d-dashboard \
//           wl2:/3d-dashboard/scripts/main.js
//
// Or build wl2_3d_dashboard_example. Flags: --compile-only (headless feature
// demo + assertions, no window), --selftest (windowed, self-driving), none
// (interactive).
import { Scene, encodeDetection, decodeDetection } from "wl2:3d";
import { compileFile } from "wl2:slint";
import { SharedQueue, hasV12Surface } from "wl2:membus";

const argv = wl2.runtime.argv || [];
const COMPILE_ONLY = argv.includes("--compile-only");
const SELFTEST = argv.includes("--selftest");
const WINDOWED = !COMPILE_ONLY;

const VIEW_W = 960;
const VIEW_H = 540;
const RING = `/wl2_3d_dashboard_view_${Date.now()}`;
const DET_QUEUE = `/wl2_3d_dashboard_det_${Date.now()}`;

function assert(condition, message) {
  if (!condition) {
    throw new Error(`assertion failed: ${message}`);
  }
}

// --- Scene + calibrated camera ---------------------------------------------
const scene = await Scene.create({ size: [VIEW_W, VIEW_H] });
scene.camera.calibrate({ fovY: 55, near: 0.1, far: 1000 });

const cam = { az: Math.PI * 0.22, el: 0.42, radius: 16, target: [0, 1.5, 0] };
function applyCamera() {
  const ce = Math.cos(cam.el);
  const eye = [
    cam.target[0] + cam.radius * ce * Math.sin(cam.az),
    cam.target[1] + cam.radius * Math.sin(cam.el),
    cam.target[2] + cam.radius * ce * Math.cos(cam.az),
  ];
  scene.camera.lookFrom(eye, cam.target);
}
applyCamera();

// --- Procedural environment: grid floor, skydome, and a row of primitives ---
scene.primitive("grid", { id: "floor", size: 24, divisions: 24, at: [0, 0, 0], color: "#243140" });
scene.primitive("skydome", { id: "sky", radius: 200, color: "#0a1622" });

const PRIMITIVE_KINDS = ["cube", "sphere", "cylinder", "cone", "arrow", "plane"];
const PALETTE = ["#4cc9f0", "#80ed99", "#ffd166", "#ef476f", "#b5179e", "#90be6d"];
const placed = [];
function placePrimitive(index, animateIn) {
  const kind = PRIMITIVE_KINDS[index % PRIMITIVE_KINDS.length];
  const angle = (index / PRIMITIVE_KINDS.length) * Math.PI * 2;
  const at = [Math.cos(angle) * 5, 0.8, Math.sin(angle) * 5];
  const node = scene.primitive(kind, {
    id: `prim-${index}`,
    at,
    scale: 0.9,
    color: PALETTE[index % PALETTE.length],
  });
  placed.push({ node, kind, at });
  if (animateIn) {
    node.set({ scale: 0.01 });
    node.animateTo({ scale: 0.9, ms: 350, ease: "outCubic" });
  }
  return node;
}
for (let i = 0; i < PRIMITIVE_KINDS.length; i++) {
  placePrimitive(i, false);
}

// --- Authored content: load a glTF site replica from a wl2:/ resource --------
let siteNode = null;
try {
  siteNode = await scene.load("wl2:/3d-dashboard/assets/site.gltf", {
    id: "site-root",
    at: [0, 0, 0],
    scale: 1,
  });
} catch (error) {
  // The renderer-independent loader validates and records the resource; a load
  // failure should not stop the demo, only note it.
  console.log(`glTF load skipped: ${error.code || error.message}`);
}

// --- A focus node with attention cues + a hero animation timeline ------------
const hero = scene.marker({ id: "hero", at: [0, 2.4, 0], label: "Tower 4", size: 0.9 });
const ATTENTION_CUES = ["rotate", "pulse", "glow", "ping", "bounce"];
let attentionIndex = 1;
hero.attention(ATTENTION_CUES[attentionIndex], { color: "#ffd166", hz: 1.4 });

// --- 2D overlays: projected labels for the hero and a few anchors ------------
const overlayAnchors = [
  { label: "Tower 4", accent: true, world: () => hero.get().position },
  { label: "Origin", accent: false, world: () => [0, 0, 0] },
];
for (let i = 0; i < placed.length; i++) {
  overlayAnchors.push({ label: placed[i].kind, accent: false, world: () => placed[i].at });
}

// --- Particles: a reusable burst emitter -------------------------------------
let burstEmitter = null;
function spawnBurst(at) {
  burstEmitter = scene.particles({
    at: at || [0, 1, 0],
    velocity: [0, 4, 0],
    velocityJitter: [3, 2, 3],
    gravity: [0, -6, 0],
    rate: 200,
    lifetime: 0.8,
    sizeStart: 0.5,
    sizeEnd: 0.0,
    colorStart: "#ffd166",
    colorEnd: [0.9, 0.2, 0.1, 0],
    blend: "additive",
    seed: 7,
    max: 400,
  });
  return burstEmitter;
}

// --- Picking: report the node under a viewport click -------------------------
let lastPicked = "—";
scene.onPick((node) => { lastPicked = node.id; });

// --- Detections: a synthetic detector feeding the ground-plane tracker -------
let detectionProducer = null;
let detectionsPlaced = 0;
let detSeq = 0;
if (hasV12Surface) {
  detectionProducer = SharedQueue.create(DET_QUEUE, 65536, true);
  scene.detectionSource(DET_QUEUE, { size: 65536 });
}
function publishDetection() {
  if (!detectionProducer) return 0;
  // A "car" weaving across the lower half of the image; tracked by id.
  const t = detSeq++;
  const px = VIEW_W * (0.3 + 0.4 * Math.abs(((t % 20) / 20) * 2 - 1));
  const py = VIEW_H * 0.72;
  detectionProducer.write(encodeDetection({
    cameraId: 1,
    id: 0,
    ts: t,
    px,
    py,
    imageWidth: VIEW_W,
    imageHeight: VIEW_H,
    confidence: 0.9,
    coord: "pixel-top-left",
    class: "car",
  }));
  const summary = scene.pollDetections({ model: "wl2:/models/car.glb", minConfidence: 0.5 });
  detectionsPlaced += summary.placed;
  return summary.placed;
}

// --- The 3D → UI frame bridge ------------------------------------------------
// publishTo() renders the scene into a FrameRing; the UI copies the latest frame
// into its `viewport` image each tick. Available when libmembus v1.2 is present.
const frameBridge = hasV12Surface;
let ringSequence = -1;
if (frameBridge) {
  scene.publishTo(RING);
}

// --- Compile the Slint UI ----------------------------------------------------
let ui;
try {
  ui = await compileFile("wl2:/3d-dashboard/ui/app.slint");
} catch (error) {
  if (Array.isArray(error.diagnostics)) {
    for (const d of error.diagnostics) {
      console.log(`${d.line}:${d.column}: ${d.message}`);
    }
  }
  throw error;
}
const win = ui.create();
win.set("viewport-width", VIEW_W);
win.set("viewport-height", VIEW_H);
win.set("primitive-kind", PRIMITIVE_KINDS[placed.length % PRIMITIVE_KINDS.length]);
win.set("attention-cue", ATTENTION_CUES[attentionIndex]);

let lastAction = "ready";
let spin = false;
function setStatus(text) { win.set("status", text); }
function setAction(text) { lastAction = text; win.set("action", text); }

// --- Per-tick update: animate, render, project overlays, refresh stats -------
let tickCount = 0;
function pullFrame() {
  if (!frameBridge) return false;
  const upd = win.setImageFromFrameRing("viewport", RING, { lastSequence: ringSequence });
  if (upd.updated) {
    ringSequence = upd.sequence;
    return true;
  }
  return false;
}
function updateOverlays() {
  const labels = overlayAnchors.map((a) => {
    const p = scene.project(a.world());
    return { x: p.x, y: p.y, text: a.label, accent: a.accent, visible: !!p.onScreen };
  });
  win.set("overlays", labels);
  return labels;
}
function updateStats() {
  win.set("stats", [
    { label: "Nodes", value: String(scene.count()) },
    { label: "Particles", value: String(scene.particleCount()) },
    { label: "Detections placed", value: String(detectionsPlaced) },
    { label: "Ticks", value: String(tickCount) },
    { label: "Last picked", value: lastPicked },
    { label: "Renderer", value: frameBridge ? "FrameRing" : "engine-only" },
  ]);
}
function step(dt) {
  tickCount++;
  if (spin) {
    cam.az += dt * 0.0004;
    applyCamera();
  }
  const completed = scene.tick(dt);
  pullFrame();
  updateOverlays();
  updateStats();
  return completed;
}

// --- Wire the control panel to engine operations -----------------------------
win.on("orbit", (dir) => { cam.az += dir * 0.18; applyCamera(); setAction(`orbit ${dir > 0 ? "right" : "left"}`); });
win.on("tilt", (dir) => {
  cam.el = Math.max(0.05, Math.min(1.4, cam.el + dir * 0.12));
  applyCamera();
  setAction(`tilt ${dir > 0 ? "up" : "down"}`);
});
win.on("zoom", (dir) => {
  cam.radius = Math.max(5, Math.min(40, cam.radius - dir * 1.5));
  applyCamera();
  setAction(`zoom ${dir > 0 ? "in" : "out"}`);
});
win.on("reset-view", () => {
  cam.az = Math.PI * 0.22; cam.el = 0.42; cam.radius = 16; cam.target = [0, 1.5, 0];
  applyCamera();
  setAction("reset view");
});
win.on("next-primitive", () => {
  const node = placePrimitive(placed.length, true);
  overlayAnchors.push({ label: placed[placed.length - 1].kind, accent: false, world: () => placed[placed.length - 1].at });
  win.set("primitive-kind", PRIMITIVE_KINDS[placed.length % PRIMITIVE_KINDS.length]);
  setAction(`added ${node.get().primitive}`);
});
win.on("add-marker", () => {
  const angle = Math.random() * Math.PI * 2;
  const at = [Math.cos(angle) * 7, 1.5 + Math.random() * 3, Math.sin(angle) * 7];
  const id = `marker-${tickCount}-${Math.floor(Math.random() * 1e4)}`;
  const m = scene.marker({ id, at, label: "Site", size: 0.6, billboard: true });
  m.attention("ping", { hz: 1.5 });
  overlayAnchors.push({ label: "Site", accent: false, world: () => m.get().position });
  setAction("added marker");
});
win.on("cycle-attention", () => {
  attentionIndex = (attentionIndex + 1) % ATTENTION_CUES.length;
  const cue = ATTENTION_CUES[attentionIndex];
  hero.attention(cue, { color: "#ffd166", hz: 1.4 });
  win.set("attention-cue", cue);
  setAction(`attention: ${cue}`);
});
win.on("burst", () => { spawnBurst([0, 1, 0]); setAction("particle burst"); });
win.on("spawn-detection", () => {
  const placedNow = publishDetection();
  setAction(placedNow ? "detection tracked" : "detection (no surface)");
});
win.on("set-spin", (on) => { spin = !!on; setAction(spin ? "auto-spin on" : "auto-spin off"); });
win.on("tick", () => { step(33); });
win.on("viewport-clicked", (x, y) => {
  const node = scene.pick(x, y);
  setStatus(node ? `Picked ${node.id} at (${Math.round(x)}, ${Math.round(y)})` : `No node at (${Math.round(x)}, ${Math.round(y)})`);
});

// --- Hero fly-through timeline (camera + node), looping with yoyo -------------
function playTimeline() {
  const tl = scene.timeline("hero-fly");
  tl.animate(hero, { position: [0, 3.4, 0], scale: 1.3, ms: 600, ease: "inOutCubic", yoyo: true, loop: false });
  tl.animate(scene.camera, { fovY: 42, ms: 600, ease: "inOutQuad", yoyo: true });
  setAction("playing fly timeline");
  return tl;
}
win.on("play-timeline", playTimeline);

// =====================================================================
// Mode dispatch
// =====================================================================
if (COMPILE_ONLY) {
  // Headless feature demonstration with assertions — the CI smoke test. No
  // window is opened, so no display or UI capability is required.
  console.log("wl2:3d dashboard: headless feature demo");

  // Camera 2D<->3D round trip.
  const probe = [2, 1.5, -3];
  const projected = scene.project(probe);
  assert(projected.onScreen, "probe point should project on screen");
  const ray = scene.camera.unproject(projected.x, projected.y);
  assert(ray !== null, "unproject should return a ray");
  const ground = scene.camera.unproject(VIEW_W / 2, VIEW_H * 0.8).hitPlane(scene.ground);
  assert(ground !== null && Math.abs(ground.point[1]) < 1e-6, "ground ray should hit y=0");

  // Primitives + content load.
  assert(scene.count() >= PRIMITIVE_KINDS.length + 1, "primitives and hero marker should exist");
  if (siteNode) {
    assert(siteNode.get().kind === "asset", "glTF should load as an asset node");
  }

  // A safe CPU texture round-trip (map/unmap).
  const tex = scene.texture({ size: [4, 4] });
  const bytes = new Uint8Array(tex.map());
  bytes[0] = 10; bytes[1] = 20; bytes[2] = 30;
  tex.unmap(bytes);
  assert(new Uint8Array(tex.map())[0] === 10, "texture unmap should persist bytes");

  // Attention + animation advance deterministically under tick().
  const cue0 = hero.attentionState().phase;
  hero.animateTo({ position: [0, 3, 0], ms: 100, ease: "outCubic" });
  step(50);
  step(60);
  assert(Math.abs(hero.get().position[1] - 3) < 1e-6, "animateTo should reach its target");
  assert(hero.attentionState().phase !== cue0, "tick should advance the attention animator");

  // Timeline (camera + node) completes.
  const tl = playTimeline();
  for (let i = 0; i < 30; i++) step(33);
  assert(tl.state().done >= 1, "timeline should complete its items");

  // Particles simulate and stay bounded.
  spawnBurst([0, 1, 0]);
  step(100);
  assert(scene.particleCount() > 0, "particles should be alive after a burst");

  // Detections place onto the ground plane (when a surface is available).
  if (frameBridge) {
    const before = detectionsPlaced;
    publishDetection();
    assert(detectionsPlaced === before + 1, "a valid detection should be placed");
    assert(decodeDetection(new Uint8Array([1, 2, 3]).buffer) === null, "garbage decodes to null");

    // The 3D → UI bridge: render to the ring and copy it into the UI image.
    const upd = win.setImageFromFrameRing("viewport", RING, {});
    assert(upd.updated && upd.width === VIEW_W && upd.height === VIEW_H,
      "the rendered frame should copy into the UI viewport image");
  }

  // Overlays project to viewport pixels.
  const labels = updateOverlays();
  assert(labels.length >= 2, "overlay labels should be produced");

  // Picking resolves the hero where it projects.
  const heroScreen = scene.project(hero.get().position);
  const pickedNode = scene.pick(heroScreen.x, heroScreen.y);
  assert(pickedNode !== null && lastPicked === "hero", "clicking the hero should pick it");

  // Dynamic resize renegotiates the viewport.
  const resized = scene.resize([VIEW_W, VIEW_H]);
  assert(resized.width === VIEW_W, "resize should report the negotiated width");

  if (detectionProducer) detectionProducer.close();
  scene.close();
  console.log(`wl2:3d dashboard headless demo ok (nodes=${resized.generation >= 0 ? scene.count?.() ?? "?" : "?"})`);
} else {
  // Windowed dashboard. The Timer in the UI drives tick() at ~30 Hz.
  win.set("live", true);
  step(0); // prime the first frame, overlays, and stats
  win.show();

  if (SELFTEST) {
    // Drive a representative slice of the controls, then quit on a UI timer so
    // the windowed path is exercised under a display without human input.
    let frames = 0;
    win.on("tick", () => {
      step(33);
      frames++;
      if (frames === 5) win.invoke("orbit", 1);
      if (frames === 10) win.invoke("next-primitive");
      if (frames === 15) win.invoke("cycle-attention");
      if (frames === 20) win.invoke("burst");
      if (frames === 25) win.invoke("spawn-detection");
      if (frames === 30) win.invoke("play-timeline");
      if (frames === 40) win.invoke("set-spin", true);
      if (frames >= 60) ui.quit();
    });
  }

  await ui.run();
  if (detectionProducer) detectionProducer.close();
  scene.close();
  console.log(`wl2:3d dashboard exited after ${tickCount} ticks`);
}
