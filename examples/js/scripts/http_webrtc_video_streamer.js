// http_webrtc_video_streamer.js -- browser UI for wl2:http + wl2:webrtc video streaming.
//
// Serves a single-page browser UI over wl2:http. The page can select a Linux
// v4l2 camera, the synthetic test pattern, or a server-side media file path,
// then receives VP8 RTP over a WebRTC video track.
//
// This example is deliberately thin: the WebRTC work is done by the wl2:webrtc
// SignalingHub (server) and the served wl2-webrtc-client.js library (browser).
// The app only mints auth tickets and builds a GStreamer pipeline per session.
//
// Run from the repository root, for example:
//   ./build/bin/wl2 run \
//     --allow-declared \
//     ./examples/js/scripts/http_webrtc_video_streamer.js -- --port=8080
//
// Then open the printed http://127.0.0.1:<port>/ URL in a browser on the same
// machine. File streaming uses a server-side path, not the browser's file input:
// browsers intentionally do not reveal local filesystem paths to web pages.

/* wl2
permissions:
  listen: ["127.0.0.1:*"]
  network: ["127.0.0.1:*"]
  sharedMemory: ["/wl2_browser_webrtc", "/wl2_webrtc_recv"]
  filesystemRead: ["${HOME}/Videos", "examples/js/scripts/lib"]
*/

import { HttpServer } from "wl2:http";
import { DeviceMonitor, listElements, parseLaunch } from "wl2:gstreamer";
import { SignalingHub, capabilities as webrtcCapabilities } from "wl2:webrtc";
import { readText } from "wl2:fs";

const HOST = "127.0.0.1";
const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
const PORT = numberArg("--port=", 8080);
const WIDTH = numberArg("--width=", 640);
const HEIGHT = numberArg("--height=", 360);
const FPS = numberArg("--fps=", 30);
const RTP_CAPS = "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000";
const SHM_PREFIX = "/wl2_browser_webrtc";
const WEBRTC_RECV_PREFIX = "/wl2_webrtc_recv";
// The browser client library, served as a static asset. Defaults to its path
// relative to the repository root (the documented run directory).
const CLIENT_LIB_PATH = stringArg("--client-lib=", "examples/js/scripts/lib/wl2-webrtc-client.js");

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
  return {
    status: 200,
    headers: { "content-type": "text/html; charset=utf-8" },
    body,
  };
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

function listVideoDevices() {
  const devices = [{ id: "test-pattern", label: "Test Pattern", device: null }];
  try {
    const monitor = DeviceMonitor.create({ classes: "Video/Source" });
    for (const dev of monitor.devices || []) {
      if (!dev.path) continue;
      devices.push({
        id: dev.path,
        label: `${dev.displayName || "Camera"} (${dev.path})`,
        device: dev.path,
      });
    }
  } catch (error) {
    console.log(`device enumeration failed: ${error.message || error}`);
  }
  return devices;
}

function sourceLaunch(selection) {
  if (selection.kind === "file") {
    const path = String(selection.path || "").trim();
    if (!path) throw new Error("Choose a server-side video file path.");
    return `filesrc location=${gstString(path)} ! decodebin`;
  }

  if (selection.kind === "camera" && selection.device) {
    return `v4l2src device=${gstString(selection.device)} do-timestamp=true`;
  }

  return "videotestsrc is-live=true pattern=ball";
}

