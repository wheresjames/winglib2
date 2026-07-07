// http_video_client.js -- browser UI that live-streams an arbitrary
// network stream over WebRTC, with UI options to save the stream to a .webm
// file and to save screen captures (.png).
//
// Serves a single-page browser UI over wl2:http. The page takes a stream URL --
// RTSP (rtsp://), HLS (http://...m3u8), DASH (http://...mpd), MJPEG
// (http://.../mjpeg), or SRT (srt://) -- then receives VP8 RTP over a WebRTC
// video track. GStreamer's uridecodebin picks most source elements from the URL
// scheme; MJPEG uses an explicit multipart pipeline. Recording and
// screenshots happen entirely in the browser (MediaRecorder on the received
// MediaStream, and a <video>->canvas frame grab) so the server never writes to
// the user's filesystem -- the "save" buttons trigger browser downloads.
//
// The WebRTC work is done by the wl2:webrtc SignalingHub (server) and the served
// wl2-webrtc-client.js library (browser); this app only mints auth tickets and
// builds a GStreamer pipeline per session (uridecodebin -> vp8enc).
//
// Run from the repository root, for example:
//   ./build/bin/wl2 run \
//     --allow-declared \
//     ./examples/js/scripts/http_video_client.js -- --port=8080
//
// Then open the printed http://127.0.0.1:<port>/ URL in a browser on the same
// machine and enter a stream URL, e.g. rtsp://127.0.0.1:8554/stream or
// http://127.0.0.1:8080/hls/stream.m3u8 -- or use the Test Pattern source (no
// protocol output needed). The companion http_video_server.js serves all
// of these protocols from its "Protocol output" panel with a copyable URL.

/* wl2
permissions:
  listen: ["127.0.0.1:*"]
  network: ["127.0.0.1:*"]
  sharedMemory: ["/wl2_rtsp_webrtc", "/wl2_webrtc_recv"]
  filesystemRead: ["examples/js/scripts/lib"]
*/

import { HttpServer } from "wl2:http";
import { listElements, parseLaunch, requiredElementsForUri } from "wl2:gstreamer";
import { SignalingHub, capabilities as webrtcCapabilities } from "wl2:webrtc";
import { readText } from "wl2:fs";

const HOST = "127.0.0.1";
const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
const PORT = numberArg("--port=", 8080);
const WIDTH = numberArg("--width=", 640);
const HEIGHT = numberArg("--height=", 360);
const FPS = numberArg("--fps=", 30);
const DEFAULT_CLIENT_BUFFER_SECONDS = numberArg("--client-buffer-seconds=", 6);
const RTP_CAPS = "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000";
const SHM_PREFIX = "/wl2_rtsp_webrtc";
const WEBRTC_RECV_PREFIX = "/wl2_webrtc_recv";
// The browser client library, served as a static asset. Defaults to its path
// relative to the repository root (the documented run directory).
const CLIENT_LIB_PATH = stringArg("--client-lib=", "examples/js/scripts/lib/wl2-webrtc-client.js");
// A default stream URL hint the page offers so a quick local test is one click.
// Defaults to the local full-circle URL produced by http_video_server.js
// with --serve-stream (rtsp://127.0.0.1:8554/stream). --rtsp= is kept as a
// back-compat alias for --url=.
const DEFAULT_STREAM_URL = stringArg("--url=", stringArg("--rtsp=", "rtsp://127.0.0.1:8554/stream"));

const startupPermissions = await wl2.runtime.requestPermissions({
  listen: [`${HOST}:${PORT}`],
  network: ["127.0.0.1:*"],
  sharedMemory: [SHM_PREFIX, WEBRTC_RECV_PREFIX],
});
if (!startupPermissions.granted) {
  throw new Error(`Required startup permissions were denied: ${startupPermissions.error}`);
}

function numberArg(prefix, fallback) {
  const arg = argv.find((value) => String(value).startsWith(prefix));
  if (!arg) return fallback;
  const parsed = Number(String(arg).slice(prefix.length));
  return Number.isFinite(parsed) ? parsed : fallback;
}

function stringArg(prefix, fallback) {
  const arg = argv.find((value) => String(value).startsWith(prefix));
  return arg ? String(arg).slice(prefix.length) : fallback;
}

