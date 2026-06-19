// Hardening: dynamic resize, untrusted-buffer rejection, denied capabilities,
// and detection-decoder fuzzing. Run with the shared-memory
// capability for the /wl2_3d_test_ prefix.
import { Scene, decodeDetection, encodeDetection } from "wl2:3d";
import { VideoBuffer, hasV12Surface } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

// --- Detection decoder fuzz: never throws, never accepts garbage -----------
{
  // Pseudo-random but deterministic byte payloads of varied lengths.
  let seed = 0x2545f491;
  const rand = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  let accepted = 0;
  for (let i = 0; i < 2000; i++) {
    const len = Math.floor(rand() * 80);
    const bytes = new Uint8Array(len);
    for (let j = 0; j < len; j++) bytes[j] = Math.floor(rand() * 256);
    const out = decodeDetection(bytes.buffer); // must not throw
    if (out !== null) accepted++;
  }
  // Random bytes essentially never form a valid "W3DD" record.
  assert(accepted === 0, `random bytes should not decode (got ${accepted})`);

  // A valid record with one byte corrupted in the magic is rejected.
  const valid = encodeDetection({ id: 1, px: 1, py: 1, imageWidth: 8, imageHeight: 8, class: "x" });
  const corrupt = new Uint8Array(valid.slice(0));
  corrupt[0] ^= 0xff;
  assert(decodeDetection(corrupt.buffer) === null, "corrupt magic should be rejected");
}

// --- Denied capability: no --allow-shared-memory was implied for this name -
{
  const scene = await Scene.create({ size: [16, 16] });
  let denied = false;
  try {
    scene.detectionSource("/not_allowed_prefix/queue");
  } catch (e) {
    denied = e.code === "shared_memory_denied" || /shared.?memory/i.test(e.message);
  }
  assert(denied, "detectionSource outside the allowed prefix must be denied");
  scene.close();
}

// --- Mesh limits and update stability -------------------------------------
{
  const scene = await Scene.create({ size: [16, 16] });

  let tooLarge = false;
  try {
    scene.surfaceGrid({ columns: 1_000_001, rows: 2, width: 1, height: 1 });
  } catch (e) {
    tooLarge = e.code === "3d_invalid_mesh" && /limit|within mesh limits/i.test(e.message);
  }
  assert(tooLarge, "surfaceGrid should reject vertex counts above the mesh limit");

  const vertices = new Float32Array(64 * 64 * 3);
  const indices = new Uint32Array((64 - 1) * (64 - 1) * 6);
  let out = 0;
  for (let y = 0; y < 64 - 1; y++) {
    for (let x = 0; x < 64 - 1; x++) {
      const a = y * 64 + x;
      const b = y * 64 + x + 1;
      const c = (y + 1) * 64 + x + 1;
      const d = (y + 1) * 64 + x;
      indices[out++] = a; indices[out++] = b; indices[out++] = c;
      indices[out++] = a; indices[out++] = c; indices[out++] = d;
    }
  }
  function fillGrid(offset) {
    let i = 0;
    for (let y = 0; y < 64; y++) {
      for (let x = 0; x < 64; x++) {
        vertices[i++] = (x - 31.5) / 12;
        vertices[i++] = (y - 31.5) / 12;
        vertices[i++] = Math.sin((x + offset) * 0.15) * Math.cos((y - offset) * 0.11);
      }
    }
  }
  fillGrid(0);
  const mesh = scene.mesh({ id: "stress-grid", vertices, indices, dynamic: true });
  for (let i = 1; i <= 24; i++) {
    fillGrid(i);
    mesh.updateMesh({ vertices });
  }
  const info = mesh.mesh();
  assert(info.vertexCount === 4096, "larger dynamic mesh should keep vertex count stable");
  assert(info.indexCount === indices.length, "larger dynamic mesh should keep index count stable");

  let rejected = false;
  try {
    mesh.updateMesh({ indices: new Uint32Array([0, 1, 999999]) });
  } catch (e) {
    rejected = e.code === "3d_invalid_mesh";
  }
  assert(rejected, "bad dynamic mesh update should be rejected");
  assert(mesh.mesh().indexCount === indices.length, "rejected update must not corrupt mesh state");

  scene.close();
}

if (!hasV12Surface) {
  console.log("wl2:3d hardening test: surface-dependent checks skipped");
} else {
  // --- Untrusted buffer: opening a nonexistent ring fails gracefully -------
  let threw = false;
  try {
    VideoBuffer.openExisting(`/wl2_3d_test_missing_${Date.now()}`);
  } catch (e) {
    threw = true;
  }
  assert(threw, "opening a missing ring should throw, not crash");

  // --- Dynamic resize: recreate the ring at a new size ---------------------
  const scene = await Scene.create({ size: [32, 16] });
  const name = `/wl2_3d_test_resize_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
  const first = scene.publishTo(name);
  assert(first.width === 32 && first.height === 16, "initial size");

  let reader = VideoBuffer.openExisting(name);
  assert(reader.metadata().width === 32, "reader sees initial width");
  reader.close();

  const resized = scene.resize([48, 24]);
  assert(resized.width === 48 && resized.height === 24, "resize updates size");
  assert(resized.generation >= 1, "resize bumps the generation");

  reader = VideoBuffer.openExisting(name);
  const meta = reader.metadata();
  assert(meta.width === 48 && meta.height === 24, `reader sees resized dims, got ${meta.width}x${meta.height}`);
  const frame = reader.frame(0);
  assert(frame.scanWidth >= 48 * 4, "resized frame stride is valid");
  reader.close();

  // Camera aspect tracked the resize so projection stays correct.
  const screen = scene.project([0, 0, -5]);
  assert(typeof screen.x === "number", "projection still works after resize");

  scene.close();
}

console.log("wl2:3d hardening ok");
