// Detections -> scene (§14.2): a synthetic detector publishes moving "car" hits
// on a memmsg queue; the scene unprojects each through the calibrated camera
// onto the ground plane and upserts a tracked node. Cars appear and track at
// the expected world positions (within tolerance). Renderer-independent.
import { Scene, encodeDetection, decodeDetection } from "wl2:3d";
import { SharedQueue, hasV12Surface } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

if (!hasV12Surface) {
  console.log("wl2:3d detections test skipped: libmembus v1.2 surface unavailable");
} else {
  const W = 1280;
  const H = 720;
  const queueSize = 65536;
  const name = `/wl2_3d_test_det_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;

  const scene = await Scene.create({ size: [W, H] });
  // Camera at eye height looking toward the horizon: the lower image hits the
  // ground, the upper image sees the sky (so a high detection ray escapes).
  scene.camera.calibrate({ fovY: 50, near: 0.1, far: 1000 });
  scene.camera.lookFrom([0, 5, 16], [0, 5, -10]);

  const producer = SharedQueue.create(name, queueSize, true);
  scene.detectionSource(name, { size: queueSize });

  // Publish a "car" detection at a given image pixel for camera 7, id 0.
  function publish(px, py) {
    const rec = encodeDetection({
      cameraId: 7,
      id: 0,
      ts: 1.0,
      px,
      py,
      imageWidth: W,
      imageHeight: H,
      confidence: 0.95,
      coord: "pixel-top-left",
      class: "car",
    });
    producer.write(rec);
  }

  // The detector's image matches the camera frame here, so the expected world
  // position is the camera's own unprojection of that pixel onto the ground.
  function expectedGround(px, py) {
    const hit = scene.camera.unproject(px, py).hitPlane(scene.ground);
    assert(hit !== null, "test pixel should hit the ground");
    return hit.point;
  }

  // Frame 1: a car in the lower-middle of the image.
  publish(640, 520);
  let summary = scene.pollDetections({ model: "wl2:/models/car.glb" });
  assert(summary.read === 1 && summary.placed === 1, `frame1 should place 1, got ${JSON.stringify(summary)}`);
  assert(scene.count() === 1, "one car node should exist");

  let node = scene.upsert("7:0").get();
  let want = expectedGround(640, 520);
  assert(Math.hypot(node.position[0] - want[0], node.position[2] - want[2]) < 0.01,
    `car should be at the unprojected ground point, got ${node.position} vs ${want}`);
  assert(Math.abs(node.position[1]) < 1e-6, "car should sit on the ground plane");
  assert(node.model === "wl2:/models/car.glb", "car should carry its model id");

  // Frame 2: same id at a new pixel -> tracked (moved, not duplicated).
  publish(740, 540);
  summary = scene.pollDetections({ model: "wl2:/models/car.glb" });
  assert(summary.placed === 1, "frame2 should place 1");
  assert(scene.count() === 1, "tracking by id must not duplicate the node");

  node = scene.upsert("7:0").get();
  want = expectedGround(740, 540);
  assert(Math.hypot(node.position[0] - want[0], node.position[2] - want[2]) < 0.01,
    "tracked car should move to the new ground point");

  // A detection whose ray escapes the ground (aimed at the sky) is a miss.
  publish(640, 10);
  summary = scene.pollDetections({ model: "wl2:/models/car.glb" });
  assert(summary.missed === 1 && summary.placed === 0, `sky detection should miss, got ${JSON.stringify(summary)}`);

  // A garbage payload is counted invalid, never crashes.
  producer.write(new Uint8Array([1, 2, 3, 4, 5]).buffer);
  summary = scene.pollDetections({});
  assert(summary.invalid === 1, `garbage should be invalid, got ${JSON.stringify(summary)}`);

  // Low-confidence detections are filtered out.
  producer.write(encodeDetection({ id: 1, px: 640, py: 520, imageWidth: W, imageHeight: H, confidence: 0.1, class: "car" }));
  summary = scene.pollDetections({ minConfidence: 0.5 });
  assert(summary.filtered === 1 && summary.placed === 0, "low-confidence should be filtered");

  // decodeDetection round-trips the wire format.
  const round = decodeDetection(encodeDetection({ id: 9, px: 1, py: 2, imageWidth: 10, imageHeight: 10, class: "person" }));
  assert(round.id === 9 && round.class === "person", "decodeDetection should round-trip");
  assert(decodeDetection(new Uint8Array([9, 9, 9]).buffer) === null, "garbage decodes to null");

  producer.close();
  scene.close();
  console.log("wl2:3d detections ok");
}
