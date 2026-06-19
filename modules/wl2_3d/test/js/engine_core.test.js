// Headless demo for the wl2:3d engine core.
// Exercises the JS-author surface with no GL knowledge: calibrated camera,
// 2D<->3D mapping, markers + attention cues, picking, detections->scene, and
// the tween player with completion callbacks. Runs with the renderer off.
import { Scene } from "wl2:3d";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function near(a, b, tol, message) {
  if (Math.abs(a - b) > tol) {
    throw new Error(`${message} (${a} vs ${b})`);
  }
}

const scene = await Scene.create({ size: [1280, 720] });

// --- Calibrated camera + 2D<->3D round trip -------------------------------
scene.camera.calibrate({ fovY: 52, near: 0.1, far: 5000 });
scene.camera.lookFrom([3, 10, 18], [0, 0, 0]);

const world = [2.5, 1.0, -4.0];
const screen = scene.project(world);
assert(screen.onScreen, "test point should project on screen");

const ray = scene.camera.unproject(screen.x, screen.y);
assert(ray !== null, "unproject should return a ray");
// The original world point must lie on the recovered ray.
const o = ray.origin;
const d = ray.direction;
const toP = [world[0] - o[0], world[1] - o[1], world[2] - o[2]];
const along = toP[0] * d[0] + toP[1] * d[1] + toP[2] * d[2];
const closest = [o[0] + d[0] * along, o[1] + d[1] * along, o[2] + d[2] * along];
const off = Math.hypot(world[0] - closest[0], world[1] - closest[1], world[2] - closest[2]);
near(off, 0, 1e-4, "world point should lie on the unprojected ray");

// --- Detection -> ground plane (the headline 2D->3D workflow) -------------
const hit = scene.camera.unproject(800, 500).hitPlane(scene.ground);
assert(hit !== null, "ray should hit the ground plane");
near(hit.point[1], 0, 1e-6, "ground hit should be on y=0");

// Place / track a detected object by id; a second detection moves it.
const car = scene.upsert("car-7", { model: "wl2:/models/car.glb", at: hit.point });
assert(car.id === "car-7", "upsert should set the node id");
const hit2 = scene.camera.unproject(820, 520).hitPlane(scene.ground);
const carAgain = scene.upsert("car-7", { at: hit2.point });
assert(scene.count() === 1, "upsert by id should track, not duplicate");
near(carAgain.get().position[0], hit2.point[0], 1e-9, "upsert should move the tracked node");

// --- Markers + attention cue ----------------------------------------------
const marker = scene.marker({ at: [0, 2, 0], label: "Building 4", size: 0.8 });
assert(marker.get().label === "Building 4", "marker label should round-trip");
marker.attention("pulse", { color: "#e11d48", hz: 1.5 });
assert(marker.attentionState().kind === "pulse", "attention behavior should be pulse");

const before = marker.attentionState().phase;
scene.tick(120);
const after = marker.attentionState();
assert(after.phase !== before, "tick should advance the attention animator");
assert(after.intensity >= 0 && after.intensity <= 1, "attention intensity in [0,1]");

// --- Picking via onPick ----------------------------------------------------
let picked = null;
scene.onPick((node) => { picked = node.id; });
const markerScreen = scene.project([0, 2, 0]);
const hitNode = scene.pick(markerScreen.x, markerScreen.y);
assert(hitNode !== null, "pick through the marker should hit it");
assert(picked === marker.id, "onPick should report the picked node id");
assert(scene.pick(5, 5) === null, "pick at the corner should miss");

// --- Tween player with completion callback --------------------------------
let done = false;
let endScale = 0;
marker.animateTo({ scale: 1.4, ms: 200, ease: "outCubic" }, () => {
  done = true;
  endScale = marker.get().scale[0];
});
scene.tick(100);
assert(!done, "tween callback should not fire before it completes");
const finished = scene.tick(100);
assert(done, "tween callback should fire on completion (marshalled to JS thread)");
assert(finished === 1, "tick should report one completed tween");
near(endScale, 1.4, 1e-6, "scale should reach the target value");

// Sequenced animations run in order.
const seqOrder = [];
marker.animateTo({ position: [5, 2, 0], ms: 100 }, () => seqOrder.push("move"));
marker.animateTo({ opacity: 0.2, ms: 100 }, () => seqOrder.push("fade"));
scene.tick(100);
scene.tick(100);
assert(seqOrder.join(",") === "move,fade", "queued tweens should run in order");
near(marker.get().position[0], 5, 1e-6, "first tween should reach its target");
near(marker.get().opacity, 0.2, 1e-6, "second tween should reach its target");

// --- Particle effects (CPU simulation; GPU rendering gated behind Magnum) --
const emitter = scene.particles({
  at: [0, 0, 0],
  velocity: [0, 2, 0],
  rate: 100,
  lifetime: 0.5,
  sizeStart: 1.0,
  sizeEnd: 0.0,
  colorStart: "#ff0000",
  colorEnd: [0, 0, 1, 0],
  seed: 99,
});
scene.tick(100); // ~10 particles over 0.1s
let estate = scene.emitterState(emitter.handle);
assert(estate.count >= 9 && estate.count <= 11, `expected ~10 particles, got ${estate.count}`);
assert(scene.particleCount() === estate.count, "particleCount should total emitters");
assert(estate.sample.life > 0 && estate.sample.life < 1, "oldest particle is partway through life");
assert(estate.sample.color[0] < 1 && estate.sample.color[2] > 0, "color interpolates red->blue");
assert(estate.sample.size < 1 && estate.sample.size > 0, "size shrinks over life");
assert(estate.sample.position[1] > 0, "particle rises under +y velocity");

scene.tick(1000); // emission continues, but old particles retire each tick
const churned = scene.emitterState(emitter.handle);
assert(churned.count <= estate.count + 100, "particle count stays bounded by lifetime");

scene.close();
console.log("wl2:3d engine core ok");
