import { compileFile, openFileDialog, saveFileDialog } from "wl2:slint";
import {
  AudioConverter,
  Demuxer,
  Evidence,
  FilterGraph,
  Muxer,
  PacketQueue,
  ReplaySession,
  analyzeTimestamps,
  capabilities,
  filters,
  generateSyntheticFixture,
  hardware,
  listCodecs,
  listFormats,
  profilePacketQueue,
  version
} from "wl2:ffmpeg";

let ui;
try {
  ui = await compileFile("wl2:/ffmpeg-replay-lab/ui/app.slint");
} catch (error) {
  if (Array.isArray(error.diagnostics)) {
    for (const diagnostic of error.diagnostics) {
      console.log(`${diagnostic.line}:${diagnostic.column}: ${diagnostic.message}`);
    }
  }
  throw error;
}

const args = wl2.runtime.argv || [];
const compileOnly = args.includes("--compile-only");
const selftest = args.includes("--selftest");
const headlessReportIndex = args.indexOf("--headless-report");

const ffv = version();
const caps = capabilities();
const videoCodecs = listCodecs({ type: "video" });
const audioCodecs = listCodecs({ type: "audio" });
const formats = listFormats();
const filterInfo = filters();
const hardwareInfo = hardware();

let win = null;
let source = null;
let streams = [];
let packet = null;
let replay = null;
let previewRingName = "";
let currentUs = 0;
let durationUs = 10_000_000;
let playing = false;
let speed = 1;
let seekMode = "accurate";
let videoFormat = "rgba";
let audioFormat = "s16";
let scaleSpec = "320x180";
let exportLength = 5;
let exportCodec = "rawvideo";
let transcodePreset = "low-latency";
let transcodeCodec = "auto";
let transcodeAudio = true;
let lastFrame = null;
let lastAudio = null;
let lastPipelineRows = [];
let lastTimestampDiagnostics = null;
let lastPlaybackPlan = null;
let lastOutputPath = "";

function stamp() {
  return Date.now();
}

function tempPath(prefix, ext) {
  return `/tmp/wl2_ffmpeg_${prefix}_${stamp()}.${ext}`;
}

function baseName(path) {
  return String(path).split("/").pop();
}

// Prompt the user for an output destination so nothing is silently dropped in
// /tmp. Returns the chosen path, or null if the user cancelled.
function promptSave(title, defaultName, ext) {
  return saveFileDialog({
    title,
    defaultName,
    filters: [
      { name: ext.toUpperCase(), extensions: [ext] },
      { name: "All files", extensions: ["*"] }
    ]
  });
}

// Record the most recent file an action produced and surface a one-click
// "Open / Verify" affordance so the human can load it back and inspect it.
function setLastOutput(path) {
  lastOutputPath = path;
  if (win) {
    win.set("last-output", compactValue(path, 60));
    win.set("has-output", true);
  }
}

function parseSpeed(value) {
  const parsed = Number(String(value).replace("x", ""));
  return Number.isFinite(parsed) && parsed !== 0 ? parsed : 1;
}

function parseSeconds(value, fallback) {
  const parsed = Math.round(Number(value));
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.max(1, Math.min(24 * 60 * 60, parsed));
}

function compactValue(value, max = 42) {
  const text = String(value);
  if (text.length <= max) {
    return text;
  }
  if (text.startsWith("/")) {
    const parts = text.split("/");
    const tail = parts.slice(-2).join("/");
    return tail.length + 4 <= max ? `.../${tail}` : `...${text.slice(-(max - 3))}`;
  }
  return `${text.slice(0, max - 3)}...`;
}

function sourceDurationFromStreams(foundStreams, fallbackSeconds) {
  for (const stream of foundStreams) {
    if (typeof stream.duration === "number" && stream.duration > 0 &&
        typeof stream.timeBaseNum === "number" && stream.timeBaseNum > 0 &&
        typeof stream.timeBaseDen === "number" && stream.timeBaseDen > 0) {
      return Math.round((stream.duration * stream.timeBaseNum * 1_000_000) / stream.timeBaseDen);
    }
    for (const key of ["durationUs", "duration_us", "duration"]) {
      if (typeof stream[key] === "number" && stream[key] > 0) {
        return stream[key] > 1_000_000_000 ? stream[key] : stream[key] * 1_000_000;
      }
    }
  }
  return Math.max(1, fallbackSeconds) * 1_000_000;
}