function json(data, status = 200) {
  return {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
    body: JSON.stringify(data),
  };
}

function html(body) {
  return { status: 200, headers: { "content-type": "text/html; charset=utf-8" }, body };
}

function javascript(body, status = 200) {
  return {
    status,
    headers: { "content-type": "text/javascript; charset=utf-8", "cache-control": "no-store" },
    body,
  };
}

function gstString(value) {
  return JSON.stringify(String(value));
}

const elementCache = new Map();
function haveElement(name) {
  if (!elementCache.has(name)) {
    elementCache.set(name, listElements({ filter: name }).some((element) => element.name === name));
  }
  return elementCache.get(name);
}

// --- Pipeline error reporting ------------------------------------------------
// GStreamer posts a bus message whose `message` is generic (e.g.
// "Could not write to resource.") while the real detail lives in the last line
// of `debug` (e.g. "Error (403): Forbidden"). These helpers surface both and
// map common ingest failures — RTSP, HLS/DASH over HTTP, SRT — to a concrete,
// actionable hint shown in the UI.
function cleanBusDetail(debug) {
  if (!debug) return "";
  const lines = String(debug).split(/\r?\n/).map((line) => line.trim()).filter((line) => line.length > 0);
  return lines.length ? lines[lines.length - 1] : "";
}

function streamHint(text, url) {
  const t = text || "";
  const scheme = String(url || "").split(":")[0].toLowerCase();
  if (/Error \(403\)|Forbidden/i.test(t)) {
    return "Hint: the server refused the request (403 Forbidden). The stream is published but this client isn't allowed to play it. Verify the URL is correct and permitted (secure token / credentials / IP allowlist), and try opening the same URL in VLC to confirm the server rejects it too.";
  }
  if (/Error \(401\)|Unauthorized|WWW-Authenticate/i.test(t)) {
    return "Hint: the server requires authentication. Provide credentials in the URL, e.g. rtsp://user:password@host/path.";
  }
  if (/Error \(404\)|Not Found/i.test(t)) {
    return scheme === "http" || scheme === "https"
      ? "Hint: the URL was not found (404). For HLS/DASH check the playlist/manifest name (stream.m3u8 / stream.mpd) and that the protocol output is running."
      : "Hint: the RTSP path was not found on the server. Double-check the application and stream names in the URL.";
  }
  if (/Could not resolve|Name resolution|no such host/i.test(t)) {
    return "Hint: could not resolve the host name. Check the URL and your network/DNS.";
  }
  if (/Connection refused|could not connect|failed to connect/i.test(t)) {
    return scheme === "srt"
      ? "Hint: the SRT listener refused the connection. Check the srt:// host/port and that the protocol output is running in listener mode."
      : "Hint: the server refused the TCP connection. Check the host/port and that the protocol output is running.";
  }
  if (/closed the connection|Could not read from resource/i.test(t)) {
    return "Hint: the server closed the connection (often follows a 403/401 or a timeout). See any earlier error above.";
  }
  if (/no element|missing element|could not instantiate|not found/i.test(t)) {
    return "Hint: a required GStreamer plugin/element is missing. Install the matching gst-plugins package (rtspsrc/souphttpsrc: gst-plugins-good; hlsdemux/dashdemux/srtsrc: gst-plugins-bad).";
  }
  if (/No such file|Could not open file/i.test(t)) {
    return "Hint: the source file could not be opened. Check the path and read permissions.";
  }
  return "";
}

function describePipelineError(msg, selection) {
  const headline = String(msg.message || "GStreamer error");
  const detail = cleanBusDetail(msg.debug);
  const source = selection && selection.kind === "url" ? `stream: ${selection.url}` : "the source";
  const lines = [`Pipeline error from ${source}: ${headline}`];
  if (detail && detail !== headline) lines.push(`Server detail: ${detail}`);
  const hint = streamHint(`${headline} ${detail}`, selection && selection.url);
  if (hint) lines.push(hint);
  return lines.join("\n");
}

