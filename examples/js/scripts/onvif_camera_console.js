// onvif_camera_console.js -- browser-based ONVIF discovery, viewing, and PTZ.
//
// Run from the repository root:
//   ./build/bin/wl2 run --allow-declared \
//     ./examples/js/scripts/onvif_camera_console.js -- --port=8080
//
// Open http://127.0.0.1:8080/. Enter an explicit local CIDR such as
// 192.168.1.0/24. The host cap prevents accidentally scanning a large network.

/* wl2
permissions:
  listen: ["127.0.0.1:*", "0.0.0.0:0"]
  network: ["*:*"]
  sharedMemory: ["/wl2_onvif_console", "/wl2_webrtc_recv"]
  filesystemRead: ["examples/js/scripts/lib"]
*/

import { HttpServer } from "wl2:http";
import { parseLaunch, listElements } from "wl2:gstreamer";
import { SignalingHub, capabilities as webrtcCapabilities } from "wl2:webrtc";
import { readText } from "wl2:fs";
import { connect, discover } from "wl2:onvif";
import { localNetworks } from "wl2:uv";

const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
const HOST = "127.0.0.1";
const PORT = numberArg("--port=", 8080);
const MAXIMUM_HOSTS = numberArg("--maximum-hosts=", 256);
const WIDTH = numberArg("--width=", 1280);
const HEIGHT = numberArg("--height=", 720);
const FPS = numberArg("--fps=", 24);
const SHM_PREFIX = "/wl2_onvif_console";
const RECV_PREFIX = "/wl2_webrtc_recv";
const CLIENT_LIB = "examples/js/scripts/lib/wl2-webrtc-client.js";
const RTP_CAPS = "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000";

// Ask for all resources up front so startup fails clearly instead of failing
// halfway through discovery or video playback.
const permissions = wl2.runtime.requestPermissions({
  listen: [`${HOST}:${PORT}`, "0.0.0.0:0"],
  network: ["*:*"],
  sharedMemory: [SHM_PREFIX, RECV_PREFIX],
  filesystemRead: [CLIENT_LIB],
});
if (!permissions.granted) {
  throw new Error(`Required permissions were denied: ${permissions.error}`);
}