function closeReplay() {
  if (replay) {
    try {
      replay.close();
    } catch (_) {
      // Best-effort cleanup for an interactive demo.
    }
  }
  replay = null;
}

function inspectSource(path, fixtureMetadata = null) {
  const demuxer = Demuxer.open(path);
  const foundStreams = demuxer.streams();
  const firstPacket = demuxer.readPacket();
  demuxer.close();

  closeReplay();
  replay = ReplaySession.open(path);
  source = {
    path,
    fixture: fixtureMetadata,
    generated: !!fixtureMetadata
  };
  streams = foundStreams;
  packet = firstPacket;
  durationUs = sourceDurationFromStreams(foundStreams, fixtureMetadata ? fixtureMetadata.durationSeconds : 10);
  currentUs = 0;
  previewRingName = `/wl2_ffmpeg_preview_${stamp()}`;
  lastFrame = null;
  lastAudio = null;
  lastTimestampDiagnostics = null;
  lastPlaybackPlan = null;
  lastPipelineRows = [
    { key: "Ready", value: "Use the buttons below to exercise encode, mux, queue, evidence, and diagnostics paths" }
  ];
}

function generateFixture(seconds = 12, codec = "rawvideo", path = null) {
  const fixture = generateSyntheticFixture({
    name: "replay-lab",
    path: path || tempPath(`replay_lab_${seconds}s_${codec}`, "avi"),
    fps: 30,
    durationSeconds: seconds,
    width: 640,
    height: 360,
    gop: 15,
    tone: true,
    videoCodec: codec
  });
  inspectSource(fixture.path, fixture);
  return fixture;
}

function ensureSource() {
  if (!source) {
    generateFixture(12, "rawvideo");
  }
}

function videoStream() {
  return streams.find((stream) => stream.type === "video") || {};
}

function audioStream() {
  return streams.find((stream) => stream.type === "audio") || {};
}

function updateSourceRows() {
  if (!win || !source) {
    return;
  }
  const v = videoStream();
  const a = audioStream();
  const rows = [
    { key: "Path", value: compactValue(source.path, 36) },
    { key: "Video", value: `${v.codec || "unknown"} ${v.width || 0}x${v.height || 0}` },
    { key: "Audio", value: a.codec ? `${a.codec} ${a.sampleRate || 0} Hz ${a.channels || 0}ch` : "none" },
    { key: "Duration", value: `${(durationUs / 1_000_000).toFixed(2)} s` },
    { key: "First packet", value: packet ? `${packet.size} bytes on stream ${packet.streamIndex}` : "none" },
    { key: "FFmpeg", value: ffv.avVersionInfo || ffv.avformat },
    { key: "Codecs", value: `${videoCodecs.length} video / ${audioCodecs.length} audio` },
    { key: "Formats", value: String(formats.length) },
    { key: "Filters", value: filterInfo.enabled ? (filterInfo.filters || []).join(", ") : "not compiled" },
    { key: "Hardware", value: `${hardwareInfo.deviceTypeCount || 0} types, software fallback ${hardwareInfo.softwareFallback ? "yes" : "no"}` }
  ];
  win.set("source-rows", rows.map((row) => ({
    key: compactValue(row.key, 14),
    value: compactValue(row.value, 38)
  })));
}

function updatePipelineRows(extra = null) {
  if (!win) {
    return;
  }
  const rows = extra ? [extra, ...lastPipelineRows] : lastPipelineRows;
  win.set("pipeline-rows", rows.map((row) => ({
    key: compactValue(row.key, 14),
    value: compactValue(row.value, 38)
  })).slice(0, 4));
}

