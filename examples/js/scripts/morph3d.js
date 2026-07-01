import { Scene } from "wl2:3d";
import { hasV12Surface } from "wl2:membus";
import { Muxer } from "wl2:ffmpeg";

const argv = wl2.runtime.argv || [];
const COMPILE_ONLY = argv.includes("--compile-only");
const SELFTEST = argv.includes("--selftest");
const outputArgIndex = argv.findIndex((arg) => arg === "--output" || arg === "-o");
const OUTPUT_PATH = outputArgIndex >= 0 ? argv[outputArgIndex + 1] : null;
const RECORD = !!OUTPUT_PATH;

const VIEW_W = 1024;
const VIEW_H = 768;
const FPS = 30;
const LOOP_SECONDS = 22;
const LOOP_FRAMES = FPS * LOOP_SECONDS;
const COLS = 25;
const ROWS = 25;
const RING = `/wl2_morph3d_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
const SLINT_SOURCE = `
  export component MorphWindow inherits Window {
    title: "wl2 morph3d";
    preferred-width: 1100px;
    preferred-height: 840px;
    background: #11151c;

    in property <image> viewport;
    in property <string> status: "ready";
    in property <bool> live: false;
    callback tick();
    callback toggle-spin();
    callback reset-view();

    Timer {
      interval: 33ms;
      running: root.live;
      triggered => { root.tick(); }
    }

    Rectangle {
      width: 100%;
      height: 100%;
      background: #11151c;

      Image {
        width: 100%;
        height: 100%;
        source: root.viewport;
        image-fit: contain;
      }

      Rectangle {
        x: 16px;
        y: 16px;
        width: 260px;
        height: 34px;
        background: #11151ccc;
        border-color: #3b4654;
        border-width: 1px;
        border-radius: 4px;
        Text {
          x: 10px;
          y: 8px;
          text: root.status;
          color: #e8f0f8;
          font-size: 14px;
        }
      }

      TouchArea {
        clicked => { root.toggle-spin(); }
      }
    }
  }