function numberArg(prefix, fallback) {
  const value = argv.find((item) => String(item).startsWith(prefix));
  if (!value) return fallback;
  const parsed = Number(String(value).slice(prefix.length));
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function response(data, status = 200) {
  return {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
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

function javascript(body) {
  return {
    status: 200,
    headers: { "content-type": "text/javascript; charset=utf-8" },
    body,
  };
}

function bodyJson(req) {
  try {
    return JSON.parse((req.body && req.body.text && req.body.text()) || "{}");
  } catch {
    throw apiError("invalid_json", "The request body is not valid JSON.", 400);
  }
}

function queryValue(req, name) {
  for (const pair of String(req.query || "").split("&")) {
    const split = pair.indexOf("=");
    const key = decodeURIComponent(split < 0 ? pair : pair.slice(0, split));
    if (key === name) {
      return decodeURIComponent(
        split < 0 ? "" : pair.slice(split + 1).replace(/\+/g, " "),
      );
    }
  }
  return "";
}
function apiError(code, message, status = 500, detail = "") {
  const error = new Error(message);
  error.code = code;
  error.status = status;
  error.detail = detail;
  return error;
}
function errorResponse(error) {
  const code = error && error.code ? String(error.code) : "internal_error";
  const status = Number(error && error.status) || (code.includes("invalid") ? 400 : 500);
  const message = error && error.message ? String(error.message) : "Unexpected server error";
  console.log(`[${code}] ${message}${error && error.detail ? `: ${error.detail}` : ""}`);
  return response(
    {
      error: {
        code,
        message,
        detail: error && error.detail ? String(error.detail) : "",
      },
    },
    status,
  );
}

async function route(work) {
  try {
    return await work();
  } catch (error) {
    return errorResponse(error);
  }
}

// Cache plugin checks because enumerating GStreamer elements is relatively
// expensive and every status request asks the same questions.
const elements = new Map();
function haveElement(name) {
  if (!elements.has(name)) {
    elements.set(
      name,
      listElements({ filter: name }).some((item) => item.name === name),
    );
  }
  return elements.get(name);
}
function mediaReady() {
  const caps = webrtcCapabilities();
  return Boolean(
    caps.media &&
    haveElement("uridecodebin") &&
    haveElement("vp8enc") &&
    haveElement("rtpvp8pay"),
  );
}
function gstString(value) {
  return `"${String(value).replace(/\\/g, "\\\\").replace(/"/g, "\\\"")}"`;
}
function uriWithCredentials(uri, username, password) {
  if (!username) return uri;
  const match = String(uri).match(/^([a-z][a-z0-9+.-]*:\/\/)(.*)$/i);
  if (!match) return uri;
  const credentials =
    `${encodeURIComponent(username)}:${encodeURIComponent(password || "")}@`;
  return `${match[1]}${credentials}${match[2].replace(/^[^/@]+@/, "")}`;
}
function urlHostname(uri) {
  const match = String(uri).match(/^[a-z][a-z0-9+.-]*:\/\/(?:[^@/]+@)?(\[[^\]]+\]|[^:/?#]+)/i);
  return match ? match[1] : "unknown";
}
function pipelineError(message) {
  const headline = String(message.message || "GStreamer error");
  // GStreamer debug output may contain the credential-bearing RTSP URI.
  const detail = String(message.debug || "")
    .replace(
      /([a-z][a-z0-9+.-]*:\/\/)[^\s/@]+:[^\s/@]+@/gi,
      "$1[credentials]@",
    )
    .replace(/\s+/g, " ")
    .trim();

  if (/401|unauthorized|authentication/i.test(`${headline} ${detail}`)) {
    return "Camera stream authentication failed. Check the username and password.";
  }
  if (/timed out|timeout/i.test(`${headline} ${detail}`)) {
    return "The camera stream timed out. Verify its RTSP service and network path.";
  }
  if (/not found|no element|missing plugin/i.test(`${headline} ${detail}`)) {
    return "A required GStreamer RTSP or codec plugin is unavailable.";
  }
  return detail && detail !== headline ? `${headline} — ${detail}` : headline;
}

// Long-running objects are kept server-side. The browser only receives opaque
// IDs, never device handles or camera credentials.
const cameras = new Map();
let cameraCounter = 0;
const scanJobs = new Map();
let scanCounter = 0;

function networkPriority(value) {
  if (/^192\.168\./.test(value)) return 0;
  if (/^10\./.test(value)) return 1;
  if (/^172\.(1[6-9]|2\d|3[01])\./.test(value)) return 2;
  return 3;
}
function networkHosts(value) {
  const prefix = Number(String(value).split("/")[1]);
  return Number.isInteger(prefix) && prefix >= 0 && prefix <= 32
    ? 2 ** (32 - prefix)
    : Infinity;
}
let availableNetworks = [];
try {
  availableNetworks = (await localNetworks()).sort(
    (a, b) =>
      networkPriority(a) - networkPriority(b) ||
      networkHosts(a) - networkHosts(b) ||
      a.localeCompare(b),
  );
} catch (error) {
  console.log(`Local network enumeration failed: ${error.message || error}`);
}

async function scanNetworks(networks) {
  const clean = [...new Set(networks.map((value) => String(value).trim()).filter(Boolean))];
  if (!clean.length) {
    throw apiError(
      "invalid_networks",
      "Enter at least one IPv4 CIDR, for example 192.168.1.0/24.",
      400,
    );
  }
  if (clean.length > 8) {
    throw apiError(
      "invalid_networks",
      "At most eight CIDRs may be scanned at once.",
      400,
    );
  }

  const session = await discover({
    networks: clean,
    maximumHosts: MAXIMUM_HOSTS,
    timeoutMs: 15000,
  });
  const found = [];
  try {
    for await (const candidate of session) {
      if (candidate.deviceServiceUrl) found.push(candidate);
    }
  } finally {
    await session.close();
  }
  return found;
}

function startScan(networks) {
  const id = `scan-${++scanCounter}`;
  const job = { id, state: "running", devices: [], error: null, startedAt: wl2.runtime.now() };
  scanJobs.set(id, job);
  scanNetworks(networks).then(
    (devices) => {
      job.state = "complete";
      job.devices = devices;
    },
    (error) => {
      job.state = "failed";
      job.error = {
        code: String(error.code || "scan_failed"),
        message: String(error.message || error),
      };
    },
  );
  // Keep a small bounded history so abandoned browser jobs cannot accumulate.
  if (scanJobs.size > 32) {
    for (const [key, value] of scanJobs) {
      if (value.state !== "running") scanJobs.delete(key);
      if (scanJobs.size <= 24) break;
    }
  }
  return job;
}

async function openCamera(input) {
  const url = String(input.deviceServiceUrl || "").trim();
  if (!/^https?:\/\//i.test(url)) {
    throw apiError(
      "invalid_device_url",
      "A valid ONVIF Device service URL is required.",
      400,
    );
  }
  const username = String(input.username || "");
  const password = String(input.password || "");
  const device = await connect(url, {
    credentials: username ? { username, password } : undefined,
    timeoutMs: 8000,
  });
  try {
    const [information, profiles] = await Promise.all([
      device.getDeviceInformation({ timeoutMs: 8000 }),
      device.media.getProfiles({ timeoutMs: 8000 }),
    ]);
    if (!profiles.length) {
      throw apiError(
        "no_media_profiles",
        "The camera did not report any Media profiles.",
        422,
      );
    }
    const profile = profiles[0];
    const media = await device.media.getStreamUri(profile.token, { timeoutMs: 8000 });
    let ptz = false;
    let ptzCapabilities = { pan: false, tilt: false, zoom: false };
    try {
      const spaces = await device.ptz.getCapabilities(profile.token, { timeoutMs: 5000 });
      ptzCapabilities = {
        // This console sends relativeMove commands, so only the relative spaces
        // describe whether these buttons are genuinely supported.
        pan: spaces.relativePanTilt === true,
        tilt: spaces.relativePanTilt === true,
        zoom: spaces.relativeZoom === true,
      };
      ptz = ptzCapabilities.pan || ptzCapabilities.tilt || ptzCapabilities.zoom;
    } catch {}
    const id = `camera-${++cameraCounter}`;
    const camera = {
      id,
      device,
      username,
      password,
      profileToken: profile.token,
      streamUri: media.uri,
      ptz,
      ptzCapabilities,
      name: information.model || information.manufacturer || `Camera ${cameraCounter}`,
      manufacturer: information.manufacturer || "",
      model: information.model || "",
      firmware: information.firmwareVersion || "",
      deviceServiceUrl: url,
      address: urlHostname(url),
    };
    cameras.set(id, camera);
    return publicCamera(camera);
  } catch (error) { try { await device.close(); } catch {} throw error; }
}

// Return only browser-safe metadata. In particular, omit credentials, the
// stream URI, and the live ONVIF device object.
function publicCamera(camera) {
  return { id: camera.id, name: camera.name, manufacturer: camera.manufacturer, model: camera.model,
    firmware: camera.firmware, address: camera.address, deviceServiceUrl: camera.deviceServiceUrl,
    ptz: camera.ptz, ptzCapabilities: camera.ptzCapabilities };
}

async function ptzCommand(camera, input) {
  if (!camera.ptz) {
    throw apiError(
      "ptz_unavailable",
      "This camera profile does not advertise PTZ support.",
      409,
    );
  }
  const feature = input.direction === "left" || input.direction === "right" ? "pan" :
    input.direction === "up" || input.direction === "down" ? "tilt" :
    input.direction === "zoomIn" || input.direction === "zoomOut" ? "zoom" : null;
  if (feature && !camera.ptzCapabilities[feature])
    throw apiError(
      "ptz_feature_unavailable",
      `This camera profile does not advertise ${feature} support.`,
      409,
    );
  const speed = Math.max(0.03, Math.min(0.25, Number(input.speed) || 0.12));
  const vector = {};
  if (input.direction === "left") vector.pan = -speed;
  else if (input.direction === "right") vector.pan = speed;
  else if (input.direction === "up") vector.tilt = speed;
  else if (input.direction === "down") vector.tilt = -speed;
  else if (input.direction === "zoomIn") vector.zoom = speed;
  else if (input.direction === "zoomOut") vector.zoom = -speed;
  else if (input.direction === "stop") {
    await camera.device.ptz.stop(camera.profileToken, { timeoutMs: 3000 });
    return;
  }
  else throw apiError("invalid_ptz_direction", "Unknown PTZ direction.", 400);
  await camera.device.ptz.relativeMove(camera.profileToken, vector, { timeoutMs: 3000 });
}

// Convert the camera's RTSP stream to VP8/RTP packets consumed by the WebRTC
// signaling session below.
function createCameraPipeline(camera, packetBufferName) {
  if (!mediaReady()) {
    throw apiError(
      "media_unavailable",
      "WebRTC media or required GStreamer elements are unavailable.",
      503,
    );
  }
  const privateUri = uriWithCredentials(camera.streamUri, camera.username, camera.password);
  const launch = [
    `uridecodebin uri=${gstString(privateUri)}`,
    "queue leaky=downstream max-size-buffers=4", "videoconvert", "videoscale", "videorate",
    `video/x-raw,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1`,
    "vp8enc deadline=1 keyframe-max-dist=48", "rtpvp8pay pt=96",
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

let clientLibrary = "";
try {
  clientLibrary = readText(CLIENT_LIB);
} catch (error) {
  const message = `Browser client library unavailable: ${error.message || error}`;
  clientLibrary = `throw new Error(${JSON.stringify(message)});`;
}

const tickets = new Map();
function mintTicket() {
  const now = wl2.runtime.now();
  for (const [token, expiry] of tickets) if (expiry < now) tickets.delete(token);
  const random =
    Math.random().toString(36).slice(2) +
    Math.random().toString(36).slice(2);
  const token = `${now.toString(36)}.${random}`;
  tickets.set(token, now + 60000); return token;
}
function verifyTicket(token) {
  const expiry = tickets.get(token);
  if (!expiry || expiry < wl2.runtime.now()) {
    tickets.delete(token);
    return false;
  }
  return "local-browser";
}

const hub = new SignalingHub({
  loopbackOnly: true, receivePacketBufferNamePrefix: RECV_PREFIX, clientIceServers: [],
  authenticate: (hello) => verifyTicket(hello && hello.token),
  onSession: (session, context) => {
    const camera = cameras.get(context.request && context.request.cameraId);
    if (!camera) throw apiError("camera_not_open", "Open the camera before starting video.", 404);
    const packetBuffer = `${SHM_PREFIX}_${context.conn.id}_${Date.now()}`;
    const pipeline = createCameraPipeline(camera, packetBuffer);
    session.addTrack({
      media: "video",
      codec: "VP8",
      payloadType: 96,
      clockRate: 90000,
      sendPacketBufferName: packetBuffer,
    });
    session.onPump((active) => {
      for (const message of pipeline.busPoll({ timeoutMs: 0, max: 16 })) {
        if (message.type === "error") {
          active.notify("error", pipelineError(message));
          active.close();
          return;
        }
        if (message.type === "eos") {
          active.notify("info", "The camera stream ended.");
          active.close();
          return;
        }
      }
    });
    session.onClose(() => { try { pipeline.close(); } catch {} });
    session.notify("info", `Opening ${camera.name}`);
  },
  onError: (error) => console.log(`WebRTC session: ${error.message || error}`),
});

const page = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ONVIF Camera Console</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: Inter, ui-sans-serif, system-ui, sans-serif;
      --bg: #0b1018;
      --panel: #121a26;
      --line: #263449;
      --text: #edf4ff;
      --muted: #91a1b8;
      --accent: #42b8a5;
      --danger: #ff6b73;
    }

    * {
      box-sizing: border-box;
    }

    body {
      margin: 0;
      background:
        radial-gradient(circle at 80% 0, #172b3d 0, transparent 35%),
        var(--bg);
      color: var(--text);
    }

    header {
      height: 72px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0 28px;
      border-bottom: 1px solid var(--line);
      background: #0d141ee6;
      backdrop-filter: blur(16px);
    }

    .brand {
      display: flex;
      gap: 12px;
      align-items: center;
    }

    .mark {
      width: 36px;
      height: 36px;
      border-radius: 10px;
      background: linear-gradient(135deg, #54d7be, #287ea7);
      display: grid;
      place-items: center;
      font-weight: 800;
    }

    .brand h1 {
      font-size: 18px;
      margin: 0;
    }

    .brand small,
    .muted {
      color: var(--muted);
    }

    main {
      display: grid;
      grid-template-columns: 340px minmax(0, 1fr);
      gap: 18px;
      padding: 18px;
      height: calc(100vh - 72px);
    }

    .panel {
      background: linear-gradient(180deg, #151f2d, #101722);
      border: 1px solid var(--line);
      border-radius: 14px;
      box-shadow: 0 18px 50px #0005;
    }

    .sidebar {
      display: flex;
      flex-direction: column;
      min-height: 0;
    }

    .scan {
      padding: 18px;
      border-bottom: 1px solid var(--line);
    }

    label {
      display: block;
      font-size: 12px;
      font-weight: 700;
      color: #b6c2d3;
      margin-bottom: 7px;
    }

    input,
    select {
      width: 100%;
      padding: 11px 12px;
      border: 1px solid #33445b;
      border-radius: 9px;
      background: #0b121c;
      color: var(--text);
      outline: none;
    }

    input:focus,
    select:focus {
      border-color: var(--accent);
      box-shadow: 0 0 0 3px #42b8a522;
    }

    .row {
      display: flex;
      gap: 8px;
      margin-top: 10px;
    }

    button {
      border: 1px solid #369f91;
      background: #208f80;
      color: white;
      border-radius: 9px;
      padding: 10px 13px;
      font-weight: 750;
      cursor: pointer;
    }

    button:hover:not(:disabled) {
      filter: brightness(1.12);
    }

    button.secondary {
      background: #182334;
      border-color: #34465e;
      color: #d7e2f1;
    }

    button:disabled {
      opacity: 0.45;
      cursor: default;
    }

    .devices {
      padding: 10px;
      overflow: auto;
    }

    .device {
      width: 100%;
      text-align: left;
      background: transparent;
      border: 1px solid transparent;
      padding: 13px;
      margin: 2px 0;
      color: var(--text);
    }

    .device:hover,
    .device.active {
      background: #1a2838;
      border-color: #354a63;
    }

    .device strong,
    .device span {
      display: block;
    }

    .device span {
      font-size: 12px;
      color: var(--muted);
      margin-top: 4px;
    }

    .empty {
      padding: 30px 16px;
      text-align: center;
      color: var(--muted);
    }

    .content {
      display: grid;
      grid-template-rows: minmax(300px, 1fr) auto 150px;
      gap: 14px;
      min-width: 0;
      min-height: 0;
    }

    .viewer {
      position: relative;
      overflow: hidden;
      background: #030609;
    }

    .viewer video {
      width: 100%;
      height: 100%;
      object-fit: contain;
    }

    .overlay {
      position: absolute;
      left: 16px;
      top: 16px;
      padding: 8px 11px;
      background: #07101bd9;
      border: 1px solid #314258;
      border-radius: 8px;
      font-size: 12px;
    }

    .setup {
      position: absolute;
      inset: 0;
      display: grid;
      place-items: center;
      background: #07101bd9;
    }

    .card {
      width: min(420px, 90%);
      padding: 22px;
    }

    .card h2 {
      margin: 0 0 6px;
    }

    .card .row button {
      flex: 1;
    }

    .details {
      display: grid;
      grid-template-columns: 1fr auto;
      gap: 14px;
      padding: 15px 18px;
      align-items: center;
    }

    .details h2 {
      font-size: 17px;
      margin: 0 0 4px;
    }

    .ptz-controls {
      display: flex;
      align-items: center;
      gap: 14px;
    }

    .ptz-options {
      display: grid;
      gap: 7px;
    }

    .ptz-options label {
      display: flex;
      align-items: center;
      gap: 7px;
      margin: 0;
      font-weight: 600;
      white-space: nowrap;
    }

    .ptz-options input {
      width: auto;
      margin: 0;
      accent-color: var(--accent);
    }

    .ptz {
      display: grid;
      grid-template-columns: repeat(3, 44px);
      grid-template-rows: repeat(3, 40px);
      gap: 5px;
    }

    .ptz button {
      padding: 0;
      font-size: 17px;
    }

    .ptz .up {
      grid-column: 2;
    }

    .ptz .left {
      grid-column: 1;
      grid-row: 2;
    }

    .ptz .stop {
      grid-column: 2;
      grid-row: 2;
      background: #26364a;
    }

    .ptz .right {
      grid-column: 3;
      grid-row: 2;
    }

    .ptz .down {
      grid-column: 2;
      grid-row: 3;
    }

    .zoom {
      display: flex;
      gap: 5px;
    }

    .log {
      padding: 12px 15px;
      overflow: auto;
      font: 12px ui-monospace, monospace;
      color: #b9c7d8;
      white-space: pre-wrap;
    }

    .error {
      color: var(--danger);
    }

    .hidden {
      display: none !important;
    }

    @media (max-width: 820px) {
      header {
        padding: 0 14px;
      }

      main {
        grid-template-columns: 1fr;
        height: auto;
      }

      .sidebar {
        max-height: 420px;
      }

      .content {
        grid-template-rows: 380px auto 150px;
      }
    }
  </style>
</head>
<body>
  <header>
    <div class="brand">
      <div class="mark">O</div>
      <div>
        <h1>ONVIF Camera Console</h1>
        <small>Discovery · Live video · PTZ</small>
      </div>
    </div>
    <small id="health">Checking media…</small>
  </header>

  <main>
    <aside class="panel sidebar">
      <div class="scan">
        <label for="networkSelect">Available local networks</label>
        <select id="networkSelect"></select>

        <div id="customNetwork" class="hidden">
          <label for="networks" style="margin-top: 10px">
            Custom IPv4 CIDRs
          </label>
          <input
            id="networks"
            placeholder="192.168.10.0/24, 10.0.0.0/24"
          >
        </div>

        <div class="row">
          <button id="scan">Scan network</button>
          <button id="clear" class="secondary">Clear</button>
        </div>
      </div>

      <div id="devices" class="devices">
        <div class="empty">
          Choose a local network to find ONVIF cameras.
        </div>
      </div>
    </aside>

    <section class="content">
      <div class="panel viewer">
        <video id="video" autoplay playsinline muted controls></video>
        <div id="overlay" class="overlay">No camera selected</div>

        <div id="setup" class="setup hidden">
          <div class="panel card">
            <h2 id="setupTitle">Open camera</h2>
            <p class="muted">
              Credentials stay on this server and are never sent back to the
              browser.
            </p>
            <label>Username</label>
            <input id="username" autocomplete="username">
            <label style="margin-top: 10px">Password</label>
            <input
              id="password"
              type="password"
              autocomplete="current-password"
            >
            <div class="row">
              <button id="open">Open & view</button>
              <button id="cancel" class="secondary">Cancel</button>
            </div>
          </div>
        </div>
      </div>

      <div class="panel details">
        <div>
          <h2 id="cameraName">Select a camera</h2>
          <div id="cameraMeta" class="muted">
            Device details will appear here.
          </div>
        </div>

        <div class="ptz-controls">
          <div id="ptzOptions" class="ptz-options hidden">
            <label>
              <input id="reversePan" type="checkbox"> Reverse pan
            </label>
            <label>
              <input id="reverseTilt" type="checkbox"> Reverse tilt
            </label>
          </div>

          <div id="ptz" class="ptz hidden">
            <button class="up" data-dir="up">↑</button>
            <button class="left" data-dir="left">←</button>
            <button class="stop" data-dir="stop">■</button>
            <button class="right" data-dir="right">→</button>
            <button class="down" data-dir="down">↓</button>
          </div>

          <div id="zoom" class="zoom hidden">
            <button data-dir="zoomOut">−</button>
            <button data-dir="zoomIn">＋</button>
          </div>
        </div>
      </div>

      <div id="log" class="panel log">Ready.</div>
    </section>
  </main>

<script type="module">
import { Wl2WebrtcClient } from "/wl2-webrtc-client.js";

const $ = (id) => document.getElementById(id);
const devices = $("devices");
const logEl = $("log");
const video = $("video");
const setup = $("setup");
const networkSelect = $("networkSelect");

let candidates = [];
let selected = null;
let opened = null;
let client = null;

// Escape values before inserting camera-provided strings into HTML.
function esc(value) {
  return String(value || "").replace(
    /[&<>"']/g,
    (character) => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      '"': "&quot;",
      "'": "&#39;",
    })[character],
  );
}

function log(message, error = false) {
  const timestamp = new Date().toLocaleTimeString();
  logEl.textContent = "[" + timestamp + "] " + message + "\n" + logEl.textContent.slice(0, 6000);
  logEl.classList.toggle("error", error);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: { "content-type": "application/json" },
    ...options,
  });
  const data = await response.json();

  if (!response.ok) {
    const error = data.error || {};
    throw new Error(error.message + (error.detail ? " — " + error.detail : ""));
  }
  return data;
}

function renderDevices() {
  if (!candidates.length) {
    devices.innerHTML =
      '<div class="empty">No ONVIF devices found. Check the CIDR, ' +
      'permissions, and camera network.</div>';
    return;
  }

  devices.innerHTML = candidates.map((device, index) =>
    '<button class="device ' + (selected === device ? "active" : "") +
      '" data-index="' + index + '">' +
      '<strong>' + esc(device.address || "ONVIF device") + '</strong>' +
      '<span>' + esc(device.evidence || "") + ' · ' +
        esc(device.deviceServiceUrl || "") + '</span>' +
    '</button>'
  ).join("");

  devices.querySelectorAll(".device").forEach((button) => {
    button.onclick = () => chooseCamera(candidates[Number(button.dataset.index)]);
  });
}

function chooseCamera(camera) {
  selected = camera;
  renderDevices();
  $("setupTitle").textContent = "Open " + camera.address;
  setup.classList.remove("hidden");
  $("username").focus();
}

function preferenceKey(axis) {
  return "wl2-onvif:" + opened.deviceServiceUrl + ":" + axis;
}

function loadPtzPreferences() {
  for (const [id, axis] of [["reversePan", "pan"], ["reverseTilt", "tilt"]]) {
    try {
      $(id).checked = localStorage.getItem(preferenceKey(axis)) === "true";
    } catch {
      $(id).checked = false;
    }
  }
}

function savePtzPreference(id, axis) {
  try {
    localStorage.setItem(preferenceKey(axis), String($(id).checked));
  } catch {
    // The console still works when the browser blocks local storage.
  }
}

async function startVideo(camera) {
  if (client) {
    client.stop();
    client = null;
  }

  const ticket = await api("/api/ticket", { method: "POST", body: "{}" });
  const websocketProtocol = location.protocol === "https:" ? "wss" : "ws";

  client = new Wl2WebrtcClient({
    url: websocketProtocol + "://" + location.host + "/signal",
    token: ticket.ticket,
    onTrack: (stream) => {
      video.srcObject = stream;
      video.play().catch((error) => {
        log("Browser playback failed: " + error.message, true);
      });
    },
    onLog: (message) => log(message),
    onState: (state) => {
      $("overlay").textContent =
        (state.trackOpen ? "Live" : "Connecting") + " · " + camera.address;
    },
    onClose: () => {
      $("overlay").textContent = "Disconnected · " + camera.address;
    },
  });

  await client.start({ cameraId: camera.id });
  $("overlay").textContent = "Connecting · " + camera.address;
}

function selectedNetworks() {
  return networkSelect.value === "custom"
    ? $("networks").value.split(",")
    : [networkSelect.value];
}

// Discovery runs as a server-side job, so poll until its terminal state.
async function waitForScan(id) {
  for (;;) {
    await new Promise((resolve) => setTimeout(resolve, 750));
    const job = await api("/api/scan/status?id=" + encodeURIComponent(id));

    if (job.state === "complete") return job.devices;
    if (job.state === "failed") {
      throw new Error(job.error?.message || "The scan failed.");
    }
  }
}

$("scan").onclick = async () => {
  try {
    $("scan").disabled = true;
    log("Scanning local network… This may take 15 seconds.");
    const started = await api("/api/scan/start", {
      method: "POST",
      body: JSON.stringify({ networks: selectedNetworks() }),
    });
    candidates = await waitForScan(started.id);
    selected = null;
    renderDevices();
    log(
      "Scan complete: " + candidates.length + " device" +
      (candidates.length === 1 ? "" : "s") + " found."
    );
  } catch (error) {
    log("Scan failed: " + error.message, true);
  } finally {
    $("scan").disabled = false;
  }
};

$("clear").onclick = () => {
  candidates = [];
  selected = null;
  renderDevices();
};

$("cancel").onclick = () => setup.classList.add("hidden");

$("open").onclick = async () => {
  try {
    $("open").disabled = true;
    log("Authenticating and reading camera capabilities…");
    opened = await api("/api/camera/open", {
      method: "POST",
      body: JSON.stringify({
        deviceServiceUrl: selected.deviceServiceUrl,
        username: $("username").value,
        password: $("password").value,
      }),
    });

    $("password").value = "";
    setup.classList.add("hidden");
    $("cameraName").textContent = opened.name;
    $("cameraMeta").textContent = [
      opened.manufacturer,
      opened.model,
      opened.firmware,
      opened.address,
    ].filter(Boolean).join(" · ");

    for (const id of ["ptz", "zoom", "ptzOptions"]) {
      $(id).classList.toggle("hidden", !opened.ptz);
    }
    loadPtzPreferences();

    const features = opened.ptzCapabilities || {};
    $("reversePan").disabled = !features.pan;
    $("reverseTilt").disabled = !features.tilt;
    document.querySelectorAll("[data-dir]").forEach((button) => {
      const direction = button.dataset.dir;
      const feature = ["left", "right"].includes(direction)
        ? "pan"
        : ["up", "down"].includes(direction)
          ? "tilt"
          : ["zoomIn", "zoomOut"].includes(direction)
            ? "zoom"
            : null;

      button.disabled = feature ? !features[feature] : !opened.ptz;
      button.title = button.disabled
        ? (feature ? feature[0].toUpperCase() + feature.slice(1) : "PTZ") +
          " is not available on this camera"
        : "";
    });

    await startVideo(opened);
    log("Viewing " + opened.name + (opened.ptz ? " with available PTZ controls." : "."));
  } catch (error) {
    log("Could not open camera: " + error.message, true);
  } finally {
    $("open").disabled = false;
  }
};

let ptzBusy = false;
let stopPending = false;

// Serialize PTZ calls. If a stop arrives during a move, send it immediately
// after the in-flight request to avoid leaving the camera moving.
async function move(direction) {
  if (!opened) return;

  if ($("reversePan").checked) {
    direction = { left: "right", right: "left" }[direction] || direction;
  }
  if ($("reverseTilt").checked) {
    direction = { up: "down", down: "up" }[direction] || direction;
  }
  if (ptzBusy) {
    if (direction === "stop") stopPending = true;
    return;
  }

  ptzBusy = true;
  try {
    await api("/api/ptz", {
      method: "POST",
      body: JSON.stringify({ cameraId: opened.id, direction, speed: 0.12 }),
    });
  } catch (error) {
    log("PTZ failed: " + error.message, true);
  } finally {
    ptzBusy = false;
    if (stopPending) {
      stopPending = false;
      move("stop");
    }
  }
}

$("reversePan").onchange = () => savePtzPreference("reversePan", "pan");
$("reverseTilt").onchange = () => savePtzPreference("reverseTilt", "tilt");

document.querySelectorAll("[data-dir]").forEach((button) => {
  const direction = button.dataset.dir;
  if (direction === "stop") {
    button.onclick = () => {
      if (!button.disabled) move("stop");
    };
    return;
  }

  let repeat = null;
  const endMove = () => {
    if (repeat) {
      clearInterval(repeat);
      repeat = null;
    }
    if (!button.disabled) move("stop");
  };

  button.onpointerdown = (event) => {
    event.preventDefault();
    if (button.disabled) return;
    move(direction);
    repeat = setInterval(() => move(direction), 350);
  };
  button.onpointerup = endMove;
  button.onpointercancel = endMove;
  button.onpointerleave = endMove;
});

// Populate network choices and report media readiness on initial load.
try {
  const status = await api("/api/status");
  $("health").textContent = status.ready ? "Media ready" : "Media unavailable";
  networkSelect.innerHTML = (status.networks || []).map((network) =>
    '<option value="' + esc(network.cidr) + '" ' +
      (network.scannable ? "" : "disabled") + '>' +
      esc(network.cidr) +
      (network.scannable ? "" : " (exceeds host cap)") +
    '</option>'
  ).join("") + '<option value="custom">Custom CIDR…</option>';
  networkSelect.onchange = () => {
    $("customNetwork").classList.toggle("hidden", networkSelect.value !== "custom");
  };

  if (!(status.networks || []).some((network) => network.scannable)) {
    networkSelect.value = "custom";
    networkSelect.onchange();
  }
  if (!status.ready) log(status.warning, true);
} catch (error) {
  log("Startup check failed: " + error.message, true);
}
</script>
</body>
</html>`;

const server = new HttpServer({ host: HOST, port: PORT, maxBodyBytes: 1 << 20 });
server.route("GET", "/", () => html(page));
server.route("GET", "/favicon.ico", () => ({ status: 204, headers: {}, body: "" }));
server.route("GET", "/wl2-webrtc-client.js", () => javascript(clientLibrary));
server.route("GET", "/api/status", () => response({
  ready: mediaReady(),
  maximumHosts: MAXIMUM_HOSTS,
  networks: availableNetworks.map((cidr) => ({
    cidr,
    scannable: networkHosts(cidr) <= MAXIMUM_HOSTS,
  })),
  warning: mediaReady()
    ? ""
    : "WebRTC media, uridecodebin, vp8enc, or rtpvp8pay is unavailable.",
}));
server.route("POST", "/api/ticket", () => response({ ticket: mintTicket(), iceServers: [] }));
server.route("POST", "/api/scan/start", (req) => route(async () => {
  const job = startScan(bodyJson(req).networks || []);
  return response({ id: job.id, state: job.state }, 202);
}));
server.route("GET", "/api/scan/status", (req) => route(async () => {
  const id = queryValue(req, "id"); const job = scanJobs.get(id);
  if (!job) throw apiError("scan_not_found", "The scan job was not found or has expired.", 404);
  return response({ id: job.id, state: job.state, devices: job.devices, error: job.error });
}));
server.route("POST", "/api/camera/open", (req) => route(async () => {
  return response(await openCamera(bodyJson(req)));
}));
server.route("POST", "/api/ptz", (req) => route(async () => {
  const input = bodyJson(req); const camera = cameras.get(String(input.cameraId || ""));
  if (!camera) throw apiError("camera_not_open", "The selected camera is not open.", 404);
  await ptzCommand(camera, input); return response({ ok: true });
}));
server.ws("/signal", { maxMessageBytes: 1 << 20,
  onMessage: (conn, message) => {
    try {
      hub.onMessage(conn, message.text());
    } catch (error) {
      console.log(`Signal error: ${error.message || error}`);
    }
  },
  onClose: (conn) => hub.onClose(conn),
});

const address = await server.listen();
console.log(`ONVIF Camera Console: http://${address.host}:${address.port}/`);
console.log(`Explicit CIDR scan cap: ${MAXIMUM_HOSTS} hosts; video ${WIDTH}x${HEIGHT}@${FPS}`);