function updateDiagnostics() {
  if (!win) {
    return;
  }
  const state = replay ? replay.state() : {};
  const rows = [
    { name: "Position", value: `${(currentUs / 1_000_000).toFixed(3)} s` },
    { name: "Speed", value: `${speed}x ${speed < 0 ? "reverse" : "forward"}` },
    { name: "Seek mode", value: seekMode },
    { name: "Video conversion", value: lastFrame ? `${lastFrame.format} ${lastFrame.width}x${lastFrame.height} ${lastFrame.size} bytes` : videoFormat },
    { name: "Audio conversion", value: lastAudio ? `${lastAudio.format} ${lastAudio.sampleRate} Hz ${lastAudio.channels}ch` : audioFormat },
    { name: "Published frames", value: String(state.publishedFrames || 0) },
    { name: "Repeats / drops", value: `${state.repeatCount || 0} / ${state.dropCount || 0}` },
    { name: "PTS anomalies", value: lastTimestampDiagnostics ? `${lastTimestampDiagnostics.duplicatePts} dup, ${lastTimestampDiagnostics.backwardPts} backward` : "not run" },
    { name: "A/V plan", value: lastPlaybackPlan ? `${lastPlaybackPlan.clock}, audio ${lastPlaybackPlan.audioEnabled ? "on" : "muted"}` : "not run" }
  ];
  win.set("diagnostics", rows.map((row) => ({
    name: compactValue(row.name, 14),
    value: compactValue(row.value, 34)
  })));
  win.set("position", Math.max(0, Math.min(1000, (currentUs / Math.max(1, durationUs)) * 1000)));
}

function publishPreview(label) {
  ensureSource();
  const published = replay.publishFrame({
    videoBufferName: previewRingName,
    timestampUs: currentUs,
    create: true,
    format: "rgba",
    fps: 30,
    buffers: 4
  });
  win.setImageFromFrameRing("preview-image", previewRingName);
  win.set("preview-label", `${label}: ${published.width}x${published.height} ${published.format} pts ${published.ptsUs} us`);
  updateDiagnostics();
  return published;
}

function seekTo(timestampUs, label = "seek") {
  ensureSource();
  currentUs = Math.max(0, Math.min(Math.max(0, durationUs - 1), timestampUs));
  replay.seek({ timestampUs: currentUs });
  const frame = replay.extractFrame({ timestampUs: currentUs, format: videoFormat, mode: seekMode });
  lastFrame = frame;
  publishPreview(`${label} (${frame.resultMode || seekMode})`);
  win.set("status", `Seeked to ${(currentUs / 1_000_000).toFixed(3)}s using ${seekMode}; converted probe frame to ${frame.format} (${frame.size} bytes)`);
}

function tickPlayback() {
  if (!playing) {
    return;
  }
  currentUs += Math.round(33_333 * speed);
  if (currentUs < 0) {
    currentUs = 0;
    playing = false;
    win.set("playing", false);
  } else if (currentUs >= durationUs) {
    currentUs = Math.max(0, durationUs - 1);
    playing = false;
    win.set("playing", false);
  }
  publishPreview("playback");
}

function runInitialAnalysis() {
  ensureSource();
  lastPlaybackPlan = replay.playbackPlan({ speed: Math.abs(speed), reverse: speed < 0 });
  lastTimestampDiagnostics = analyzeTimestamps({ path: source.path });
  const profile = profilePacketQueue({
    path: source.path,
    queueName: `/wl2_ffmpeg_profile_lab_${stamp()}`,
    iterations: 32
  });
  lastPipelineRows = [
    { key: "A/V sync", value: `${lastPlaybackPlan.clock}, audio ${lastPlaybackPlan.audioEnabled ? "synced" : "muted"}, drift ${Math.round(lastPlaybackPlan.driftBudgetUs)}us` },
    { key: "Timestamps", value: `${lastTimestampDiagnostics.packets} packets, ${lastTimestampDiagnostics.anomalies.length} anomalies` },
    { key: "Queue profile", value: `${profile.perRecordUs.toFixed(2)} us/record` }
  ];
}

