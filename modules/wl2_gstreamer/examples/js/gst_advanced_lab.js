// gst_advanced_lab.js — advanced wl2:gstreamer pipeline features.
//
// Demonstrates the tee multi-sink helper (preview ring + file at once), a live
// text overlay driven by records read from a wl2:membus SharedQueue, still-frame
// snapshot export, and the latency / caps negotiation utilities.
//
// Run (headless; finite sources so it exits on its own):
//   wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_adv \
//           modules/wl2_gstreamer/examples/js/gst_advanced_lab.js
//
// Writing the recording and PNG uses a filesink and needs no extra permission;
// the shared-memory grant covers the frame rings and the overlay SharedQueue.

import { SharedQueue } from "wl2:membus";
import {
  Caps,
  teeVideoBuffer,
  overlayVideoBuffer,
  listElements
} from "wl2:gstreamer";

const stamp = Date.now();
const PREVIEW = `/wl2_gst_adv_preview_${stamp}`;
const OVERLAY = `/wl2_gst_adv_overlay_${stamp}`;
const QUEUE = `/wl2_gst_adv_status_${stamp}`;
const RECORD = `/tmp/wl2_gst_adv_${stamp}.webm`;
const SNAPSHOT = `/tmp/wl2_gst_adv_${stamp}.png`;

const have = (name) => listElements({ filter: name }).some((e) => e.name === name);

function pump(pipeline, onTick, maxIterations = 300) {
  for (let i = 0; i < maxIterations; ++i) {
    for (const message of pipeline.busPoll({ timeoutMs: 20, max: 32 })) {
      if (message.type === "error") throw new Error(message.message || "pipeline error");
      if (message.type === "eos") return true;
    }
    if (onTick) onTick(i);
  }
  return false;
}

// --- Caps negotiation utility ---
const caps = Caps.parse("video/x-raw,format=RGBA,width=320,height=240,framerate=30/1");
console.log(`caps: ${caps.structures[0].name} ${caps.structures[0].fields.width}x${caps.structures[0].fields.height}`);

// --- tee: preview ring + recorded file simultaneously ---
const canRecord = have("vp8enc") && have("webmmux");
const tee = teeVideoBuffer({
  videoBufferName: PREVIEW,
  outputPath: canRecord ? RECORD : undefined,
  source: "videotestsrc num-buffers=60 pattern=ball",
  width: 320,
  height: 240,
  fps: 30
});
tee.play();

// Once frames are flowing, report latency + negotiated caps and grab a still.
let snapped = false;
pump(tee, () => {
  if (snapped || tee.stats().wl2_video_sink.frames < 1) return;
  snapped = true;
  const latency = tee.queryLatency();
  console.log(`latency: supported=${latency.supported} live=${latency.live} min=${latency.minLatency}`);
  const neg = tee.negotiatedCaps({ element: "wl2_video_sink", pad: "sink" });
  console.log(`negotiated sink caps: ${neg.negotiated} ${neg.structures[0]?.fields.format}`);
  if (have("pngenc")) {
    const shot = tee.snapshot({ path: SNAPSHOT });
    console.log(`snapshot: ${shot.width}x${shot.height} -> ${shot.path}`);
  } else {
    const shot = tee.snapshot({});
    console.log(`snapshot metadata only: ${shot.width}x${shot.height} (no pngenc)`);
  }
});
console.log(`tee preview frames: ${tee.stats().wl2_video_sink.frames}${canRecord ? `, recorded ${RECORD}` : " (no encoder, skipped file)"}`);
tee.close();

// --- overlay text driven from a SharedQueue ---
// A producer publishes low-rate status records; the render loop reads the latest
// record each tick and pushes it into the live textoverlay. This is the plan's
// recommended transport for lower-rate metadata (not per-frame callbacks).
const statusQueue = SharedQueue.create(QUEUE, 4096, true);
const overlay = overlayVideoBuffer({
  videoBufferName: OVERLAY,
  text: "starting",
  source: "videotestsrc num-buffers=60 pattern=smpte",
  width: 320,
  height: 240,
  fps: 30
});
overlay.play();

let ticks = 0;
pump(overlay, () => {
  // Producer side: emit a status record roughly every 10 ticks.
  if (ticks % 10 === 0) statusQueue.write(`frame batch ${ticks / 10} @ ${new Date().toISOString()}`);
  // Consumer side: apply the newest queued status to the overlay.
  const record = statusQueue.read(0);
  if (record && !record.empty) {
    const text = String.fromCharCode(...new Uint8Array(record.payload));
    overlay.setOverlayText({ text });
  }
  ticks++;
});
console.log(`overlay frames: ${overlay.stats().wl2_video_sink.frames}`);
overlay.close();
statusQueue.close();

console.log("advanced lab done");