function describePipelineWarning(msg) {
  const headline = String(msg.message || "warning");
  const detail = cleanBusDetail(msg.debug);
  return detail && detail !== headline ? `${headline} (${detail})` : headline;
}

// A URL-source selection: { kind: "url", url } (the old { kind: "rtsp", url }
// is accepted as an alias).
function isUrlSelection(selection) {
  return selection && (selection.kind === "url" || selection.kind === "rtsp");
}

function isSegmentedUri(url) {
  const text = String(url || "").toLowerCase();
  return text.includes(".m3u8") || text.includes(".mpd");
}

function isMjpegUri(url) {
  const text = String(url || "").toLowerCase();
  return text.includes("mjpeg");
}

function sourceLaunch(selection) {
  if (isUrlSelection(selection)) {
    const url = String(selection.url || "").trim();
    if (!url) throw new Error("Enter a stream URL (e.g. rtsp://host:port/path, http://host/hls/stream.m3u8, http://host/mjpeg, srt://host:port).");
    if (isMjpegUri(url)) {
      return `souphttpsrc location=${gstString(url)} is-live=true do-timestamp=true ! multipartdemux single-stream=true ! image/jpeg,framerate=${FPS}/1 ! jpegparse ! jpegdec`;
    }
    // uridecodebin picks source elements from the scheme end to end: rtsp:// ->
    // rtspsrc, http://...m3u8 -> souphttpsrc+hlsdemux, http://...mpd ->
    // dashdemux, srt:// -> srtsrc; each followed by depayload/demux + decode.
    // Only the video stream is consumed; an audio stream, if present, is left
    // unlinked and ignored by the downstream video-only chain.
    return `uridecodebin uri=${gstString(url)}`;
  }
  return "videotestsrc is-live=true pattern=ball";
}

function createRtpPipeline(packetBufferName, selection) {
  if (!haveElement("vp8enc")) throw new Error("GStreamer element vp8enc is not installed.");
  if (!haveElement("rtpvp8pay")) throw new Error("GStreamer element rtpvp8pay is not installed.");
  if (isUrlSelection(selection)) {
    // Scheme-aware readiness: report exactly which source elements the entered
    // URL needs instead of hard-requiring rtspsrc.
    const missing = requiredElementsForUri(String(selection.url || "").trim())
      .filter((name) => !haveElement(name));
    if (missing.length) {
      throw new Error(`Missing GStreamer elements for this URL: ${missing.join(", ")} (install the matching gst-plugins package).`);
    }
  }

  const bufferSeconds = isUrlSelection(selection) && isSegmentedUri(selection.url)
    ? Math.max(0, Math.min(60, Number(selection.clientBufferSeconds) || 0))
    : 0;
  const ingestQueue = bufferSeconds > 0
    ? `queue max-size-buffers=0 max-size-bytes=0 max-size-time=${Math.round(bufferSeconds * 1000000000)} min-threshold-time=${Math.round(bufferSeconds * 1000000000)} ! identity sync=true`
    : "queue leaky=downstream max-size-buffers=4";

  const launch = [
    sourceLaunch(selection),
    ingestQueue,
    "videoconvert",
    "videoscale",
    "videorate",
    `video/x-raw,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1`,
    "vp8enc deadline=1 keyframe-max-dist=30",
    "rtpvp8pay pt=96",
    "appsink name=wl2_packet_sink sync=false drop=true max-buffers=8",
  ].join(" ! ");

  const pipeline = parseLaunch(launch);
  pipeline.attachPacketSink({
    packetBufferName,
    create: true,
    buffers: 256,
    arenaSize: 8 * 1024 * 1024,
    maxRecord: 262144,
    caps: RTP_CAPS,
  });
  pipeline.play();
  return pipeline;
}

// --- Browser client library ------------------------------------------------
// Read the reusable client at startup and serve it verbatim to the browser.
let clientLibSource = "";
try {
  const libPermission = wl2.runtime.requestPermissions({ filesystemRead: [CLIENT_LIB_PATH] });
  if (libPermission.granted) {
    clientLibSource = readText(CLIENT_LIB_PATH);
  } else {
    console.log(`client library permission denied: ${libPermission.error}`);
  }
} catch (error) {
  console.log(`could not read ${CLIENT_LIB_PATH}: ${error.message || error}`);
}
if (!clientLibSource) {
  console.log(`WARNING: client library not loaded. Pass --client-lib=<path> pointing at wl2-webrtc-client.js.`);
  clientLibSource = `throw new Error("wl2-webrtc-client.js was not found on the server. Start with --client-lib=<path>.");`;
}