function reportMetrics() {
  return {
    module: "wl2:ffmpeg",
    purpose: "interactive replay lab",
    ffmpeg: ffv,
    capabilities: caps,
    source,
    streams,
    packet,
    lastFrame,
    lastAudio,
    playbackPlan: lastPlaybackPlan,
    timestampDiagnostics: lastTimestampDiagnostics,
    filters: filterInfo,
    hardware: hardwareInfo,
    videoCodecs: videoCodecs.length,
    audioCodecs: audioCodecs.length,
    formats: formats.length
  };
}

if (compileOnly) {
  console.log("wl2_ffmpeg Replay Lab compiled");
  globalThis.__ffmpegReplayLabCompiled = true;
} else if (headlessReportIndex >= 0 || selftest) {
  generateFixture(2, "rawvideo");
  runInitialAnalysis();
  const frame = replay.extractFrame({ timestampUs: 500_000, format: "rgb24", mode: "accurate" });
  const audio = replay.extractAudio({ timestampUs: 500_000, durationUs: 100_000, sampleRate: 16_000, channels: 1, format: "s16" });
  lastFrame = frame;
  lastAudio = audio;
  const out = generateSyntheticFixture({
    name: "replay-lab-export",
    path: tempPath("export_2s_clock_counter", "avi"),
    fps: 30,
    durationSeconds: 2,
    width: 640,
    height: 360,
    gop: 15,
    tone: true
  });
  lastPipelineRows.unshift({ key: "Export", value: `${out.path} ${out.frameCount} frames clock/counter + tone` });
  console.log(JSON.stringify(reportMetrics(), null, 2));
  closeReplay();
} else {
  generateFixture(12, "rawvideo");
  runInitialAnalysis();

  win = ui.create();
  win.set("selected-speed", "1x");
  win.set("selected-mode", seekMode);
  win.set("selected-video-format", videoFormat);
  win.set("selected-audio-format", audioFormat);
  win.set("selected-scale", scaleSpec);
  win.set("export-length", String(exportLength));
  win.set("export-codec", exportCodec);
  win.set("transcode-preset", transcodePreset);
  win.set("transcode-codec", transcodeCodec);
  win.set("transcode-audio", transcodeAudio);
  win.set("filters-enabled", !!filterInfo.enabled);
  updateSourceRows();
  updatePipelineRows();
  publishPreview("initial frame");
  win.set("status", "Replay Lab is ready. Load a file or generate/export a clock+counter fixture, then use the controls to exercise decode, seek, conversion, mux, queue, evidence, and diagnostics paths.");

  win.on("play", () => {
    ensureSource();
    playing = true;
    win.set("playing", true);
    lastPlaybackPlan = replay.playbackPlan({ speed: Math.abs(speed), reverse: speed < 0 });
    win.set("status", `Playing at ${speed}x with ${lastPlaybackPlan.clock}; audio ${lastPlaybackPlan.audioEnabled ? "eligible for sync" : "muted by plan"}`);
    updateDiagnostics();
  });
  win.on("pause", () => {
    playing = false;
    win.set("playing", false);
    win.set("status", "Paused");
    updateDiagnostics();
  });
  win.on("tick", () => tickPlayback());
  win.on("seek-delta", (deltaUs) => seekTo(currentUs + deltaUs, "relative seek"));
  win.on("seek-percent", (value) => {
    const target = (Number(value) / 1000) * durationUs;
    // Ignore the echo from our own position updates while playing; only act on
    // a real drag (a meaningful jump away from the current cursor).
    if (playing && Math.abs(target - currentUs) < 100_000) {
      return;
    }
    seekTo(target, "timeline seek");
  });
  win.on("set-seek-mode", (mode) => {
    seekMode = mode;
    win.set("selected-mode", mode);
    seekTo(currentUs, `${mode} seek`);
  });
  win.on("set-speed", (value) => {
    speed = parseSpeed(value);
    win.set("selected-speed", value);
    if (replay) {
      lastPlaybackPlan = replay.playbackPlan({ speed: Math.abs(speed), reverse: speed < 0 });
    }
    win.set("status", `Selected ${speed}x. Reverse and high-speed playback exercise the scheduler/audio mute plan.`);
    updateDiagnostics();
  });
  win.on("set-video-format", (value) => {
    videoFormat = value;
    win.set("selected-video-format", value);
  });
  win.on("set-audio-format", (value) => {
    audioFormat = value;
    win.set("selected-audio-format", value);
  });
  win.on("set-scale", (value) => {
    scaleSpec = value;
    win.set("selected-scale", value);
  });
  win.on("set-export-length", (value) => {
    exportLength = parseSeconds(value, exportLength);
    win.set("export-length", String(exportLength));
  });
  win.on("set-export-codec", (value) => {
    exportCodec = value;
    win.set("export-codec", value);
  });
  win.on("set-transcode-preset", (value) => {
    transcodePreset = value;
    win.set("transcode-preset", value);
  });
  win.on("set-transcode-codec", (value) => {
    transcodeCodec = value;
    win.set("transcode-codec", value);
  });
  win.on("set-transcode-audio", (value) => {
    transcodeAudio = !!value;
    win.set("transcode-audio", transcodeAudio);
  });
  win.on("open-last-output", () => {
    if (!lastOutputPath) {
      return;
    }
    try {
      playing = false;
      win.set("playing", false);
      inspectSource(lastOutputPath, null);
      updateSourceRows();
      updatePipelineRows();
      publishPreview("output review");
      win.set("active-page", "source");
      win.set("status", `Loaded output for review: ${lastOutputPath}. Play/seek to verify it decodes correctly.`);
    } catch (error) {
      win.set("status", `Could not open output ${lastOutputPath}: ${error.message || error}`);
    }
  });
  win.on("generate-fixture", async () => {
    try {
      const path = await promptSave("Save synthetic source as", `synthetic_source_${exportCodec}.avi`, "avi");
      if (!path) {
        win.set("status", "Generate cancelled");
        return;
      }
      playing = false;
      win.set("playing", false);
      generateFixture(12, exportCodec, path);
      runInitialAnalysis();
      updateSourceRows();
      updatePipelineRows();
      publishPreview("generated fixture");
      setLastOutput(source.path);
      win.set("status", `Generated source with burned-in clock/frame counter and synced tone audio: ${source.path}`);
    } catch (error) {
      win.set("status", `Generate failed: ${error.message || error}`);
    }
  });
  win.on("open-file", async () => {
    const path = await openFileDialog({
      title: "Open media file",
      filters: [
        { name: "Media", extensions: ["avi", "mp4", "mkv", "mov", "webm", "mpg", "mpeg", "ts"] },
        { name: "All files", extensions: ["*"] }
      ]
    });
    if (!path) {
      win.set("status", "Open cancelled");
      return;
    }
    try {
      playing = false;
      win.set("playing", false);
      inspectSource(path, null);
      updateSourceRows();
      updatePipelineRows();
      publishPreview("opened file");
      win.set("status", `Loaded ${path}. Use play, seek, conversion, transcode, remux, queue, and evidence controls against this file.`);
    } catch (error) {
      win.set("status", `Open failed: ${error.message || error}`);
    }
  });
  win.on("export-test-video", async () => {
    try {
      exportLength = parseSeconds(win.get("export-length"), exportLength);
      const path = await promptSave("Save test video as", `clock_counter_${exportLength}s.avi`, "avi");
      if (!path) {
        win.set("status", "Export cancelled");
        return;
      }
      const out = generateSyntheticFixture({
        name: "replay-lab-export",
        path,
        fps: 30,
        durationSeconds: exportLength,
        width: 640,
        height: 360,
        gop: 15,
        tone: true,
        videoCodec: exportCodec
      });
      lastPipelineRows.unshift({ key: "Export test video", value: `${out.frameCount} frames -> ${baseName(out.path)}` });
      updatePipelineRows();
      setLastOutput(out.path);
      win.set("status", `Exported ${exportLength}s test video with burned-in clock, frame counter, and synced tone audio: ${out.path}. Use Open / Verify to play it back.`);
    } catch (error) {
      win.set("status", `Export failed: ${error.message || error}`);
    }
  });
  win.on("convert-video", () => {
    try {
      ensureSource();
      lastFrame = replay.extractFrame({ timestampUs: currentUs, format: videoFormat, mode: seekMode });
      publishPreview(`${videoFormat} probe`);
      const pixels = Math.max(1, lastFrame.width * lastFrame.height);
      const bpp = (lastFrame.size / pixels).toFixed(2);
      win.set("status", `Decoded current frame and converted to ${lastFrame.format}: ${lastFrame.width}x${lastFrame.height}, ${lastFrame.size} bytes (${bpp} bytes/pixel). Preview shows the same frame; the conversion details appear under Diagnostics.`);
    } catch (error) {
      win.set("status", `Video conversion failed: ${error.message || error}`);
    }
  });
  win.on("convert-audio", () => {
    try {
      ensureSource();
      const decoded = replay.extractAudio({
        timestampUs: currentUs,
        durationUs: 250_000,
        sampleRate: 48_000,
        channels: 2,
        format: "s16"
      });
      lastAudio = AudioConverter.convert({
        data: decoded.data,
        inFormat: "s16",
        inSampleRate: decoded.sampleRate,
        inChannels: decoded.channels,
        outFormat: audioFormat,
        outSampleRate: 16_000,
        outChannels: 1
      });
      win.set("status", `Decoded audio and converted to ${lastAudio.format} ${lastAudio.sampleRate} Hz ${lastAudio.channels}ch, ${lastAudio.samples} samples (${lastAudio.size} bytes)`);
      updateDiagnostics();
    } catch (error) {
      win.set("status", `Audio conversion failed: ${error.message || error}`);
    }
  });
  win.on("scale-video", () => {
    try {
      ensureSource();
      const out = FilterGraph.apply({
        path: source.path,
        timestampUs: currentUs,
        filter: `scale=${scaleSpec.replace("x", ":")}`,
        format: "rgb24"
      });
      lastFrame = out;
      win.set("status", `FilterGraph scale ${scaleSpec}: ${out.width}x${out.height} ${out.format}, ${out.size} bytes`);
      updateDiagnostics();
    } catch (error) {
      win.set("status", `Scale failed: ${error.message || error}`);
    }
  });
  win.on("transcode", async () => {
    try {
      ensureSource();
      const path = await promptSave("Transcode to", `${baseName(source.path).replace(/\.[^.]*$/, "")}_transcoded.avi`, "avi");
      if (!path) {
        win.set("status", "Transcode cancelled");
        return;
      }
      win.set("status", `Transcoding (${transcodePreset}${transcodeAudio ? ", with audio" : ", no audio"})… this runs synchronously and may pause the UI briefly.`);
      const options = { path: source.path, outputPath: path, preset: transcodePreset, audio: transcodeAudio };
      if (transcodeCodec && transcodeCodec !== "auto") {
        options.videoCodec = transcodeCodec;
      }
      const out = Muxer.transcode(options);
      lastPipelineRows.unshift({ key: "Transcode", value: `${out.videoCodec} ${out.videoPackets}v/${out.audioPackets}a -> ${baseName(out.outputPath)}` });
      updatePipelineRows();
      setLastOutput(out.outputPath);
      win.set("status", `Transcoded to ${out.videoCodec} (${out.videoPackets} video, ${out.audioPackets} audio packets): ${out.outputPath}. Use Open / Verify to play it back.`);
    } catch (error) {
      win.set("status", `Transcode failed: ${error.message || error}`);
    }
  });
  win.on("remux", async () => {
    try {
      ensureSource();
      const path = await promptSave("Remux to", `${baseName(source.path).replace(/\.[^.]*$/, "")}_remux.avi`, "avi");
      if (!path) {
        win.set("status", "Remux cancelled");
        return;
      }
      const out = Muxer.remux({ path: source.path, outputPath: path });
      lastPipelineRows.unshift({ key: "Remux", value: `${out.packets} packets, extradata ${out.extradataPreserved ? "preserved" : "changed"} -> ${baseName(out.outputPath)}` });
      updatePipelineRows();
      setLastOutput(out.outputPath);
      win.set("status", `Remuxed ${out.packets} packets (extradata ${out.extradataPreserved ? "preserved" : "changed"}): ${out.outputPath}. Use Open / Verify to play it back.`);
    } catch (error) {
      win.set("status", `Remux failed: ${error.message || error}`);
    }
  });
  win.on("packet-queue", async () => {
    try {
      ensureSource();
      const path = await promptSave("Packet-queue round trip to", `${baseName(source.path).replace(/\.[^.]*$/, "")}_queue.avi`, "avi");
      if (!path) {
        win.set("status", "Packet queue cancelled");
        return;
      }
      const out = PacketQueue.streamThroughQueue({
        path: source.path,
        outputPath: path,
        queueName: `/wl2_ffmpeg_stream_lab_${stamp()}`,
        policy: "lossless",
        queueSize: 1048576,
        maxDepth: 16
      });
      lastPipelineRows.unshift({ key: "Packet queue", value: `${out.recordsRead}/${out.recordsWritten}, dropped ${out.dropped} -> ${baseName(out.outputPath)}` });
      updatePipelineRows();
      setLastOutput(out.outputPath);
      win.set("status", `Streamed compressed packets through queue. Payload ${out.payloadPreserved ? "preserved" : "changed"}, timestamps ${out.timestampsPreserved ? "preserved" : "changed"}: ${out.outputPath}. Use Open / Verify to play it back.`);
    } catch (error) {
      win.set("status", `Packet queue failed: ${error.message || error}`);
    }
  });
  win.on("export-still", async () => {
    try {
      ensureSource();
      const path = await promptSave("Save still as", `still_${Math.round(currentUs / 1000)}ms.png`, "png");
      if (!path) {
        win.set("status", "Still export cancelled");
        return;
      }
      const out = Evidence.exportStill({ path: source.path, outputPath: path, timestampUs: currentUs, format: "png" });
      lastPipelineRows.unshift({ key: "Still", value: `${out.bytes} bytes -> ${baseName(out.outputPath)}` });
      updatePipelineRows();
      setLastOutput(out.outputPath);
      win.set("status", `Exported still frame (${out.bytes} bytes): ${out.outputPath}. Use Open / Verify to view the saved image.`);
    } catch (error) {
      win.set("status", `Still export failed: ${error.message || error}`);
    }
  });
  win.on("export-clip", async () => {
    try {
      ensureSource();
      const path = await promptSave("Save clip as", `clip_${Math.round(currentUs / 1000)}ms.avi`, "avi");
      if (!path) {
        win.set("status", "Clip export cancelled");
        return;
      }
      const startUs = Math.max(0, currentUs - 500_000);
      const endUs = Math.min(durationUs, currentUs + 1_500_000);
      const out = Evidence.extractClip({ path: source.path, outputPath: path, startUs, endUs, mode: "auto" });
      lastPipelineRows.unshift({ key: "Clip", value: `${out.mode}, ${out.videoFrames} frames -> ${baseName(out.outputPath)}` });
      updatePipelineRows();
      setLastOutput(out.outputPath);
      win.set("status", `Exported ${out.mode} clip (${out.videoFrames} frames) around current position: ${out.outputPath}. Use Open / Verify to play it back.`);
    } catch (error) {
      win.set("status", `Clip export failed: ${error.message || error}`);
    }
  });
  win.on("diagnose", () => {
    try {
      ensureSource();
      lastTimestampDiagnostics = analyzeTimestamps({ path: source.path });
      lastPipelineRows.unshift({ key: "PTS diagnostics", value: `${lastTimestampDiagnostics.packets} packets, dup ${lastTimestampDiagnostics.duplicatePts}, backward ${lastTimestampDiagnostics.backwardPts}, gaps ${lastTimestampDiagnostics.gaps}` });
      updatePipelineRows();
      updateDiagnostics();
      win.set("status", `Timestamp diagnostics found ${lastTimestampDiagnostics.anomalies.length} anomalies.`);
    } catch (error) {
      win.set("status", `Diagnostics failed: ${error.message || error}`);
    }
  });
  win.on("json-report", () => {
    console.log(JSON.stringify(reportMetrics(), null, 2));
    win.set("status", "Wrote Replay Lab JSON report to stdout");
  });

  win.show();
  await ui.run();
  closeReplay();
}
