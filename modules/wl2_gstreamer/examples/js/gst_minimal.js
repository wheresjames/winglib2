import { parseLaunch, version } from "wl2:gstreamer";

function pollToEos(pipeline, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 16 });
    for (const message of messages) {
      console.log(`${message.type}:${message.source}`);
      if (message.type === "error") throw new Error(message.message || "pipeline error");
      if (message.type === "eos") return true;
    }
  }
  return false;
}

const v = version();
console.log(`GStreamer ${v.gstreamer.string}`);

const pipeline = parseLaunch("videotestsrc num-buffers=3 ! fakesink");
try {
  pipeline.play();
  if (!pollToEos(pipeline)) {
    throw new Error("pipeline timed out before EOS");
  }
  console.log("minimal pipeline complete");
} finally {
  pipeline.close();
}
