import { VideoBuffer } from "wl2:membus";
import { testPattern } from "wl2:gstreamer";

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

const name = `/wl2_gst_testpattern_${Date.now()}`;
const video = VideoBuffer.create(name, 160, 90, 30, 4);

try {
  const pipeline = testPattern({
    videoBufferName: name,
    width: 160,
    height: 90,
    fps: 30,
    buffers: 4,
    numBuffers: 12
  });
  pipeline.play();
  if (!pollToEos(pipeline)) throw new Error("test pattern timed out");
  const stats = pipeline.stats().wl2_video_sink;
  const meta = video.metadata();
  console.log(`frames=${stats.frames} sequence=${meta.sequence} caps=${stats.negotiatedCaps}`);
  pipeline.close();
} finally {
  video.close();
}
