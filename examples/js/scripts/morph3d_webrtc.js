// morph3d_webrtc.js -- stream a live wl2:3d scene to browsers over WebRTC.
//
// This is morph3d.js's animated, morphing parametric surface, but instead of
// rendering into a Slint window it renders into a shared VideoBuffer, encodes it
// to VP8 RTP with GStreamer, and broadcasts it to any number of browser viewers
// over WebRTC using the wl2:webrtc SignalingHub and the wl2-webrtc-client.js
// browser library.
//
// One scene + one encoder feeds all viewers (each viewer is just another WebRTC
// sender reading the same RTP PacketBuffer). Rendering is driven on demand by the
// viewers' signaling pump ticks, gated to the target frame rate.
//
// Run from the repository root, for example:
//   ./build/bin/wl2 run \
//     --allow-declared --allow-graphics \
//     ./examples/js/scripts/morph3d_webrtc.js -- --port=8080
//
// Then open the printed http://127.0.0.1:<port>/ URL in a browser on the same
// machine and click Start.

/* wl2
permissions:
  listen: ["127.0.0.1:*"]
  network: ["127.0.0.1:*"]
  sharedMemory: ["/wl2_morph3d_webrtc", "/wl2_webrtc_recv"]
  graphics: true
  filesystemRead: ["examples/js/scripts/lib"]
*/

import { HttpServer } from "wl2:http";
import { parseLaunch, listElements } from "wl2:gstreamer";
import { SignalingHub, capabilities as webrtcCapabilities } from "wl2:webrtc";
import { readText } from "wl2:fs";
import { Scene } from "wl2:3d";
import { hasV12Surface } from "wl2:membus";

const HOST = "127.0.0.1";
const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
const PORT = numberArg("--port=", 8080);
const WIDTH = numberArg("--width=", 800);
const HEIGHT = numberArg("--height=", 450);
const FPS = numberArg("--fps=", 30);
const FRAME_INTERVAL_MS = 1000 / FPS;
const RTP_CAPS = "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000";
const SHM_PREFIX = "/wl2_morph3d_webrtc";
const WEBRTC_RECV_PREFIX = "/wl2_webrtc_recv";
const FRAME_RING = `${SHM_PREFIX}_ring`;      // scene RGBA frames
const PACKET_BUFFER = `${SHM_PREFIX}_rtp`;    // shared VP8 RTP, one encode for all viewers
const CLIENT_LIB_PATH = stringArg("--client-lib=", "examples/js/scripts/lib/wl2-webrtc-client.js");

const startupPermissions = await wl2.runtime.requestPermissions({
  listen: [`${HOST}:${PORT}`],
  network: ["127.0.0.1:*"],
  sharedMemory: [SHM_PREFIX, FRAME_RING, PACKET_BUFFER, WEBRTC_RECV_PREFIX],
  graphics: true,
});
if (!startupPermissions.granted) {
  throw new Error(`Required startup permissions were denied: ${startupPermissions.error}`);
}
if (!hasV12Surface) {
  throw new Error("wl2:3d frame streaming requires libmembus v1.2 video-surface support.");
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
  return { status, headers: { "content-type": "application/json; charset=utf-8" }, body: JSON.stringify(data) };
}
function html(body) {
  return { status: 200, headers: { "content-type": "text/html; charset=utf-8" }, body };
}
function javascript(body) {
  return { status: 200, headers: { "content-type": "text/javascript; charset=utf-8", "cache-control": "no-store" }, body };
}

const elementCache = new Map();
function haveElement(name) {
  if (!elementCache.has(name)) {
    elementCache.set(name, listElements({ filter: name }).some((element) => element.name === name));
  }
  return elementCache.get(name);
}

// --- Morphing parametric surface (from morph3d.js) -------------------------
const COLS = 25;
const ROWS = 25;

function mod(value, divisor) {
  return value - Math.floor(value / divisor) * divisor;
}

function transitionWeight(clock, start) {
  if (start < clock && start + 2 > clock) {
    const p = clock - start;
    return p > 1 ? 2 - p : p;
  }
  return 0;
}

const vertices = new Float32Array(COLS * ROWS * 3);
const indices = new Uint16Array((COLS - 1) * (ROWS - 1) * 6);
{
  let ii = 0;
  for (let y = 0; y < ROWS - 1; y++) {
    for (let x = 0; x < COLS - 1; x++) {
      const a = y * COLS + x;
      const b = y * COLS + x + 1;
      const c = (y + 1) * COLS + x + 1;
      const d = (y + 1) * COLS + x;
      indices[ii++] = a; indices[ii++] = b; indices[ii++] = c;
      indices[ii++] = a; indices[ii++] = c; indices[ii++] = d;
    }
  }
}

