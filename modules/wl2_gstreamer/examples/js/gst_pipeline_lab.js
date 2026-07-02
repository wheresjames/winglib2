import { VideoBuffer } from "wl2:membus";
import {
  discoverMedia,
  filePlayback,
  listElements,
  recordVideoBuffer,
  testPattern
} from "wl2:gstreamer";

const argv = wl2.runtime.argv || [];
const inputIndex = argv.findIndex((arg) => arg === "--input" || arg === "-i");
const outputIndex = argv.findIndex((arg) => arg === "--output" || arg === "-o");
const INPUT = inputIndex >= 0 ? argv[inputIndex + 1] : null;
const OUTPUT = outputIndex >= 0 ? argv[outputIndex + 1] : "/tmp/wl2_gst_pipeline_lab.webm";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function pollToEos(pipeline, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 32 });
    for (const message of messages) {
      if (message.type === "error") throw new Error(message.message || "pipeline error");
      if (message.type === "eos") return true;
    }
  }
  return false;
}

function haveElement(name) {
  return listElements({ filter: name }).some((element) => element.name === name);
}

const sourceName = `/wl2_gst_lab_src_${Date.now()}`;
const previewName = `/wl2_gst_lab_preview_${Date.now()}`;
const playbackName = `/wl2_gst_lab_playback_${Date.now()}`;

const source = VideoBuffer.create(sourceName, 160, 90, 30, 4);
const preview = VideoBuffer.create(previewName, 160, 90, 30, 4);
const playback = VideoBuffer.create(playbackName, 160, 90, 30, 4);

try {
  let recorded = false;
  const previewPipe = testPattern({
    videoBufferName: previewName,
    width: 160,
    height: 90,
    fps: 30,
    numBuffers: 8
  });
  previewPipe.play();
  assert(pollToEos(previewPipe), "test pattern did not finish");
  const previewFrames = previewPipe.stats().wl2_video_sink.frames;
  previewPipe.close();
  console.log(`preview frames: ${previewFrames}`);

  source.fill(0, 0x33);
  source.fill(1, 0x99);

  if (haveElement("vp8enc") && haveElement("webmmux")) {
    const record = recordVideoBuffer({
      videoBufferName: sourceName,
      outputPath: OUTPUT,
      encoder: "vp8enc deadline=1",
      muxer: "webmmux"
    });
    record.play();
    record.pushVideoFrame({ slot: 0, pts: 0, duration: 33333333 });
    record.pushVideoFrame({ slot: 1, pts: 33333333, duration: 33333333 });
    record.endOfStream();
    assert(pollToEos(record), "record did not finish");
    record.close();
    recorded = true;
    console.log(`recorded: ${OUTPUT}`);
  } else {
    console.log("skipping record: vp8enc/webmmux are not installed");
  }

  const playbackPath = INPUT || (recorded ? OUTPUT : null);
  if (playbackPath) {
    try {
      const info = discoverMedia({ path: playbackPath });
      console.log(`metadata: duration=${info.duration} streams=${info.streams.length}`);
    } catch (error) {
      if (error.code !== "gstreamer_unsupported") throw error;
      console.log("metadata unavailable: gstreamer-pbutils-1.0 is not linked");
    }

    const player = filePlayback({
      path: playbackPath,
      videoBufferName: playbackName,
      width: 160,
      height: 90,
      fps: 30,
      buffers: 4
    });
    player.play();
    assert(pollToEos(player), "playback did not finish");
    console.log(`playback frames: ${player.stats().wl2_video_sink.frames}`);
    player.close();
  } else {
    console.log("skipping playback: no input file and no recorded output");
  }
} finally {
  source.close();
  preview.close();
  playback.close();
}
