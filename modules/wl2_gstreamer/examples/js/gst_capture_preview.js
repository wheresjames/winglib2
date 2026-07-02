import { captureDevice, DeviceMonitor } from "wl2:gstreamer";

const argv = wl2.runtime.argv || [];
const deviceIndex = argv.findIndex((arg) => arg === "--device" || arg === "-d");
const DEVICE = deviceIndex >= 0 ? argv[deviceIndex + 1] : null;
const LIST_DEVICES = argv.includes("--list-devices");
const PLAY = argv.includes("--play");
const BUFFER = `/wl2_gst_capture_${Date.now()}`;

function poll(pipeline, iterations = 20) {
  for (let i = 0; i < iterations; ++i) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 16 });
    for (const message of messages) {
      if (message.type === "error") throw new Error(message.message || "pipeline error");
    }
  }
}

if (LIST_DEVICES) {
  const devices = DeviceMonitor.create();
  console.log(`devices: ${devices.devices.length}`);
  for (const device of devices.devices) {
    if (device.path || device.displayName) {
      console.log(`${device.class}: ${device.displayName}${device.path ? ` (${device.path})` : ""}`);
    }
  }
}

const pipeline = captureDevice({
  videoBufferName: BUFFER,
  device: DEVICE || undefined,
  width: 320,
  height: 240,
  fps: 30,
  buffers: 4
});

try {
  if (PLAY) {
    pipeline.play();
    poll(pipeline);
    console.log(`captured frames: ${pipeline.stats().wl2_video_sink.frames}`);
  } else {
    console.log("capture pipeline constructed");
  }
  console.log(`buffer: ${BUFFER}`);
} finally {
  pipeline.close();
}