function updateVertices(milliseconds) {
  const pi = Math.PI;
  const pi2 = Math.PI * 2;
  const clock = mod(milliseconds / 2000, 11);
  let out = 0;
  for (let yIndex = 0; yIndex < ROWS; yIndex++) {
    for (let xIndex = 0; xIndex < COLS; xIndex++) {
      const u0 = (xIndex / (COLS - 1)) * pi2 - pi;
      const v0 = (yIndex / (ROWS - 1)) * pi2 - pi;
      let x = 0, y = 0, z = 0;

      let p = 1 > clock ? 1 - clock : transitionWeight(clock, 10);
      x += p * u0; y += p * v0;

      p = transitionWeight(clock, 0);
      x += p * Math.sin(u0); y += p * v0; z += p * Math.cos(u0);

      p = transitionWeight(clock, 1);
      x += p * Math.sin(u0) * (pi - v0) * 0.2; y += p * v0; z += p * Math.cos(u0) * (pi - v0) * 0.2;

      p = transitionWeight(clock, 2);
      x += p * Math.sin(u0) * Math.cos(v0 / 2); y += p * Math.sin(v0 / 2); z += p * Math.cos(u0) * Math.cos(v0 / 2);

      p = transitionWeight(clock, 3);
      x += p * (2 + Math.cos(v0)) * Math.cos(u0); y += p * Math.sin(v0); z += p * (2 + Math.cos(v0)) * Math.sin(u0);

      p = transitionWeight(clock, 4);
      let u = (u0 + pi) * 1.5;
      x += p * (Math.cos(u) * (u / (3 * pi) * Math.cos(v0) + 2));
      y += p * (u * Math.sin(v0) / (3 * pi));
      z += p * (Math.sin(u) * (u / (3 * pi) * Math.cos(v0) + 2));

      p = transitionWeight(clock, 5);
      u = u0 + pi;
      x += p * (0.5 * u * Math.cos(u) * (Math.cos(v0) + 1));
      y += p * (0.5 * u * Math.sin(v0));
      z += p * (0.5 * u * Math.sin(u) * (Math.cos(v0) + 1));

      p = transitionWeight(clock, 6);
      x += p * (Math.cos(u0 * 1.5) * (Math.cos(v0) + 2));
      y += p * (Math.sin(u0 * 1.5) * (Math.cos(v0) + 2));
      z += p * (Math.sin(v0) + u0);

      p = transitionWeight(clock, 7);
      u = u0 * 2;
      const a = 0.5;
      x += p * (a * (Math.cos(u) * Math.cos(v0) + 3 * Math.cos(u) * (1.5 + Math.sin(1.5 * u) / 2)));
      y += p * (a * (Math.sin(v0) + 2 * Math.cos(1.5 * u)));
      z += p * (a * (Math.sin(u) * Math.cos(v0) + 3 * Math.sin(u) * (1.5 + Math.sin(1.5 * u) / 2)));

      p = transitionWeight(clock, 8);
      u = u0 + pi;
      let v = v0 * 0.2;
      x += p * (Math.cos(u) + v * Math.cos(u / 2) * Math.cos(u));
      y += p * (Math.sin(u) + v * Math.cos(u / 2) * Math.sin(u));
      z += p * (v * Math.sin(u / 2));

      p = transitionWeight(clock, 9);
      u = u0;
      v = v0 + pi;
      x += p * ((2 + Math.cos(u / 2) * Math.sin(v) - Math.sin(u / 2) * Math.sin(2 * v)) * Math.cos(u));
      y += p * (Math.sin(u / 2) * Math.sin(v) + Math.cos(u / 2) * Math.sin(2 * v));
      z += p * ((2 + Math.cos(u / 2) * Math.sin(v) - Math.sin(u / 2) * Math.sin(2 * v)) * Math.sin(u));

      vertices[out++] = x; vertices[out++] = y; vertices[out++] = z;
    }
  }
  return clock;
}

// --- Scene + one shared VP8/RTP encoder ------------------------------------
const scene = await Scene.create({ size: [WIDTH, HEIGHT], buffers: 8 });
scene.camera.calibrate({ fovY: 45, near: 0.1, far: 1000 });
scene.camera.lookFrom([0, 6, 15], [0, 0, 0]);
scene.setAmbientLight("#555555");
scene.light({ id: "key", kind: "directional", direction: [0.35, 0.8, 0.5], color: "#ffffff", intensity: 0.9 });
scene.light({ id: "fill", kind: "directional", direction: [-0.6, 0.25, -0.5], color: "#6aa8ff", intensity: 0.35 });

