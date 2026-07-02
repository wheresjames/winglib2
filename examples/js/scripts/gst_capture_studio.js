// gst_capture_studio.js — a wl2:gstreamer + wl2:slint capture studio.
//
// Opens a UI that lists the video capture devices configured on the system,
// shows a live preview from the selected device, and lets the user record
// video (.webm) and grab still screenshots (.png/.jpg).
//
// Run:
//   ./build/bin/wl2 run --allow-ui \
//     --shared-memory-allow /wl2_capture \
//     ./examples/js/scripts/gst_capture_studio.js
//
// Opening a window needs --allow-ui; the frame rings need a shared-memory grant
// on the /wl2_capture prefix. Recording/screenshots need v4l2src (for real
// cameras) plus vp8enc+webmmux and pngenc/jpegenc; the UI reports what is
// missing instead of failing.

import { compile } from "wl2:slint";
import {
  DeviceMonitor,
  captureDevice,
  recordVideoBuffer,
  listElements
} from "wl2:gstreamer";

const WIDTH = 640;
const HEIGHT = 480;
const FPS = 30;
const BUFFERS = 8;
const OUT_DIR = "/tmp";

const elementCache = new Map();
function haveElement(name) {
  if (!elementCache.has(name)) {
    elementCache.set(name, listElements({ filter: name }).some((e) => e.name === name));
  }
  return elementCache.get(name);
}

// Discover capture devices. DeviceMonitor is part of core GStreamer, so this
// works even when the optional pbutils feature is off. The synthetic test
// pattern is always offered so the example runs without a camera.
function enumerateDevices() {
  const devices = [{ label: "Test Pattern (no device)", device: null }];
  try {
    const monitor = DeviceMonitor.create({ classes: "Video/Source" });
    for (const dev of monitor.devices || []) {
      if (!dev.path) continue; // need a v4l2 device path to open it
      devices.push({ label: `${dev.displayName || "Camera"} (${dev.path})`, device: dev.path });
    }
  } catch (error) {
    console.log(`device enumeration failed: ${error.message || error}`);
  }
  return devices;
}

const SOURCE = `
import { Button, ComboBox, VerticalBox, HorizontalBox } from "std-widgets.slint";

export component Studio inherits Window {
  title: "wl2 capture studio";
  preferred-width: 700px;
  preferred-height: 620px;

  in property <[string]> device-names;
  in property <image> frame;
  in property <string> status: "Select a device";
  in property <bool> recording: false;
  in property <bool> can-record: true;
  in property <bool> can-shot: true;

  callback device-selected(int);
  callback toggle-record();
  callback screenshot();
  callback tick();

  Timer {
    interval: 33ms;
    running: true;
    triggered => { root.tick(); }
  }

  VerticalBox {
    padding: 12px;
    spacing: 10px;

    HorizontalBox {
      spacing: 8px;
      Text { text: "Device:"; vertical-alignment: center; }
      ComboBox {
        horizontal-stretch: 1;
        model: root.device-names;
        current-index: 0;
        selected => { root.device-selected(self.current-index); }
      }
    }

    Rectangle {
      vertical-stretch: 1;
      background: #101014;
      border-radius: 6px;
      Image {
        width: 100%;
        height: 100%;
        source: root.frame;
        image-fit: contain;
      }
    }

    HorizontalBox {
      spacing: 8px;
      alignment: center;
      Button {
        text: root.recording ? "Stop Recording" : "Start Recording";
        enabled: root.can-record;
        primary: root.recording;
        clicked => { root.toggle-record(); }
      }
      Button {
        text: "Screenshot";
        enabled: root.can-shot;
        clicked => { root.screenshot(); }
      }
    }

    Text {
      text: root.status;
      wrap: word-wrap;
      horizontal-alignment: center;
    }
  }
}
`;

const ui = await compile(SOURCE);
const win = ui.create();

const devices = enumerateDevices();
win.set("device-names", devices.map((d) => d.label));

// Encoder/muxer availability drives which actions the UI enables.
const canRecord = haveElement("vp8enc") && haveElement("webmmux");
const shotEncoder = haveElement("pngenc")
  ? { encoder: "pngenc", ext: "png" }
  : haveElement("jpegenc")
    ? { encoder: "jpegenc", ext: "jpg" }
    : null;
win.set("can-record", canRecord);
win.set("can-shot", shotEncoder !== null);

// Current live-capture session and recording state.
let session = null; // { ring, pipeline }
let recorder = null; // { pipeline, pushedSeq }

function stamp() {
  return new Date().toISOString().replace(/[:.]/g, "-");
}

function latestSequence() {
  if (!session) return 0;
  const bridge = session.pipeline.stats().wl2_video_sink;
  return bridge ? bridge.sequence : 0;
}

