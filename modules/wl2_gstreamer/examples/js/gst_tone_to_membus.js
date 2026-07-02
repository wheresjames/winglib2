import { AudioBuffer } from "wl2:membus";
import { parseLaunch } from "wl2:gstreamer";

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

const name = `/wl2_gst_tone_${Date.now()}`;
const audio = AudioBuffer.create(name, 2, 16, 48000, 50, 16);

try {
  const pipeline = parseLaunch(
    "audiotestsrc wave=sine freq=440 num-buffers=8 ! audioconvert ! audioresample ! " +
    "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 ! appsink name=wl2_audio_sink"
  );
  pipeline.attachAudioSink({ audioBufferName: name, create: false });
  pipeline.play();
  if (!pollToEos(pipeline)) throw new Error("tone pipeline timed out");
  const stats = pipeline.stats().wl2_audio_sink;
  const meta = audio.metadata();
  console.log(`buffers=${stats.buffers} sequence=${meta.sequence} caps=${stats.negotiatedCaps}`);
  pipeline.close();
} finally {
  audio.close();
}