`;

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function mod(value, divisor) {
  return value - Math.floor(value / divisor) * divisor;
}

function transitionWeight(clock, start) {
  if (start < clock && start + 2 > clock) {
    const p = clock - start;
    return p > 1 ? 2 - p : p;
  }
  return 0;
}

const vertices = new Float32Array(COLS * ROWS * 3);
const indices = new Uint16Array((COLS - 1) * (ROWS - 1) * 6);
let ii = 0;
for (let y = 0; y < ROWS - 1; y++) {
  for (let x = 0; x < COLS - 1; x++) {
    const a = y * COLS + x;
    const b = y * COLS + x + 1;
    const c = (y + 1) * COLS + x + 1;
    const d = (y + 1) * COLS + x;
    indices[ii++] = a; indices[ii++] = b; indices[ii++] = c;
    indices[ii++] = a; indices[ii++] = c; indices[ii++] = d;
  }
}

function updateVertices(milliseconds) {
  const pi = Math.PI;
  const pi2 = Math.PI * 2;
  const clock = mod(milliseconds / 2000, 11);
  let out = 0;
  for (let yIndex = 0; yIndex < ROWS; yIndex++) {
    for (let xIndex = 0; xIndex < COLS; xIndex++) {
      const u0 = (xIndex / (COLS - 1)) * pi2 - pi;
      const v0 = (yIndex / (ROWS - 1)) * pi2 - pi;
      let x = 0;
      let y = 0;
      let z = 0;

      let p = 1 > clock ? 1 - clock : transitionWeight(clock, 10);
      x += p * u0;
      y += p * v0;

      p = transitionWeight(clock, 0);
      x += p * Math.sin(u0);
      y += p * v0;
      z += p * Math.cos(u0);

      p = transitionWeight(clock, 1);
      x += p * Math.sin(u0) * (pi - v0) * 0.2;
      y += p * v0;
      z += p * Math.cos(u0) * (pi - v0) * 0.2;

      p = transitionWeight(clock, 2);
      x += p * Math.sin(u0) * Math.cos(v0 / 2);
      y += p * Math.sin(v0 / 2);
      z += p * Math.cos(u0) * Math.cos(v0 / 2);

      p = transitionWeight(clock, 3);
      x += p * (2 + Math.cos(v0)) * Math.cos(u0);
      y += p * Math.sin(v0);
      z += p * (2 + Math.cos(v0)) * Math.sin(u0);

      p = transitionWeight(clock, 4);
      let u = (u0 + pi) * 1.5;
      x += p * (Math.cos(u) * (u / (3 * pi) * Math.cos(v0) + 2));
      y += p * (u * Math.sin(v0) / (3 * pi));
      z += p * (Math.sin(u) * (u / (3 * pi) * Math.cos(v0) + 2));

      p = transitionWeight(clock, 5);
      u = u0 + pi;
      x += p * (0.5 * u * Math.cos(u) * (Math.cos(v0) + 1));
      y += p * (0.5 * u * Math.sin(v0));
      z += p * (0.5 * u * Math.sin(u) * (Math.cos(v0) + 1));

      p = transitionWeight(clock, 6);
      x += p * (Math.cos(u0 * 1.5) * (Math.cos(v0) + 2));
      y += p * (Math.sin(u0 * 1.5) * (Math.cos(v0) + 2));
      z += p * (Math.sin(v0) + u0);

      p = transitionWeight(clock, 7);
      u = u0 * 2;
      const a = 0.5;
      x += p * (a * (Math.cos(u) * Math.cos(v0) + 3 * Math.cos(u) * (1.5 + Math.sin(1.5 * u) / 2)));
      y += p * (a * (Math.sin(v0) + 2 * Math.cos(1.5 * u)));
      z += p * (a * (Math.sin(u) * Math.cos(v0) + 3 * Math.sin(u) * (1.5 + Math.sin(1.5 * u) / 2)));

      p = transitionWeight(clock, 8);
      u = u0 + pi;
      let v = v0 * 0.2;
      x += p * (Math.cos(u) + v * Math.cos(u / 2) * Math.cos(u));
      y += p * (Math.sin(u) + v * Math.cos(u / 2) * Math.sin(u));
      z += p * (v * Math.sin(u / 2));

      p = transitionWeight(clock, 9);
      u = u0;
      v = v0 + pi;
      x += p * ((2 + Math.cos(u / 2) * Math.sin(v) - Math.sin(u / 2) * Math.sin(2 * v)) * Math.cos(u));
      y += p * (Math.sin(u / 2) * Math.sin(v) + Math.cos(u / 2) * Math.sin(2 * v));
      z += p * ((2 + Math.cos(u / 2) * Math.sin(v) - Math.sin(u / 2) * Math.sin(2 * v)) * Math.sin(u));

      vertices[out++] = x;
      vertices[out++] = y;
      vertices[out++] = z;
    }
  }
  return clock;
}

const scene = await Scene.create({ size: [VIEW_W, VIEW_H], buffers: RECORD ? LOOP_FRAMES : 3 });
scene.camera.calibrate({ fovY: 45, near: 0.1, far: 1000 });
scene.camera.lookFrom([0, 6, 15], [0, 0, 0]);
scene.setAmbientLight("#555555");
scene.light({ id: "key", kind: "directional", direction: [0.35, 0.8, 0.5], color: "#ffffff", intensity: 0.9 });
scene.light({ id: "fill", kind: "directional", direction: [-0.6, 0.25, -0.5], color: "#6aa8ff", intensity: 0.35 });

updateVertices(0);
const mesh = scene.mesh({
  id: "morph-grid",
  vertices,
  indices,
  color: "#e8f0f8",
  dynamic: true,
  scale: 1.35,
  rotation: [-0.25, 0, 0],
});
scene.primitive("grid", { id: "floor", size: 12, divisions: 12, at: [0, -4, 0], color: "#2a3440" });

let ui = null;
let win = null;
const slint = await import("wl2:slint");
ui = await slint.compile(SLINT_SOURCE);
if (!COMPILE_ONLY && !RECORD) {
  win = ui.create();
}
let lastSequence = -1;
let spin = true;
let yaw = 0;
let tickCount = 0;
let lastClock = 0;

if (hasV12Surface) {
  scene.publishTo(RING);
}

function pullFrame() {
  if (!hasV12Surface || !win) return false;
  const update = win.setImageFromFrameRing("viewport", RING, { lastSequence });
  if (update.updated) {
    lastSequence = update.sequence;
    return true;
  }
  return false;
}

function step(now) {
  const clock = updateVertices(now);
  if (spin) yaw += 0.018;
  mesh.updateMesh({ vertices, rotation: [-0.25, yaw, 0] });
  scene.tick(33);
  pullFrame();
  tickCount++;
  lastClock = clock;
  if (win) {
    win.set("status", `shape ${clock.toFixed(2)}  frames ${tickCount}`);
  }
}

function recordLoop(path) {
  assert(hasV12Surface, "recording requires libmembus v1.2 surface support");
  assert(path && !path.startsWith("--"), "--output requires a file path");
  for (let i = 0; i < LOOP_FRAMES; i++) {
    step(i * (1000 / FPS));
  }
  const out = Muxer.writeVideoBuffer({
    videoBufferName: RING,
    outputPath: path,
    fps: FPS,
    frames: LOOP_FRAMES,
    startSlot: 1,
    preset: "low-latency",
  });
  assert(out.frames === LOOP_FRAMES, "recorded frame count mismatch");
  assert(out.reopened, "recorded output should reopen");
  assert(out.reopenedWidth === VIEW_W && out.reopenedHeight === VIEW_H, "recorded dimensions mismatch");
  return out;
}

if (win) {
  win.on("tick", () => {
    step(wl2.runtime.now());
    if (SELFTEST && tickCount >= 8) {
      ui.quit();
    }
  });
  win.on("toggle-spin", () => { spin = !spin; });
  win.on("reset-view", () => { yaw = 0; });
}

if (COMPILE_ONLY) {
  for (let i = 0; i < 8; i++) {
    step(i * 120);
  }
  assert(mesh.mesh().vertexCount === COLS * ROWS, "mesh vertex count");
  assert(mesh.mesh().indexCount === (COLS - 1) * (ROWS - 1) * 6, "mesh index count");
  assert(tickCount === 8, "compile-only ticks");
  assert(lastClock >= 0, "clock should update");
  scene.close();
  console.log("wl2 morph3d compile-only ok");
} else if (RECORD) {
  const out = recordLoop(OUTPUT_PATH);
  scene.close();
  console.log(`wl2 morph3d wrote ${out.frames} frames to ${OUTPUT_PATH}`);
} else {
  win.set("live", true);
  step(wl2.runtime.now());
  win.show();
  await ui.run();
  if (SELFTEST) {
    assert(tickCount >= 8, "selftest timer should drive frames");
    assert(!hasV12Surface || lastSequence >= 0, "selftest should display a published frame");
    console.log("wl2 morph3d selftest ok");
  }
  scene.close();
}
