import { Scene } from "wl2:3d";
import { hasV12Surface } from "wl2:membus";

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

const scene = await Scene.create({ size: [48, 24] });
scene.camera.calibrate({ fovY: 60, near: 0.1, far: 1000 });
scene.camera.lookFrom([0, 0, 10], [0, 0, 0]);

if (hasV12Surface) {
  const name = `/wl2_3d_test_camera_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
  scene.publishTo(name);
  const video = scene.camera.videoSource(name, { mode: "filmPlane" });
  assert(video.name === name, "camera video name should round-trip");
  assert(video.mode === "filmPlane", "camera video mode should round-trip");
  assert(video.width === 48 && video.height === 24, "camera video dimensions");
  assert(video.format === "rgba8", "camera video format");
  const state = scene.camera.videoState();
  assert(state.sequence >= video.sequence, "camera video state should expose sequence");
  const uv = scene.camera.filmUv(24, 12);
  near(uv[0], 0.5, 1e-9, "film u");
  near(uv[1], 0.5, 1e-9, "film v");
}

const projected = scene.projectBox([
  [-1, -1, 0],
  [1, -1, 0],
  [1, 1, 0],
  [-1, 1, 0],
]);
assert(projected.points.length === 4, "projectBox should return projected points");
assert(projected.width > 0 && projected.height > 0, "projectBox should compute a positive rectangle");
assert(projected.onScreen, "centered box should be on screen");

const node = scene.marker({ id: "animated-node", at: [0, 0, 0], size: 0.5 });
const timeline = scene.timeline("node-main");
let completed = 0;
timeline.animate(node, {
  position: [10, 0, 0],
  ms: 100,
  ease: "linear",
  onComplete: () => {
    completed += 1;
  },
});
scene.tick(50);
let pos = node.get().position;
near(pos[0], 5, 1e-6, "linear timeline should advance node halfway");
scene.tick(50);
near(node.get().position[0], 10, 1e-6, "timeline should complete node animation");
assert(completed === 1, "timeline completion callback should run");
assert(timeline.state().name === "node-main", "timeline name should round-trip");
assert(timeline.state().done === 1, "completed timeline item should be done");

const paused = scene.timeline();
paused.animate(node, { position: [20, 0, 0], ms: 100 });
paused.pause();
scene.tick(100);
near(node.get().position[0], 10, 1e-6, "paused timeline should not advance");
paused.resume();
scene.tick(100);
near(node.get().position[0], 20, 1e-6, "resumed timeline should advance");

const looping = scene.timeline();
looping.animate(node, { position: [30, 0, 0], ms: 100, loop: true, yoyo: true });
scene.tick(100);
near(node.get().position[0], 30, 1e-6, "looping timeline reaches target");
scene.tick(50);
const yoyoX = node.get().position[0];
assert(yoyoX > 20 && yoyoX < 30, "yoyo loop should move back toward the start");
looping.cancel();
const canceledState = looping.state();
assert(canceledState.canceled && canceledState.done === 1, "cancel should mark loop item done");

const overlay = scene.overlay(node, { id: "animated-overlay", offset: [0, 0, 0], leaderLine: true });
const overlayTimeline = scene.timeline("overlay");
overlayTimeline.animate(overlay, { offset: [1, 2, 0], ms: 100, ease: "linear" });
scene.tick(50);
let overlayState = scene.overlayState(overlay.handle);
near(overlayState.world[0], node.get().position[0] + 0.5, 1e-6, "overlay offset x halfway");
near(overlayState.world[1], 1, 1e-6, "overlay offset y halfway");
scene.tick(50);
overlayState = scene.overlayState(overlay.handle);
near(overlayState.world[0], node.get().position[0] + 1, 1e-6, "overlay offset x complete");
near(overlayState.world[1], 2, 1e-6, "overlay offset y complete");

const cameraTimeline = scene.timeline();
cameraTimeline.animate(scene.camera, {
  eye: [0, 5, 20],
  target: [0, 1, 0],
  fovY: 45,
  near: 0.5,
  far: 500,
  ms: 100,
});
scene.tick(100);
const camera = scene.camera.state();
near(camera.eye[1], 5, 1e-6, "camera timeline eye y");
near(camera.eye[2], 20, 1e-6, "camera timeline eye z");
near(camera.target[1], 1, 1e-6, "camera timeline target y");
near(camera.fovY, 45, 1e-6, "camera timeline fov");
near(camera.near, 0.5, 1e-6, "camera timeline near");
near(camera.far, 500, 1e-6, "camera timeline far");

scene.close();
console.log("wl2:3d camera timeline ok");