// --- Signaling authentication ----------------------------------------------
// A short-lived bearer ticket minted over HTTP and presented on the signaling
// socket. This keeps the WebRTC transport gated behind the app's own auth.
//
// NOTE: for a real deployment issue a signed (HMAC) ticket so verification is
// stateless and tamper-proof, and bind it to the authenticated user. This demo
// uses an in-memory random token because the runtime ships no crypto module.
const tickets = new Map(); // token -> expiry (ms, wl2.runtime.now clock)
const TICKET_TTL_MS = 60000;
let ticketCounter = 0;

function mintTicket() {
  const now = wl2.runtime.now();
  for (const [token, expiry] of tickets) {
    if (expiry < now) tickets.delete(token);
  }
  const token = `${now.toString(36)}.${(++ticketCounter).toString(36)}.` +
    `${Math.random().toString(36).slice(2)}${Math.random().toString(36).slice(2)}`;
  tickets.set(token, now + TICKET_TTL_MS);
  return token;
}

function verifyTicket(token) {
  if (!token) return false;
  const expiry = tickets.get(token);
  if (!expiry || expiry < wl2.runtime.now()) {
    tickets.delete(token);
    return false;
  }
  return "browser"; // user id; single fixed identity for this local demo
}

// --- WebRTC signaling hub --------------------------------------------------
const hub = new SignalingHub({
  loopbackOnly: true,
  receivePacketBufferNamePrefix: WEBRTC_RECV_PREFIX,
  clientIceServers: [], // loopback-only: the browser needs no STUN/TURN
  authenticate: (hello) => verifyTicket(hello && hello.token),
  onSession: startStream,
  onError: (error) => console.log(`session error: ${error.message || error}`),
});

// Build the media pipeline for one authenticated client and attach its track.
function startStream(session, ctx) {
  const selection = ctx.request || { kind: "test" };
  const label = isUrlSelection(selection)
    ? `stream: ${selection.url}`
    : "test pattern";

  const packetBufferName = `${SHM_PREFIX}_${ctx.conn.id}_${Date.now()}`;
  const pipeline = createRtpPipeline(packetBufferName, selection);
  session.data.pipeline = pipeline;
  session.addTrack({
    media: "video",
    codec: "VP8",
    payloadType: 96,
    clockRate: 90000,
    sendPacketBufferName: packetBufferName,
  });

  // Poll the GStreamer bus alongside the WebRTC event loop, and tear the
  // pipeline down with the session. Errors carry a generic `message` plus a
  // `debug` string with the real RTSP detail; describePipelineError maps that
  // to a clear, actionable line shown in the browser.
  session.data.warned = new Set();
  session.onPump((s) => {
    for (const message of pipeline.busPoll({ timeoutMs: 0, max: 16 })) {
      if (message.type === "error") {
        s.notify("error", describePipelineError(message, selection));
        s.close();
        return;
      }
      if (message.type === "warning") {
        const text = describePipelineWarning(message);
        if (!session.data.warned.has(text)) {
          session.data.warned.add(text);
          s.notify("info", `GStreamer warning: ${text}`);
        }
      }
      if (message.type === "eos") { s.notify("info", "End of stream."); s.close(); return; }
    }
  });
  session.onClose(() => { try { pipeline.close(); } catch {} });
  session.notify("info", `Starting ${label}`);
  if (isUrlSelection(selection) && isSegmentedUri(selection.url) && Number(selection.clientBufferSeconds) > 0) {
    session.notify("info", `Client buffer: ${Number(selection.clientBufferSeconds)} seconds`);
  }
}