function stopRecording(reason) {
  if (!recorder) return;
  try {
    recorder.pipeline.endOfStream();
    // Give the muxer a moment to finalize the file.
    const deadline = Date.now() + 2000;
    while (Date.now() < deadline) {
      const done = recorder.pipeline.busPoll({ timeoutMs: 50, max: 16 })
        .some((m) => m.type === "eos" || m.type === "error");
      if (done) break;
    }
  } catch (error) {
    console.log(`stop recording: ${error.message || error}`);
  } finally {
    recorder.pipeline.close();
    recorder = null;
    win.set("recording", false);
    if (reason) win.set("status", reason);
  }
}

function stopSession() {
  stopRecording();
  if (session) {
    try { session.pipeline.close(); } catch { /* already closed */ }
    session = null;
  }
}

function startSession(index) {
  stopSession();
  const choice = devices[index] || devices[0];
  const ring = `/wl2_capture_${Date.now()}`;
  try {
    const pipeline = captureDevice({
      videoBufferName: ring,
      device: choice.device || undefined,
      width: WIDTH,
      height: HEIGHT,
      fps: FPS,
      buffers: BUFFERS
    });
    pipeline.play();
    session = { ring, pipeline };
    win.set("status", `Live: ${choice.label}`);
  } catch (error) {
    win.set("status", `Cannot open ${choice.label}: ${error.message || error}`);
  }
}

function toggleRecord() {
  if (!session) return;
  if (recorder) {
    const path = recorder.path;
    stopRecording(`Saved video: ${path}`);
    return;
  }
  const path = `${OUT_DIR}/wl2_capture_${stamp()}.webm`;
  try {
    const pipeline = recordVideoBuffer({
      videoBufferName: session.ring,
      outputPath: path,
      encoder: "vp8enc deadline=1",
      muxer: "webmmux"
    });
    pipeline.play();
    recorder = { pipeline, path, pushedSeq: latestSequence() };
    win.set("recording", true);
    win.set("status", `Recording to ${path}`);
  } catch (error) {
    win.set("status", `Record failed: ${error.message || error}`);
  }
}

function screenshot() {
  if (!session || !shotEncoder) return;
  const seq = latestSequence();
  if (seq <= 0) {
    win.set("status", "No frame captured yet");
    return;
  }
  const path = `${OUT_DIR}/wl2_capture_${stamp()}.${shotEncoder.ext}`;
  // A one-shot encode pipeline reading the same ring: appsrc -> videoconvert ->
  // <encoder> -> identity -> filesink. identity fills the muxer slot so a single
  // encoded still lands in the file.
  let shot = null;
  try {
    shot = recordVideoBuffer({
      videoBufferName: session.ring,
      outputPath: path,
      encoder: shotEncoder.encoder,
      muxer: "identity"
    });
    shot.play();
    shot.pushVideoFrame({ slot: (seq - 1) % BUFFERS });
    shot.endOfStream();
    const deadline = Date.now() + 2000;
    while (Date.now() < deadline) {
      const done = shot.busPoll({ timeoutMs: 50, max: 16 })
        .some((m) => m.type === "eos" || m.type === "error");
      if (done) break;
    }
    win.set("status", `Saved screenshot: ${path}`);
  } catch (error) {
    win.set("status", `Screenshot failed: ${error.message || error}`);
  } finally {
    if (shot) shot.close();
  }
}

function onTick() {
  if (!session) return;
  // Copy the newest frame into the preview.
  try {
    win.setImageFromFrameRing("frame", session.ring);
  } catch {
    return; // no frame in the ring yet
  }
  // Feed newly written frames into the recorder, skipping any that were already
  // overwritten in the ring since the last tick.
  if (recorder) {
    const seq = latestSequence();
    if (seq - recorder.pushedSeq > BUFFERS) recorder.pushedSeq = seq - BUFFERS;
    while (recorder.pushedSeq < seq) {
      try {
        recorder.pipeline.pushVideoFrame({ slot: recorder.pushedSeq % BUFFERS });
      } catch (error) {
        stopRecording(`Recording stopped: ${error.message || error}`);
        break;
      }
      recorder.pushedSeq++;
    }
  }
}

win.on("device-selected", (index) => startSession(index));
win.on("toggle-record", toggleRecord);
win.on("screenshot", screenshot);
win.on("tick", onTick);

// --self-test drives the capture/record/screenshot path programmatically on the
// test-pattern device, without opening a window. Used to validate the pipeline
// plumbing in headless environments (no display, no v4l2 camera required).
function pump(ms) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) onTick();
}

if ((wl2.runtime.argv || []).includes("--self-test")) {
  try {
    startSession(0);
    pump(500);
    console.log(`self-test capture sequence=${latestSequence()}`);
    toggleRecord();
    pump(1000);
    toggleRecord();
    screenshot();
    console.log(`self-test status: ${win.get("status")}`);
  } finally {
    stopSession();
  }
} else {
  // Start on the first device (the always-present test pattern) so there is an
  // immediate preview.
  startSession(0);
  try {
    win.show();
    await ui.run();
  } finally {
    stopSession();
  }
}
