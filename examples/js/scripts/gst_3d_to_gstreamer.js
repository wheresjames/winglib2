import { Scene } from "wl2:3d";
import { recordVideoBuffer } from "wl2:gstreamer";

const argv = wl2.runtime.argv || [];
const outputIndex = argv.findIndex((arg) => arg === "--output" || arg === "-o");
const OUTPUT = outputIndex >= 0 ? argv[outputIndex + 1] : "/tmp/wl2_gst_3d_bridge.webm";
const RING = `/wl2_gst_3d_bridge_${Date.now()}`;

function pollToEos(pipeline, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 16 });
    for (const message of messages) {
      if (message.type === "error") throw new Error(message.message || "pipeline error");
      if (message.type === "eos") return true;
    }
  }
  return false;
}

const scene = await Scene.create({ size: [320, 180], buffers: 4 });
const meta = scene.publishTo(RING);

const recorder = recordVideoBuffer({
  videoBufferName: RING,
  outputPath: OUTPUT,
  encoder: "vp8enc deadline=1",
  muxer: "webmmux"
});

try {
  recorder.play();
  recorder.pushVideoFrame({ slot: 0, pts: 0, duration: 33333333 });
  recorder.endOfStream();
  if (!pollToEos(recorder)) throw new Error("3D bridge record timed out");
  console.log(`recorded ${meta.width}x${meta.height} 3D frame to ${OUTPUT}`);
} finally {
  recorder.close();
}
