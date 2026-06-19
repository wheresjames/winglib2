import { Scene } from "wl2:3d";
import { VideoBuffer, hasV12Surface } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function frameChecksum(bytes, width, height, stride) {
  let sum = 0;
  for (let y = 0; y < height; y++) {
    const row = y * stride;
    for (let x = 0; x < width; x++) {
      const i = row + x * 4;
      sum = (sum + bytes[i] * 3 + bytes[i + 1] * 5 + bytes[i + 2] * 7) >>> 0;
    }
  }
  return sum;
}

// Count pixels whose color differs from the top-left (background) pixel, so a
// frame that rendered geometry is distinguishable from a blank/cleared buffer.
function foregroundPixels(bytes, width, height, stride) {
  const br = bytes[0], bg = bytes[1], bb = bytes[2];
  let count = 0;
  for (let y = 0; y < height; y++) {
    const row = y * stride;
    for (let x = 0; x < width; x++) {
      const i = row + x * 4;
      if (Math.abs(bytes[i] - br) + Math.abs(bytes[i + 1] - bg) + Math.abs(bytes[i + 2] - bb) > 24) {
        count++;
      }
    }
  }
  return count;
}

const scene = await Scene.create({ size: [64, 48] });
scene.camera.calibrate({ fovY: 50, near: 0.1, far: 100 });
scene.camera.lookFrom([0, 0, 5], [0, 0, 0]);
scene.setAmbientLight("#333333");
const key = scene.light({
  id: "key",
  kind: "directional",
  direction: [0, 0, 1],
  color: "#ffffff",
  intensity: 0.8,
});
assert(key.get().kind === "light", "scene.light should return a light node");

const vertices = new Float32Array([
  -1.0, -1.0, 0.0,
   1.0, -1.0, 0.0,
   0.0,  1.0, 0.0,
]);
const indices = new Uint16Array([0, 1, 2]);
const mesh = scene.mesh({
  id: "dynamic-triangle",
  vertices,
  indices,
  color: "#4cc9f0",
  dynamic: true,
});

let info = mesh.mesh();
assert(info.vertexCount === 3, "mesh vertex count should round-trip");
assert(info.indexCount === 3, "mesh index count should round-trip");
assert(info.triangleCount === 1, "mesh triangle count should round-trip");
assert(info.dynamic === true, "mesh dynamic flag should round-trip");
assert(mesh.get().kind === "mesh", "mesh node kind should be mesh");

let bounds = mesh.bounds();
assert(bounds.radius > 1.0, "mesh bounds should be computed from vertices");
assert(bounds.min[0] === -1 && bounds.max[1] === 1, "mesh min/max should reflect vertices");

mesh.updateMesh({
  vertices: new Float32Array([
    -0.25, -0.25, 0.0,
     0.25, -0.25, 0.0,
     0.0,   0.25, 0.0,
  ]),
  color: "#ef476f",
});
info = mesh.mesh();
assert(info.vertexCount === 3 && info.indexCount === 3, "partial update should preserve index data");
bounds = mesh.bounds();
assert(bounds.radius < 0.5, "updateMesh should recompute bounds");

const center = scene.project([0, 0, 0]);
const picked = scene.pick(center.x, center.y);
assert(picked !== null && picked.id === "dynamic-triangle", "mesh picking should hit triangle geometry");

const grid = scene.surfaceGrid({
  id: "grid-surface",
  columns: 4,
  rows: 3,
  width: 6,
  height: 2,
  at: [0, 0, -1],
  dynamic: true,
});
const gridInfo = grid.mesh();
assert(gridInfo.vertexCount === 12, "surfaceGrid should create columns*rows vertices");
assert(gridInfo.indexCount === 36, "surfaceGrid should create two triangles per cell");
const gridBounds = grid.bounds();
assert(gridBounds.min[0] === -3 && gridBounds.max[1] === 1, "surfaceGrid should center local bounds");

let invalid = null;
try {
  scene.mesh({
    vertices: new Float32Array([0, 0, 0]),
    indices: new Uint16Array([0, 1, 2]),
  });
} catch (error) {
  invalid = error;
}
assert(invalid && invalid.code === "3d_invalid_mesh", "invalid mesh should be rejected");

if (!hasV12Surface) {
  console.log("wl2:3d dynamic mesh frame check skipped: libmembus v1.2 surface unavailable");
} else {
  const name = `/wl2_3d_test_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
  const meta = scene.publishTo(name);
  // The GPU writer (Magnum) is used when graphics is authorized and a GL context
  // exists; otherwise the CPU writer draws the same model. gpuActive reports which
  // path produced the frame; we surface it but don't require it (GPU-less hosts
  // fall back to CPU), matching the shared frame contract.
  console.log(`wl2:3d dynamic mesh renderer=${meta.renderer} gpuActive=${meta.gpuActive}`);
  const video = VideoBuffer.openExisting(name);
  try {
    const frame1 = video.frame(0);
    const bytes1 = frame1.data.uint8Array();
    assert(
      foregroundPixels(bytes1, 64, 48, frame1.scanWidth) > 0,
      "published frame should contain rendered geometry (non-blank)",
    );
    const before = frameChecksum(bytes1, 64, 48, frame1.scanWidth);
    mesh.updateMesh({
      vertices: new Float32Array([
        -1.2, -1.2, 0.0,
         1.2, -1.2, 0.0,
         0.0,  1.2, 0.0,
      ]),
      color: "#80ed99",
    });
    const frame2 = video.frame(0);
    const after = frameChecksum(frame2.data.uint8Array(), 64, 48, frame2.scanWidth);
    assert(before !== after, "dynamic mesh update should change rendered pixels");
  } finally {
    video.close();
  }
}

scene.close();
console.log("wl2:3d dynamic mesh ok");