const page = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>wl2 HTTP video client</title>
  <style>
    :root { color-scheme: dark; font-family: Inter, ui-sans-serif, system-ui, sans-serif; }
    body { margin: 0; background: #111315; color: #eef2f3; }
    main { max-width: 1120px; min-height: 100vh; box-sizing: border-box; margin: 0 auto; padding: 24px; display: grid; grid-template-columns: 340px 1fr; grid-template-rows: minmax(0, 1fr) 190px; gap: 20px; }
    h1 { margin: 0 0 16px; font-size: 24px; font-weight: 650; }
    label { display: block; font-size: 13px; font-weight: 620; margin: 14px 0 6px; color: #b9c4c9; }
    select, input { width: 100%; box-sizing: border-box; padding: 9px 10px; border: 1px solid #3a4449; border-radius: 6px; background: #171b1e; color: #eef2f3; }
    select:focus, input:focus { outline: 2px solid #4fa3d8; outline-offset: 1px; border-color: #4fa3d8; }
    input::placeholder { color: #758188; }
    button { border: 1px solid #59a9da; border-radius: 6px; background: #1b78ad; color: #f7fbfc; padding: 9px 12px; font-weight: 650; cursor: pointer; }
    button:hover:not(:disabled) { background: #2388c0; }
    button.secondary { background: #191e21; color: #dce5e8; border-color: #46535a; }
    button.secondary:hover:not(:disabled) { background: #232a2e; }
    button:disabled { opacity: .55; cursor: default; }
    .panel { background: #181c1f; border: 1px solid #30383d; border-radius: 8px; padding: 16px; box-shadow: 0 12px 32px rgba(0, 0, 0, .28); }
    .controls { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 16px; }
    .hidden { display: none !important; }
    .video-wrap { background: #050607; min-height: 360px; display: grid; place-items: center; border: 1px solid #30383d; border-radius: 8px; overflow: hidden; }
    video { width: 100%; height: 100%; max-height: 72vh; object-fit: contain; background: #050607; }
    .status { margin-top: 12px; min-height: 22px; font: 13px ui-monospace, SFMono-Regular, Menlo, monospace; color: #9fb0b8; white-space: pre-wrap; }
    .metrics { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 12px; }
    .metric { border: 1px solid #30383d; border-radius: 6px; padding: 9px; background: #181c1f; }
    .metric span { display: block; color: #93a0a6; font-size: 12px; }
    .metric strong { display: block; margin-top: 2px; font-size: 16px; color: #f2f6f7; overflow-wrap: anywhere; }
    .rec-dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; background: #c0392b; margin-right: 6px; vertical-align: middle; animation: pulse 1s infinite; }
    @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: .3; } }
    .log-panel { grid-column: 1 / -1; min-width: 0; display: flex; flex-direction: column; }
    .log-head { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 8px; }
    .log-head h2 { margin: 0; font-size: 14px; font-weight: 650; color: #dce5e8; }
    .log-actions { display: flex; gap: 8px; }
    .log-actions button { padding: 5px 8px; font-size: 12px; }
    .log-scroll { flex: 1; min-height: 0; overflow: auto; border: 1px solid #30383d; border-radius: 8px; background: #070809; }
    #log { min-width: max-content; margin: 0; padding: 10px 12px; font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; line-height: 1.45; color: #c6d1d6; white-space: pre; }
    @media (max-width: 820px) { main { grid-template-columns: 1fr; grid-template-rows: auto auto 190px; padding: 14px; } }
  </style>
</head>
<body>
  <main>
    <section class="panel">
      <h1>wl2 HTTP video client</h1>

      <label for="sourceKind">Source</label>
      <select id="sourceKind">
        <option value="url">Stream URL</option>
        <option value="test">Test pattern</option>
      </select>

      <div id="streamUrlField">
      <label for="streamUrl">Stream URL (rtsp:// http://...m3u8 http://...mpd http://.../mjpeg srt://)</label>
        <input id="streamUrl" placeholder="rtsp://127.0.0.1:8554/stream">
      </div>

      <div id="clientBufferField">
        <label for="clientBuffer">Client buffer seconds (HLS/DASH)</label>
        <input id="clientBuffer" type="number" min="0" max="60" step="1" value="${DEFAULT_CLIENT_BUFFER_SECONDS}">
      </div>

      <div class="controls">
        <button id="start">Start</button>
        <button id="stop" class="secondary" disabled>Stop</button>
      </div>

      <label>Save</label>
      <div class="controls">
        <button id="record" class="secondary" disabled>Record to file</button>
        <button id="snapshot" class="secondary" disabled>Screenshot</button>
      </div>

      <div id="status" class="status">Loading...</div>
    </section>

    <section>
      <div class="video-wrap"><video id="video" autoplay playsinline controls muted></video></div>
      <div class="metrics">
        <div class="metric"><span>Input</span><strong id="inputCard">url</strong></div>
        <div class="metric hidden" data-buffer-card><span>Client buffer</span><strong id="bufferCard">${DEFAULT_CLIENT_BUFFER_SECONDS}s</strong></div>
        <div class="metric"><span>Browser connection</span><strong id="connection">idle</strong></div>
        <div class="metric"><span>RTP packets</span><strong id="packets">0</strong></div>
      </div>
    </section>

    <section class="panel log-panel">
      <div class="log-head">
        <h2>Logs</h2>
        <div class="log-actions">
          <button id="copyLog" class="secondary" type="button">Copy</button>
          <button id="clearLog" class="secondary" type="button">Clear</button>
        </div>
      </div>
      <div class="log-scroll"><pre id="log"></pre></div>
    </section>
  </main>

  <script type="module">
    import { Wl2WebrtcClient } from "/wl2-webrtc-client.js";

    const statusEl = document.getElementById("status");
    const sourceKindEl = document.getElementById("sourceKind");
    const streamUrlFieldEl = document.getElementById("streamUrlField");
    const streamUrlEl = document.getElementById("streamUrl");
    const clientBufferFieldEl = document.getElementById("clientBufferField");
    const clientBufferEl = document.getElementById("clientBuffer");
    const startEl = document.getElementById("start");
    const stopEl = document.getElementById("stop");
    const recordEl = document.getElementById("record");
    const snapshotEl = document.getElementById("snapshot");
    const videoEl = document.getElementById("video");
    const connectionEl = document.getElementById("connection");
    const packetsEl = document.getElementById("packets");
    const inputCardEl = document.getElementById("inputCard");
    const bufferCardEl = document.getElementById("bufferCard");
    const logEl = document.getElementById("log");
    const copyLogEl = document.getElementById("copyLog");
    const clearLogEl = document.getElementById("clearLog");

    let client = null;
    let logLines = [];
    let mediaStream = null;
    let mediaRecorder = null;
    let recordedChunks = [];

    function stamp() {
      return new Date().toISOString().replace(/[:.]/g, "-");
    }

    function log(text) {
      const line = "[" + new Date().toLocaleTimeString() + "] " + String(text);
      logLines.push(line);
      if (logLines.length > 1000) logLines = logLines.slice(logLines.length - 1000);
      logEl.textContent = logLines.join("\n");
      logEl.parentElement.scrollTop = logEl.parentElement.scrollHeight;
      statusEl.textContent = String(text); // left pane shows only the latest line
    }

    function isSegmentedUrl(url) {
      const text = String(url || "").toLowerCase();
      return text.includes(".m3u8") || text.includes(".mpd");
    }

    function updateSourceFields() {
      const urlMode = sourceKindEl.value === "url";
      const segmented = urlMode && isSegmentedUrl(streamUrlEl.value.trim());
      streamUrlFieldEl.classList.toggle("hidden", !urlMode);
      clientBufferFieldEl.classList.toggle("hidden", !segmented);
      document.querySelectorAll("[data-buffer-card]").forEach((el) => el.classList.toggle("hidden", !segmented));
      inputCardEl.textContent = urlMode ? (segmented ? "HLS/DASH URL" : "stream URL") : "test";
      bufferCardEl.textContent = (Number(clientBufferEl.value) || 0) + "s";
    }

    async function loadStatus() {
      const entered = streamUrlEl.value.trim();
      const res = await fetch("/api/status" + (entered ? "?url=" + encodeURIComponent(entered) : ""));
      const data = await res.json();
      if (data.defaultUrl) streamUrlEl.placeholder = data.defaultUrl;
      log(data.ready && data.urlReady !== false
        ? "Ready."
        : data.warning || "Some required features are unavailable.");
    }

    function selection() {
      if (sourceKindEl.value === "url") {
        return {
          kind: "url",
          url: streamUrlEl.value.trim(),
          clientBufferSeconds: Number(clientBufferEl.value) || 0,
        };
      }
      return { kind: "test" };
    }

    // Re-check element readiness when the entered URL's scheme changes.
    let lastCheckedUrl = "";
    streamUrlEl.addEventListener("change", () => {
      const url = streamUrlEl.value.trim();
      if (url && url !== lastCheckedUrl) {
        lastCheckedUrl = url;
        loadStatus().catch(() => {});
      }
    });

    // --- Browser-side "save stream to file" via MediaRecorder -------------
    // The received MediaStream (VP8 WebRTC track) is recorded to a WebM blob
    // and downloaded. Recording runs entirely in the browser; the server never
    // touches the local filesystem.
    function startRecording() {
      if (!mediaStream) return;
      try {
        const mime = MediaRecorder.isTypeSupported("video/webm;codecs=vp8")
          ? "video/webm;codecs=vp8"
          : "video/webm";
        mediaRecorder = new MediaRecorder(mediaStream, { mimeType: mime });
      } catch (error) {
        log("Recording unavailable: " + (error.message || error));
        return;
      }
      recordedChunks = [];
      mediaRecorder.ondataavailable = (event) => { if (event.data && event.data.size) recordedChunks.push(event.data); };
      mediaRecorder.onstop = () => {
        const blob = new Blob(recordedChunks, { type: "video/webm" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "wl2-stream-" + stamp() + ".webm";
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        recordedChunks = [];
        log("Saved stream to file (browser download).");
      };
      mediaRecorder.start(1000); // gather chunks every second
      recordEl.innerHTML = '<span class="rec-dot"></span>Stop recording';
      recordEl.disabled = false;
      log("Recording stream...");
    }

    function stopRecording() {
      if (mediaRecorder && mediaRecorder.state !== "inactive") {
        mediaRecorder.stop();
      }
      mediaRecorder = null;
      recordEl.textContent = "Record to file";
      refreshSaveButtons();
    }

    // --- Browser-side "save screen capture" via <video> -> canvas --------
    function screenshot() {
      const w = videoEl.videoWidth || 640;
      const h = videoEl.videoHeight || 360;
      if (!w || !h) { log("No video frame yet."); return; }
      const canvas = document.createElement("canvas");
      canvas.width = w;
      canvas.height = h;
      const ctx = canvas.getContext("2d");
      ctx.drawImage(videoEl, 0, 0, w, h);
      canvas.toBlob((blob) => {
        if (!blob) { log("Screenshot failed."); return; }
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "wl2-screenshot-" + stamp() + ".png";
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        log("Saved screenshot (browser download).");
      }, "image/png");
    }

    function refreshSaveButtons() {
      const live = !!mediaStream;
      snapshotEl.disabled = !live;
      // The Record button stays enabled while live so it can be clicked again to
      // stop recording; the label (set in start/stopRecording) conveys the state.
      recordEl.disabled = !live;
    }

    async function start() {
      stop();
      startEl.disabled = true;
      stopEl.disabled = false;
      log("Requesting session...");

      // 1. Get a signaling ticket from the app's HTTP auth endpoint.
      const ticketRes = await fetch("/api/ticket", { method: "POST" });
      if (!ticketRes.ok) throw new Error("Could not obtain a session ticket.");
      const { ticket, iceServers } = await ticketRes.json();

      // 2. Hand off to the wl2:webrtc browser client. It authenticates, answers
      //    the server's offer, trickles ICE, and delivers the media track.
      client = new Wl2WebrtcClient({
        url: (location.protocol === "https:" ? "wss" : "ws") + "://" + location.host + "/signal",
        token: ticket,
        iceServers,
        onTrack: (stream) => {
          mediaStream = stream;
          videoEl.srcObject = stream;
          refreshSaveButtons();
        },
        onState: (s) => {
          connectionEl.textContent = s.connection || "idle";
          packetsEl.textContent = String(s.sentPackets || 0);
        },
        onLog: (line) => log(line),
        onClose: () => {
          if (!stopEl.disabled) log("Session closed.");
          mediaStream = null;
          stopRecording();
          refreshSaveButtons();
        },
      });
      await client.start(selection());
    }

    function stop() {
      stopRecording();
      if (client) { client.stop(); client = null; }
      videoEl.srcObject = null;
      mediaStream = null;
      startEl.disabled = false;
      stopEl.disabled = true;
      refreshSaveButtons();
      connectionEl.textContent = "idle";
      packetsEl.textContent = "0";
    }

    startEl.addEventListener("click", () => start().catch((error) => {
      log(error.message || String(error));
      stop();
    }));
    stopEl.addEventListener("click", stop);
    recordEl.addEventListener("click", () => {
      if (mediaRecorder && mediaRecorder.state === "recording") stopRecording();
      else startRecording();
    });
    snapshotEl.addEventListener("click", screenshot);
    clearLogEl.addEventListener("click", () => { logLines = []; logEl.textContent = ""; });
    copyLogEl.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(logLines.join("\n"));
        log("Copied logs to clipboard.");
      } catch (error) {
        log("Copy failed: " + (error.message || error));
      }
    });
    sourceKindEl.addEventListener("change", () => {
      updateSourceFields();
    });
    streamUrlEl.addEventListener("input", updateSourceFields);
    clientBufferEl.addEventListener("input", updateSourceFields);
    updateSourceFields();
    loadStatus().catch((error) => log(error.message || String(error)));
  </script>
</body>
</html>`;

const server = new HttpServer({ host: HOST, port: PORT, maxBodyBytes: 1 << 20 });

server.route("GET", "/", () => html(page));
server.route("GET", "/wl2-webrtc-client.js", () => javascript(clientLibSource));
server.route("POST", "/api/ticket", () => json({ ticket: mintTicket(), iceServers: [] }));
server.route("GET", "/api/status", (req) => {
  const caps = webrtcCapabilities();
  const ready = caps.media && caps.websocket !== false && haveElement("vp8enc") && haveElement("rtpvp8pay");
  // Scheme-aware readiness for the currently-entered URL (?url=...): which
  // source elements it needs and which are missing on this install.
  let urlQuery = "";
  for (const pair of String(req.query || "").split("&")) {
    const eq = pair.indexOf("=");
    if (eq > 0 && pair.slice(0, eq) === "url") {
      try { urlQuery = decodeURIComponent(pair.slice(eq + 1).replace(/\+/g, "%20")).trim(); } catch {}
      break;
    }
  }
  const checkedUrl = urlQuery || DEFAULT_STREAM_URL;
  let requiredElements = [];
  let missingElements = [];
  try {
    requiredElements = requiredElementsForUri(checkedUrl);
    missingElements = requiredElements.filter((name) => !haveElement(name));
  } catch {}
  let warning = "";
  if (!ready) warning = "Need wl2:webrtc media plus GStreamer vp8enc and rtpvp8pay.";
  else if (missingElements.length) {
    warning = `Missing elements for ${checkedUrl}: ${missingElements.join(", ")}; test pattern still works.`;
  }
  return json({
    ready,
    urlReady: ready && missingElements.length === 0,
    checkedUrl,
    requiredElements,
    missingElements,
    warning,
    dimensions: { width: WIDTH, height: HEIGHT, fps: FPS },
    defaultUrl: DEFAULT_STREAM_URL,
  });
});

server.ws("/signal", {
  maxMessageBytes: 1 << 20,
  onMessage: (conn, msg) => {
    try {
      hub.onMessage(conn, msg.text());
    } catch (error) {
      console.log(`signal error: ${error.message || error}`);
    }
  },
  onClose: (conn) => hub.onClose(conn),
});

const address = await server.listen();
console.log(`wl2 HTTP video client: http://${address.host}:${address.port}/`);
console.log(`Serving ${WIDTH}x${HEIGHT}@${FPS} VP8 RTP. Shared-memory prefix: ${SHM_PREFIX}`);
console.log(`Default stream URL hint: ${DEFAULT_STREAM_URL} (pass --url=<url> to change; rtsp://, http://...m3u8, http://...mpd, srt://)`);
