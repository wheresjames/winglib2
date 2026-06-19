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

const scene = await Scene.create({ size: [640, 360] });
scene.camera.calibrate({ fovY: 60, near: 0.1, far: 1000 });
scene.camera.lookFrom([0, 0, 10], [0, 0, 0]);

const parent = scene.marker({ id: "parent", at: [10, 0, 0], size: 1 });
const child = scene.marker({ id: "child", at: [12, 0, 0], size: 0.5, pivot: [0.1, 0.2, 0.3] });

child.attachTo(parent, { preserveWorld: true });
let state = child.get();
near(state.world[0], 12, 1e-9, "attach preserveWorld should keep world x");
near(state.position[0], 2, 1e-9, "attach preserveWorld should convert to local position");
assert(state.parent !== 0, "child should have a parent after attach");
near(state.pivot[1], 0.2, 1e-9, "pivot should round-trip");

child.detach({ preserveWorld: true });
state = child.get();
near(state.world[0], 12, 1e-9, "detach preserveWorld should keep world x");
assert(state.parent === 0, "child should detach to root");

const mover = scene.marker({ id: "mover", at: [0, 0, 0], size: 0.5 });
mover.faceTarget([10, 0, 0]);
state = mover.get();
near(state.rotation[1], Math.PI / 2, 1e-6, "faceTarget should yaw toward +x");
mover.moveLocal([0, 0, 2]);
state = mover.get();
near(state.position[0], 2, 1e-6, "moveLocal should move along local forward");
near(state.position[2], 0, 1e-6, "moveLocal should rotate local z into world x");

const bounds = mover.bounds();
near(bounds.center[0], 2, 1e-6, "bounds center follows world position");
near(bounds.radius, 0.5, 1e-9, "bounds radius follows marker size");
const matrix = mover.matrix();
assert(Array.isArray(matrix) && matrix.length === 16, "matrix should expose 16 numbers");
near(matrix[12], 2, 1e-6, "matrix translation x should match world position");

const overlayNode = scene.marker({ id: "overlay-node", at: [0, 0, 0], size: 0.4, billboard: true });
const overlay = scene.overlay(overlayNode, {
  id: "status",
  label: "Status",
  offset: [0, 1, 0],
  leaderLine: true,
});
assert(overlay.id === "status" && overlay.label === "Status", "overlay metadata should round-trip");
assert(overlay.visible, "center overlay should be visible");
near(overlay.screen.x, 320, 1e-6, "center overlay x");
assert(overlay.leaderLine, "leaderLine should round-trip");

overlayNode.set({ at: [1, 0, 0] });
const movedOverlay = scene.overlayState(overlay.handle);
assert(movedOverlay.screen.x > overlay.screen.x, "overlay should track node movement");

const pointOverlay = scene.overlay([0, 0, 0], { id: "point" });
assert(pointOverlay.visible, "world-point overlay should project");

overlayNode.attention("bounce", { hz: 2 });
scene.tick(125);
let attention = overlayNode.attentionState();
assert(attention.kind === "bounce", "bounce attention kind");
assert(attention.lift > 0, "bounce should derive lift");

overlayNode.attention("ping", { hz: 1 });
scene.tick(250);
attention = overlayNode.attentionState();
assert(attention.kind === "ping", "ping attention kind");
assert(attention.ring > 0 && attention.ring < 1, "ping should derive expanding ring");
assert(attention.intensity > 0 && attention.intensity < 1, "ping intensity should fade");

scene.close();
console.log("wl2:3d rich scene ok");