updateVertices(0);
const mesh = scene.mesh({
  id: "morph-grid", vertices, indices, color: "#e8f0f8",
  dynamic: true, scale: 1.35, rotation: [-0.25, 0, 0],
});
scene.primitive("grid", { id: "floor", size: 12, divisions: 12, at: [0, -4, 0], color: "#2a3440" });
scene.publishTo(FRAME_RING);

function createEncoder() {
  if (!haveElement("vp8enc")) throw new Error("GStreamer element vp8enc is not installed.");
  if (!haveElement("rtpvp8pay")) throw new Error("GStreamer element rtpvp8pay is not installed.");
  const launch = [
    "appsrc name=wl2_video_src is-live=true format=time",
    "videoconvert", "videoscale", "videorate",
    `video/x-raw,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1`,
    "vp8enc deadline=1 keyframe-max-dist=30",
    "rtpvp8pay pt=96",
    "appsink name=wl2_packet_sink sync=false drop=true max-buffers=8",
  ].join(" ! ");
  const pipeline = parseLaunch(launch);
  pipeline.attachVideoSource({ videoBufferName: FRAME_RING });
  pipeline.attachPacketSink({
    packetBufferName: PACKET_BUFFER,
    create: true,
    buffers: 256,
    arenaSize: 8 * 1024 * 1024,
    maxRecord: 262144,
    caps: RTP_CAPS,
  });
  pipeline.play();
  return pipeline;
}
const encoder = createEncoder();

// The render loop is driven on demand by viewers' pump ticks and gated to FPS,
// so it stays at the target rate regardless of how many viewers are connected.
let spin = true;
let yaw = 0;
let frames = 0;
let lastRenderAt = 0;
const animStart = wl2.runtime.now();
const activeSessions = new Set();

// Only render/encode once at least one viewer's media track is actually open.
// Rendering during the ICE/DTLS handshake would pre-fill the RTP buffer, so the
// first pump would send a burst (risking the initial keyframe being dropped).
// Starting fresh means the browser's first packet is a real-time keyframe.
function anyTrackOpen() {
  for (const session of activeSessions) {
    try { if (session.stats().trackOpen) return true; } catch {}
  }
  return false;
}

function renderTick() {
  if (!anyTrackOpen()) return;
  const now = wl2.runtime.now();
  if (now - lastRenderAt < FRAME_INTERVAL_MS) return;
  lastRenderAt = now;
  updateVertices(now - animStart);
  if (spin) yaw += 0.018;
  mesh.updateMesh({ vertices, rotation: [-0.25, yaw, 0] });
  scene.tick(FRAME_INTERVAL_MS);
  try {
    encoder.pushVideoFrame({ latest: true });
  } catch (error) {
    console.log(`pushVideoFrame failed: ${error.message || error}`);
  }
  for (const message of encoder.busPoll({ timeoutMs: 0, max: 8 })) {
    if (message.type === "error") console.log(`encoder error: ${message.message || "GStreamer error"}`);
  }
  frames++;
}

// --- Browser client library (served as a static asset) ---------------------
let clientLibSource = "";
try {
  const libPermission = wl2.runtime.requestPermissions({ filesystemRead: [CLIENT_LIB_PATH] });
  if (libPermission.granted) clientLibSource = readText(CLIENT_LIB_PATH);
  else console.log(`client library permission denied: ${libPermission.error}`);
} catch (error) {
  console.log(`could not read ${CLIENT_LIB_PATH}: ${error.message || error}`);
}
if (!clientLibSource) {
  console.log(`WARNING: client library not loaded. Pass --client-lib=<path> to wl2-webrtc-client.js.`);
  clientLibSource = `throw new Error("wl2-webrtc-client.js was not found on the server. Start with --client-lib=<path>.");`;
}

// --- Signaling authentication ----------------------------------------------
// Short-lived bearer ticket (see http_webrtc_video_streamer.js for the same
// pattern and the note that production should use signed HMAC tickets).
const tickets = new Map();
const TICKET_TTL_MS = 60000;
let ticketCounter = 0;
function mintTicket() {
  const now = wl2.runtime.now();
  for (const [token, expiry] of tickets) if (expiry < now) tickets.delete(token);
  const token = `${now.toString(36)}.${(++ticketCounter).toString(36)}.` +
    `${Math.random().toString(36).slice(2)}${Math.random().toString(36).slice(2)}`;
  tickets.set(token, now + TICKET_TTL_MS);
  return token;
}
function verifyTicket(token) {
  if (!token) return false;
  const expiry = tickets.get(token);
  if (!expiry || expiry < wl2.runtime.now()) { tickets.delete(token); return false; }
  return "viewer";
}