function createRtpPipeline(packetBufferName, selection) {
  if (!haveElement("vp8enc")) throw new Error("GStreamer element vp8enc is not installed.");
  if (!haveElement("rtpvp8pay")) throw new Error("GStreamer element rtpvp8pay is not installed.");

  const launch = [
    sourceLaunch(selection),
    "queue leaky=downstream max-size-buffers=4",
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
  const label = selection.kind === "file"
    ? `file: ${selection.path}`
    : selection.device
      ? `camera: ${selection.device}`
      : "test pattern";

  if (selection.kind === "file") {
    const filePermission = wl2.runtime.requestPermissions({
      filesystemRead: [String(selection.path || "").trim()],
    });
    if (!filePermission.granted) {
      throw new Error(`File read permission denied: ${filePermission.error}`);
    }
  }

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
  // pipeline down with the session.
  session.onPump((s) => {
    for (const message of pipeline.busPoll({ timeoutMs: 0, max: 16 })) {
      if (message.type === "error") { s.notify("error", message.message || "GStreamer error"); s.close(); return; }
      if (message.type === "eos") { s.notify("info", "End of stream."); s.close(); return; }
    }
  });
  session.onClose(() => { try { pipeline.close(); } catch {} });
  session.notify("info", `Starting ${label}`);
}

const page = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>wl2 WebRTC video streamer</title>
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
    .controls { display: flex; gap: 8px; margin-top: 16px; }
    .video-wrap { background: #050607; min-height: 360px; display: grid; place-items: center; border: 1px solid #30383d; border-radius: 8px; overflow: hidden; }
    video { width: 100%; height: 100%; max-height: 72vh; object-fit: contain; background: #050607; }
    .status { margin-top: 12px; min-height: 22px; font: 13px ui-monospace, SFMono-Regular, Menlo, monospace; color: #9fb0b8; white-space: pre-wrap; }
    .metrics { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 12px; }
    .metric { border: 1px solid #30383d; border-radius: 6px; padding: 9px; background: #181c1f; }
    .metric span { display: block; color: #93a0a6; font-size: 12px; }
    .metric strong { display: block; margin-top: 2px; font-size: 16px; color: #f2f6f7; }
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
      <h1>wl2 WebRTC video streamer</h1>

      <label for="sourceKind">Source</label>
      <select id="sourceKind">
        <option value="test">Test pattern</option>
        <option value="camera">v4l camera</option>
        <option value="file">Video file path</option>
      </select>

      <label for="camera">Camera</label>
      <select id="camera"></select>

      <label for="filePath">Server-side file path</label>
      <input id="filePath" placeholder="/home/me/Videos/sample.mp4">

      <div class="controls">
        <button id="start">Start</button>
        <button id="stop" class="secondary" disabled>Stop</button>
      </div>

      <div id="status" class="status">Loading devices...</div>
    </section>

    <section>
      <div class="video-wrap"><video id="video" autoplay playsinline controls muted></video></div>
      <div class="metrics">
        <div class="metric"><span>Connection</span><strong id="connection">idle</strong></div>
        <div class="metric"><span>ICE</span><strong id="ice">idle</strong></div>
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
    const cameraEl = document.getElementById("camera");
    const sourceKindEl = document.getElementById("sourceKind");
    const filePathEl = document.getElementById("filePath");
    const startEl = document.getElementById("start");
    const stopEl = document.getElementById("stop");
    const videoEl = document.getElementById("video");
    const connectionEl = document.getElementById("connection");
    const iceEl = document.getElementById("ice");
    const packetsEl = document.getElementById("packets");
    const logEl = document.getElementById("log");
    const copyLogEl = document.getElementById("copyLog");
    const clearLogEl = document.getElementById("clearLog");

    let client = null;
    let logLines = [];

    function log(text) {
      const line = "[" + new Date().toLocaleTimeString() + "] " + String(text);
      logLines.push(line);
      if (logLines.length > 1000) logLines = logLines.slice(logLines.length - 1000);
      logEl.textContent = logLines.join("\n");
      logEl.parentElement.scrollTop = logEl.parentElement.scrollHeight;
      // The left pane shows only the latest line; full history lives in Logs.
      statusEl.textContent = String(text);
    }

    async function loadDevices() {
      const res = await fetch("/api/devices");
      const data = await res.json();
      cameraEl.innerHTML = "";
      for (const device of data.devices) {
        const option = document.createElement("option");
        option.value = device.device || "";
        option.textContent = device.label;
        cameraEl.appendChild(option);
      }
      log(data.ready ? "Ready." : data.warning || "Some required features are unavailable.");
    }

    function selection() {
      const kind = sourceKindEl.value;
      if (kind === "file") return { kind, path: filePathEl.value.trim() };
      if (kind === "camera") return { kind, device: cameraEl.value || null };
      return { kind: "test" };
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
        onTrack: (stream) => { videoEl.srcObject = stream; },
        onState: (s) => {
          connectionEl.textContent = s.connection || "idle";
          iceEl.textContent = s.ice || "idle";
          packetsEl.textContent = String(s.sentPackets || 0);
        },
        onLog: (line) => log(line),
        onClose: () => { if (!stopEl.disabled) log("Session closed."); },
      });
      await client.start(selection());
    }

    function stop() {
      if (client) { client.stop(); client = null; }
      videoEl.srcObject = null;
      startEl.disabled = false;
      stopEl.disabled = true;
      connectionEl.textContent = "idle";
      iceEl.textContent = "idle";
    }

    startEl.addEventListener("click", () => start().catch((error) => {
      log(error.message || String(error));
      stop();
    }));
    stopEl.addEventListener("click", stop);
    clearLogEl.addEventListener("click", () => { logLines = []; logEl.textContent = ""; });
    copyLogEl.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(logLines.join("\n"));
        log("Copied logs to clipboard.");
      } catch (error) {
        log("Copy failed: " + (error.message || error));
      }
    });
    loadDevices().catch((error) => log(error.message || String(error)));
  </script>
</body>
</html>`;

const server = new HttpServer({ host: HOST, port: PORT, maxBodyBytes: 1 << 20 });

server.route("GET", "/", () => html(page));
server.route("GET", "/wl2-webrtc-client.js", () => javascript(clientLibSource));
server.route("POST", "/api/ticket", () => json({ ticket: mintTicket(), iceServers: [] }));
server.route("GET", "/api/devices", () => {
  const caps = webrtcCapabilities();
  const ready = caps.media && caps.websocket !== false && haveElement("vp8enc") && haveElement("rtpvp8pay");
  return json({
    ready,
    warning: ready ? "" : "Need wl2:webrtc media plus GStreamer vp8enc and rtpvp8pay.",
    dimensions: { width: WIDTH, height: HEIGHT, fps: FPS },
    devices: listVideoDevices(),
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
console.log(`wl2:http + wl2:webrtc video streamer: http://${address.host}:${address.port}/`);
console.log(`Serving ${WIDTH}x${HEIGHT}@${FPS} VP8 RTP. Shared-memory prefix: ${SHM_PREFIX}`);