// --- WebRTC signaling hub: every viewer shares the one RTP encode -----------
let viewers = 0;
const hub = new SignalingHub({
  loopbackOnly: true,
  receivePacketBufferNamePrefix: WEBRTC_RECV_PREFIX,
  clientIceServers: [],
  authenticate: (hello) => verifyTicket(hello && hello.token),
  onSession: (session) => {
    session.addTrack({
      media: "video", codec: "VP8", payloadType: 96, clockRate: 90000,
      sendPacketBufferName: PACKET_BUFFER,
    });
    // Any active viewer drives the shared render (gated to FPS); then this
    // session pumps the shared RTP buffer to its own browser.
    session.onPump(renderTick);
    activeSessions.add(session);
    viewers++;
    session.onClose(() => { activeSessions.delete(session); viewers = Math.max(0, viewers - 1); });
    session.notify("info", "Streaming live 3D scene.");
  },
  onError: (error) => console.log(`session error: ${error.message || error}`),
});

const page = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>wl2 morph3d over WebRTC</title>
  <style>
    :root { color-scheme: dark; font-family: Inter, ui-sans-serif, system-ui, sans-serif; }
    body { margin: 0; background: #0d1016; color: #eef2f3; }
    main { max-width: 1120px; min-height: 100vh; box-sizing: border-box; margin: 0 auto; padding: 24px; display: grid; grid-template-columns: 300px 1fr; grid-template-rows: minmax(0, 1fr) 180px; gap: 20px; }
    h1 { margin: 0 0 6px; font-size: 22px; font-weight: 650; }
    p.lede { margin: 0 0 14px; color: #9fb0b8; font-size: 13px; line-height: 1.5; }
    button { border: 1px solid #59a9da; border-radius: 6px; background: #1b78ad; color: #f7fbfc; padding: 9px 12px; font-weight: 650; cursor: pointer; }
    button:hover:not(:disabled) { background: #2388c0; }
    button.secondary { background: #191e21; color: #dce5e8; border-color: #46535a; }
    button.secondary:hover:not(:disabled) { background: #232a2e; }
    button:disabled { opacity: .55; cursor: default; }
    .panel { background: #141922; border: 1px solid #263041; border-radius: 8px; padding: 16px; box-shadow: 0 12px 32px rgba(0, 0, 0, .3); }
    .controls { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 16px; }
    .video-wrap { background: #05070b; min-height: 360px; display: grid; place-items: center; border: 1px solid #263041; border-radius: 8px; overflow: hidden; }
    video { width: 100%; height: 100%; max-height: 72vh; object-fit: contain; background: #05070b; }
    .status { margin-top: 12px; min-height: 20px; font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; color: #8ea6c4; white-space: pre-wrap; }
    .metrics { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 12px; }
    .metric { border: 1px solid #263041; border-radius: 6px; padding: 9px; background: #141922; }
    .metric span { display: block; color: #7d90a8; font-size: 12px; }
    .metric strong { display: block; margin-top: 2px; font-size: 16px; color: #f2f6f7; }
    .log-panel { grid-column: 1 / -1; min-width: 0; display: flex; flex-direction: column; }
    .log-head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px; }
    .log-head h2 { margin: 0; font-size: 14px; font-weight: 650; color: #dce5e8; }
    .log-scroll { flex: 1; min-height: 0; overflow: auto; border: 1px solid #263041; border-radius: 8px; background: #05070b; }
    #log { min-width: max-content; margin: 0; padding: 10px 12px; font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; line-height: 1.45; color: #b7c6dc; white-space: pre; }
    @media (max-width: 820px) { main { grid-template-columns: 1fr; grid-template-rows: auto auto 180px; padding: 14px; } }
  </style>
</head>
<body>
  <main>
    <section class="panel">
      <h1>morph3d over WebRTC</h1>
      <p class="lede">A live wl2:3d scene, rendered on the server, VP8-encoded with GStreamer, and streamed to your browser over WebRTC. All viewers share one render.</p>
      <div class="controls">
        <button id="start">Start</button>
        <button id="stop" class="secondary" disabled>Stop</button>
        <button id="spin" class="secondary">Toggle spin</button>
      </div>
      <div id="status" class="status">Idle.</div>
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
      <div class="log-head"><h2>Logs</h2></div>
      <div class="log-scroll"><pre id="log"></pre></div>
    </section>
  </main>

  <script type="module">
    import { Wl2WebrtcClient } from "/wl2-webrtc-client.js";

    const statusEl = document.getElementById("status");
    const startEl = document.getElementById("start");
    const stopEl = document.getElementById("stop");
    const spinEl = document.getElementById("spin");
    const videoEl = document.getElementById("video");
    const connectionEl = document.getElementById("connection");
    const iceEl = document.getElementById("ice");
    const packetsEl = document.getElementById("packets");
    const logEl = document.getElementById("log");

    let client = null;
    let logLines = [];
    function log(text) {
      const line = "[" + new Date().toLocaleTimeString() + "] " + String(text);
      logLines.push(line);
      if (logLines.length > 500) logLines = logLines.slice(logLines.length - 500);
      logEl.textContent = logLines.join("\n");
      logEl.parentElement.scrollTop = logEl.parentElement.scrollHeight;
      statusEl.textContent = String(text);
    }

    async function start() {
      stop();
      startEl.disabled = true;
      stopEl.disabled = false;
      log("Requesting session...");
      const ticketRes = await fetch("/api/ticket", { method: "POST" });
      if (!ticketRes.ok) throw new Error("Could not obtain a session ticket.");
      const { ticket, iceServers } = await ticketRes.json();
      client = new Wl2WebrtcClient({
        url: (location.protocol === "https:" ? "wss" : "ws") + "://" + location.host + "/signal",
        token: ticket,
        iceServers,
        onTrack: (stream) => {
          videoEl.srcObject = stream;
          // Muted autoplay usually works, but some browsers/settings block it.
          // Fall back to a click-to-play prompt if play() is rejected.
          videoEl.play().catch(() => {
            log("Autoplay was blocked — click the video to start playback.");
            const resume = () => { videoEl.play().catch(() => {}); videoEl.removeEventListener("click", resume); };
            videoEl.addEventListener("click", resume);
          });
        },
        onState: (s) => {
          connectionEl.textContent = s.connection || "idle";
          iceEl.textContent = s.ice || "idle";
          packetsEl.textContent = String(s.sentPackets || 0);
        },
        onLog: (line) => log(line),
        onClose: () => { if (!stopEl.disabled) log("Session closed."); },
      });
      await client.start({});
    }

    function stop() {
      if (client) { client.stop(); client = null; }
      videoEl.srcObject = null;
      startEl.disabled = false;
      stopEl.disabled = true;
      connectionEl.textContent = "idle";
      iceEl.textContent = "idle";
    }

    startEl.addEventListener("click", () => start().catch((error) => { log(error.message || String(error)); stop(); }));
    stopEl.addEventListener("click", stop);
    spinEl.addEventListener("click", async () => {
      try {
        const res = await fetch("/api/control", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ toggleSpin: true }) });
        const data = await res.json();
        log("Spin " + (data.spin ? "on" : "off") + ".");
      } catch (error) {
        log("Control failed: " + (error.message || error));
      }
    });
    log("Ready. Click Start to view the live scene.");
  </script>
</body>
</html>`;

const server = new HttpServer({ host: HOST, port: PORT, maxBodyBytes: 1 << 20 });

server.route("GET", "/", () => html(page));
server.route("GET", "/favicon.ico", () => ({ status: 204, headers: {}, body: "" }));
server.route("GET", "/wl2-webrtc-client.js", () => javascript(clientLibSource));
server.route("POST", "/api/ticket", () => json({ ticket: mintTicket(), iceServers: [] }));
server.route("POST", "/api/control", (req) => {
  let body = {};
  try { body = JSON.parse((req.body && req.body.text && req.body.text()) || "{}"); } catch {}
  if (body.toggleSpin) spin = !spin;
  else if (typeof body.spin === "boolean") spin = body.spin;
  return json({ spin, viewers });
});
server.route("GET", "/api/info", () => {
  const caps = webrtcCapabilities();
  const ready = caps.media && haveElement("vp8enc") && haveElement("rtpvp8pay");
  return json({ ready, width: WIDTH, height: HEIGHT, fps: FPS, viewers, frames, spin });
});

server.ws("/signal", {
  maxMessageBytes: 1 << 20,
  onMessage: (conn, msg) => {
    try { hub.onMessage(conn, msg.text()); }
    catch (error) { console.log(`signal error: ${error.message || error}`); }
  },
  onClose: (conn) => hub.onClose(conn),
});

const address = await server.listen();
console.log(`wl2:3d + wl2:webrtc morph streamer: http://${address.host}:${address.port}/`);
console.log(`Rendering ${WIDTH}x${HEIGHT}@${FPS} (${scene.metadata().renderer}); shared RTP buffer ${PACKET_BUFFER}`);
