// http_video_server.js -- browser UI for wl2:http + wl2:webrtc video streaming.
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
//     ./examples/js/scripts/http_video_server.js -- --port=8080
//
// Then open the printed http://127.0.0.1:<port>/ URL in a browser on the same
// machine. File streaming uses a server-side path, not the browser's file input:
// browsers intentionally do not reveal local filesystem paths to web pages.
//
// Protocol output (optional, for a local full circle with
// http_video_client.js): the browser UI has a "Protocol output" panel with
// a protocol dropdown (RTSP / HLS / DASH / SRT / MJPEG / WebRTC), Start/Stop
// buttons, and a copy-to-clipboard Stream URL. The server re-streams the
// currently-selected source over the chosen protocol:
//   - RTSP: a tiny in-process TCP-interleaved RTSP/RTP server (VP8), one client
//     at a time.
//   - HLS / DASH: GStreamer hlssink/dashsink writing segments into a temp
//     directory served at /hls and /dash on this HTTP port (H.264).
//   - SRT: GStreamer srtsink in listener mode (H.264 over MPEG-TS).
//   - MJPEG: a long-lived multipart/x-mixed-replace HTTP response at /mjpeg.
//   - WebRTC: the page you are already looking at; Start just surfaces the page
//     URL for copying (the consumer is a browser, not http_video_client.js).
// Pass --serve-stream (alias --serve-rtsp) to start the protocol output at
// startup with --stream-protocol=rtsp|hls|dash|srt|mjpeg (default rtsp):
//   ./build/bin/wl2 run --trust-declared \
//     ./examples/js/scripts/http_video_server.js -- \
//     --port=8080 --serve-stream --stream-protocol=rtsp --rtsp-port=8554
//   # then, in another terminal:
//   ./build/bin/wl2 run --trust-declared \
//     ./examples/js/scripts/http_video_client.js -- \
//     --port=8090 --url=rtsp://127.0.0.1:8554/stream
// --rtsp-source=test|camera|file (default test), --rtsp-camera=<dev>,
// --rtsp-file=<path>, --rtsp-host=<host> (default 127.0.0.1),
// --rtsp-port=<port> (default 8554), --rtsp-path=<path> (default /stream),
// --srt-port=<port> (default 7001), --out-dir=<dir> (HLS/DASH output root;
// default: a fresh temp dir under /tmp/wl2-stream).
// One protocol output runs at a time. Serving a real camera to both the stream
// server and a WebRTC browser session at once is not supported (the device
// can't be opened twice) -- use one consumer at a time.

/* wl2
permissions:
  listen: ["127.0.0.1:*"]
  network: ["127.0.0.1:*"]
  sharedMemory: ["/wl2_browser_webrtc", "/wl2_webrtc_recv", "/wl2_rtsp_server", "/wl2_stream_mjpeg"]
  filesystemRead: ["${HOME}/Videos", "examples/js/scripts/lib", "/tmp/wl2-stream"]
  filesystemWrite: ["/tmp/wl2-stream"]
*/

import { HttpServer } from "wl2:http";
import { DeviceMonitor, listElements, parseLaunch, buildHlsOutput, buildDashOutput, buildSrtOutput } from "wl2:gstreamer";
import { SignalingHub, capabilities as webrtcCapabilities } from "wl2:webrtc";
import { listen, setTimeout as asioSetTimeout } from "wl2:asio";
import { PacketBuffer } from "wl2:membus";
import { readText, mkdir, mkdtemp, remove } from "wl2:fs";

const sleep = (ms) => new Promise((resolve) => asioSetTimeout(resolve, ms));

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

// --- Protocol output options -------------------------------------------------
// When --serve-stream (alias --serve-rtsp) is on, the script also starts the
// protocol output at startup so http_video_client.js (or any
// stock client) can connect for a local full circle with no external tools.
// The source is chosen with --rtsp-source=test|camera|file (default test).
const SERVE_STREAM = boolArg("--serve-stream", false) || boolArg("--serve-rtsp", false);
const STREAM_PROTOCOL = stringArg("--stream-protocol=", "rtsp");
const RTSP_HOST = stringArg("--rtsp-host=", "127.0.0.1");
const RTSP_PORT = numberArg("--rtsp-port=", 8554);
const RTSP_PATH = stringArg("--rtsp-path=", "/stream");
const SRT_PORT = numberArg("--srt-port=", 7001);
const OUT_DIR = stringArg("--out-dir=", "");
const RTSP_SHM_PREFIX = "/wl2_rtsp_server";
const MJPEG_SHM_PREFIX = "/wl2_stream_mjpeg";
const STREAM_OUT_BASE = "/tmp/wl2-stream";
const RTSP_SOURCE = stringArg("--rtsp-source=", "test");
const RTSP_CAMERA = stringArg("--rtsp-camera=", "");
const RTSP_FILE = stringArg("--rtsp-file=", "");

// Always request the stream-server scopes at startup so the browser UI can
// start any protocol server on demand (not just when --serve-stream is passed).
// They are listed in the declared permissions block above, so --trust-declared
// approves them; the listen wildcard covers a UI-chosen RTSP port.
const startupPermissions = await wl2.runtime.requestPermissions({
  listen: [`${HOST}:${PORT}`, "127.0.0.1:*"],
  network: ["127.0.0.1:*"],
  sharedMemory: [SHM_PREFIX, WEBRTC_RECV_PREFIX, RTSP_SHM_PREFIX, MJPEG_SHM_PREFIX],
  filesystemRead: [OUT_DIR || STREAM_OUT_BASE],
  filesystemWrite: [OUT_DIR || STREAM_OUT_BASE],
});
if (!startupPermissions.granted) {
  throw new Error(`Required startup permissions were denied: ${startupPermissions.error}`);
}

// --- HLS/DASH output directories ---------------------------------------------
// Fixed per-run output root: static mounts must be registered before listen(),
// so the /hls and /dash directories are created up front and reused by every
// start/stop cycle (stop wipes their contents). A fresh temp root is created
// per run unless --out-dir names one.
let STREAM_OUT_ROOT = "";
try {
  if (OUT_DIR) {
    mkdir(OUT_DIR, { recursive: true });
    STREAM_OUT_ROOT = OUT_DIR;
  } else {
    mkdir(STREAM_OUT_BASE, { recursive: true });
    STREAM_OUT_ROOT = mkdtemp(`${STREAM_OUT_BASE}/out-`);
  }
} catch (error) {
  console.log(`stream output directory unavailable (HLS/DASH disabled): ${error.message || error}`);
}
const HLS_DIR = STREAM_OUT_ROOT ? `${STREAM_OUT_ROOT}/hls` : "";
const DASH_DIR = STREAM_OUT_ROOT ? `${STREAM_OUT_ROOT}/dash` : "";
if (HLS_DIR) mkdir(HLS_DIR, { recursive: true });
if (DASH_DIR) mkdir(DASH_DIR, { recursive: true });
const SEGMENT_SECONDS = 2;
const DEFAULT_SEGMENT_BUFFER_SECONDS = 12;

// Recreate a protocol output dir as an empty directory.
function resetOutputDir(dir) {
  try { remove(dir, { recursive: true }); } catch {}
  mkdir(dir, { recursive: true });
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

function boolArg(prefix, fallback) {
  const match = argv.find((value) => {
    const s = String(value);
    return s === prefix || s.startsWith(prefix + "=");
  });
  if (match === undefined) return fallback;
  const value = String(match).slice(prefix.length + 1); // skip "prefix="
  if (String(match) === prefix) return true; // bare flag form: --serve-rtsp
  return value === "1" || value === "on" || value === "true" || value === "yes";
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
  return createSourcePipeline(packetBufferName, "wl2_packet_sink", selection, {
    buffers: 256,
    arenaSize: 8 * 1024 * 1024,
    maxRecord: 262144,
    appSinkMaxBuffers: 8,
  });
}

// Persistent source pipeline for the local RTSP server. Writes VP8 RTP packets
// into its own packet buffer (consumed by the RTSP server via wl2:membus).
function createRtspPipeline(packetBufferName, selection) {
  return createSourcePipeline(packetBufferName, "rtsp_packet_sink", selection, {
    buffers: 64,
    arenaSize: 8 * 1024 * 1024,
    maxRecord: 262144,
    appSinkMaxBuffers: 64,
  });
}

function createSourcePipeline(packetBufferName, sinkName, selection, opts) {
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
    `appsink name=${sinkName} sync=false drop=true max-buffers=${opts.appSinkMaxBuffers}`,
  ].join(" ! ");

  const pipeline = parseLaunch(launch);
  pipeline.attachPacketSink({
    packetBufferName,
    elementName: sinkName,
    create: true,
    buffers: opts.buffers,
    arenaSize: opts.arenaSize,
    maxRecord: opts.maxRecord,
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

// --- Local RTSP server ------------------------------------------------------
// A minimal TCP-interleaved RTSP/RTP server (RFC 4566/5584) over wl2:asio that
// re-streams VP8 RTP packets from a membus PacketBuffer produced by an
// rtsp_packet_sink appsink. One client at a time; TCP-interleaved transport
// only. The RTP send loop is driven by socket.read timeouts (wl2 server-side JS
// has no setTimeout), which double as a ~20 Hz heartbeat.
function rtspSelection() {
  if (RTSP_SOURCE === "camera") return { kind: "camera", device: RTSP_CAMERA || null };
  if (RTSP_SOURCE === "file") return { kind: "file", path: RTSP_FILE };
  return { kind: "test" };
}

function textEncode(str) {
  // RTSP messages are ASCII; build a Uint8Array from char codes.
  const bytes = new Uint8Array(str.length);
  for (let i = 0; i < str.length; ++i) bytes[i] = str.charCodeAt(i) & 0xff;
  return bytes;
}

function rtspResponse(statusLine, cseq, headers, body) {
  const lines = [`RTSP/1.0 ${statusLine}`, `CSeq: ${cseq}`];
  for (const [key, value] of headers) lines.push(`${key}: ${value}`);
  let text = lines.join("\r\n") + "\r\n\r\n";
  if (body) text += body;
  return textEncode(text);
}

function rtspSdp(path) {
  // One VP8 video track on payload 96, control trackID=0, sendonly.
  // m= port 0 + RTP/AVP signals TCP-interleaved (the SETUP response fixes the
  // transport to RTP/AVP/TCP;unicast;interleaved=0-1).
  return [
    "v=0",
    "o=- 1 1 IN IP4 127.0.0.1",
    "s=wl2 rtsp server",
    "c=IN IP4 0.0.0.0",
    "t=0 0",
    `m=video 0 RTP/AVP 96`,
    "a=rtpmap:96 VP8/90000",
    "a=control:trackID=0",
    "a=sendonly",
    "",
  ].join("\r\n");
}

function parseRtspRequest(text) {
  const lines = text.split("\r\n");
  const first = lines.shift() || "";
  const sp = first.split(" ");
  if (sp.length < 3) return null;
  const req = { method: sp[0], uri: sp[1], version: sp[2], headers: {} };
  for (const line of lines) {
    if (!line) continue;
    const idx = line.indexOf(":");
    if (idx < 0) continue;
    const key = line.slice(0, idx).trim().toLowerCase();
    const value = line.slice(idx + 1).trim();
    req.headers[key] = value;
  }
  return req;
}

// Find the next CRLFCRLF (end of headers) in the byte buffer; returns its index
// or -1. Binary-safe (works on a number[] buffer).
function findHeaderEnd(buf, from) {
  for (let i = from; i + 3 < buf.length; ++i) {
    if (buf[i] === 13 && buf[i + 1] === 10 && buf[i + 2] === 13 && buf[i + 3] === 10) return i;
  }
  return -1;
}

async function serveRtspClient(sock, pb, path) {
  let playing = false;
  let lastSeq = 0;
  let sessionId = `${Date.now().toString(36)}.${Math.random().toString(36).slice(2, 8)}`;
  const buf = []; // accumulating byte buffer for RTSP requests / interleaved frames

  function rtpChannel() { return 0; } // RTP on interleaved channel 0; RTCP on 1 (unused).

  // Send all RTP packets newer than lastSeq as TCP-interleaved frames. The
  // membus PacketBuffer is a ring indexed by slot; record(slot) reads the record
  // currently in that slot. We walk slots backward from the latest write
  // (pointer-1), collecting records whose sequence > lastSeq, then send them in
  // ascending sequence order.
  async function drainRtp() {
    if (!playing) return;
    const meta = pb.metadata();
    const newest = meta.sequence;
    if (newest < lastSeq) { lastSeq = newest; return; } // producer restarted
    if (newest <= lastSeq) return;
    const toSend = [];
    for (let k = 0; k < meta.buffers; ++k) {
      const slot = (((meta.pointer - 1 - k) % meta.buffers) + meta.buffers) % meta.buffers;
      let rec;
      try {
        rec = pb.record(slot);
      } catch (error) {
        break; // unwritten slot -> stop walking back
      }
      const s = rec.sequence;
      if (s <= lastSeq) break;        // reached already-sent packets
      if (s > newest) continue;       // defensive: should not happen
      toSend.push({ s, payload: rec.payload });
    }
    toSend.sort((a, b) => a.s - b.s);
    for (const item of toSend) {
      const payload = item.payload && item.payload.uint8Array ? item.payload.uint8Array() : null;
      if (!payload || payload.length === 0) { lastSeq = Math.max(lastSeq, item.s); continue; }
      const frame = new Uint8Array(4 + payload.length);
      frame[0] = 0x24; // '$'
      frame[1] = rtpChannel();
      frame[2] = (payload.length >> 8) & 0xff;
      frame[3] = payload.length & 0xff;
      frame.set(payload, 4);
      try {
        await sock.write(frame);
      } catch (error) {
        throw new Error("rtsp socket write failed: " + (error && error.message ? error.message : error));
      }
      lastSeq = item.s;
    }
  }

  function handleRequest(req) {
    const cseq = req.headers["cseq"] !== undefined ? req.headers["cseq"] : "0";
    const method = req.method.toUpperCase();
    if (method === "OPTIONS") {
      return rtspResponse("200 OK", cseq, [["Public", "OPTIONS,DESCRIBE,SETUP,PLAY,TEARDOWN,GET_PARAMETER"]], "");
    }
    if (method === "DESCRIBE") {
      const sdp = rtspSdp(path);
      return rtspResponse("200 OK", cseq, [
        ["Content-Type", "application/sdp"],
        ["Content-Length", String(sdp.length)],
      ], sdp);
    }
    if (method === "SETUP") {
      // Accept any client transport; answer with TCP-interleaved (unicast).
      return rtspResponse("200 OK", cseq, [
        ["Session", `${sessionId};timeout=60`],
        ["Transport", "RTP/AVP/TCP;unicast;interleaved=0-1"],
      ], "");
    }
    if (method === "PLAY") {
      playing = true;
      const meta = pb.metadata();
      lastSeq = meta.sequence; // start streaming from the next produced packet
      return rtspResponse("200 OK", cseq, [
        ["Session", `${sessionId};timeout=60`],
        ["Range", "npt=0.000-"],
      ], "");
    }
    if (method === "TEARDOWN") {
      playing = false;
      return rtspResponse("200 OK", cseq, [["Session", `${sessionId};timeout=60`]], "");
    }
    if (method === "GET_PARAMETER") {
      return rtspResponse("200 OK", cseq, [], "");
    }
    return rtspResponse("501 Not Implemented", cseq, [], "");
  }

  try {
    while (true) {
      let chunk;
      try {
        chunk = await sock.read({ maxBytes: 4096, timeoutMs: 50 });
      } catch (error) {
        // socket.read rejects on timeout -> use it as the streaming heartbeat.
        if (error && (error.code === "asio_timeout" || /timed out/i.test(error.message || ""))) {
          try { await drainRtp(); } catch (e) { break; }
          continue;
        }
        break; // real read error -> drop the client
      }
      // 0-length read == EOF.
      const bytes = chunk && chunk.uint8Array ? chunk.uint8Array() : null;
      if (!bytes || bytes.length === 0) break;
      for (let i = 0; i < bytes.length; ++i) buf.push(bytes[i]);

      // Peel complete messages: interleaved frames (0x24-prefixed) and RTSP
      // requests (terminated by CRLFCRLF).
      let progress = true;
      while (progress) {
        progress = false;
        if (buf.length === 0) break;
        if (buf[0] === 0x24) {
          // Interleaved frame from the client (RTCP on channel 1): $<ch><len16><data>.
          if (buf.length < 4) break;
          const len = (buf[2] << 8) | buf[3];
          if (buf.length < 4 + len) break;
          buf.splice(0, 4 + len); // discard client RTCP for this demo
          progress = true;
          continue;
        }
        const hdrEnd = findHeaderEnd(buf, 0);
        if (hdrEnd < 0) break;
        const text = String.fromCharCode(...buf.slice(0, hdrEnd));
        buf.splice(0, hdrEnd + 4);
        const req = parseRtspRequest(text);
        if (req) {
          const resp = handleRequest(req);
          try { await sock.write(resp); } catch (e) { return; }
          if (req.method.toUpperCase() === "PLAY") {
            try { await drainRtp(); } catch (e) { return; }
          }
          if (req.method.toUpperCase() === "TEARDOWN") { playing = false; return; }
        }
        progress = true;
      }
      // Also push RTP while playing after handling requests.
      try { await drainRtp(); } catch (e) { break; }
    }
  } finally {
    playing = false;
    try { sock.close(); } catch {}
  }
}

// Start the RTSP server: listen, then serve clients one at a time. The source
// pipeline's bus is polled on each accept timeout so ingest errors surface.
// Protocol output state (all protocols). Mutated in place by start/stop so the
// RTSP accept loop (which captures the same object) observes running=false on
// stop. `server`/`clientSock`/`packetBufferName` are RTSP/MJPEG-specific;
// `outDir` is HLS/DASH-specific.
let streamState = {
  running: false, protocol: null, server: null, pipeline: null, clientSock: null,
  packetBufferName: null, url: null, host: null, port: null, path: null,
  outDir: null, error: null,
};

function pollPipelineBus(pipeline) {
  if (!pipeline) return null;
  try {
    for (const message of pipeline.busPoll({ timeoutMs: 0, max: 16 })) {
      if (message.type === "error") {
        const detail = message.message || "GStreamer error";
        console.log(`rtsp source error: ${detail}`);
        return detail;
      }
    }
  } catch {}
  return null;
}

// Accept loop for one RTSP server instance. Runs until state.running is false.
async function serveRtspAcceptLoop(state) {
  while (state.running) {
    let sock;
    try {
      sock = await state.server.accept({ timeoutMs: 1000 });
    } catch (error) {
      if (!state.running) break;
      if (error && (error.code === "asio_timeout" || /timed out/i.test(error.message || ""))) {
        const err = pollPipelineBus(state.pipeline);
        if (err) { state.error = err; break; }
        continue;
      }
      if (state.running) state.error = error && error.message ? error.message : String(error);
      break;
    }
    if (!state.running) { try { sock.close(); } catch {} break; }
    if (!sock) { pollPipelineBus(state.pipeline); continue; }
    state.clientSock = sock;
    let pb;
    try {
      pb = PacketBuffer.openExisting(state.packetBufferName);
    } catch (error) {
      console.log(`rtsp: could not open packet buffer ${state.packetBufferName}: ${error && error.message ? error.message : error}`);
      try { sock.close(); } catch {}
      state.clientSock = null;
      continue;
    }
    try {
      await serveRtspClient(sock, pb, state.path);
    } catch (error) {
      console.log(`rtsp client error: ${error && error.message ? error.message : error}`);
    }
    state.clientSock = null;
    const err = pollPipelineBus(state.pipeline);
    if (err) state.error = err;
  }
  try { state.server && state.server.close(); } catch {}
  try { state.pipeline && state.pipeline.close(); } catch {}
  try { state.clientSock && state.clientSock.close(); } catch {}
  if (state === streamState) {
    console.log(`Local RTSP server stopped: ${state.url || "(no url)"}`);
    state.running = false;
    state.server = state.pipeline = state.clientSock = null;
    state.url = null;
  }
}

// --- Protocol branches --------------------------------------------------------

// RTSP: the hand-written TCP-interleaved RTSP/RTP server above (VP8). Resolves
// to the new state once the listening socket is bound; the accept loop runs in
// the background.
async function startRtspBranch(cfg) {
  const packetBufferName = `${RTSP_SHM_PREFIX}_${Date.now()}`;
  const pipeline = createRtspPipeline(packetBufferName, cfg.source);
  let server;
  try {
    server = await listen({ host: cfg.host, port: cfg.port });
  } catch (error) {
    try { pipeline.close(); } catch {}
    throw error;
  }
  const addr = server.address ? server.address() : null;
  const port = (addr && addr.port) || cfg.port;
  const url = `rtsp://${cfg.host}:${port}${cfg.path}`;
  const state = {
    running: true, protocol: "rtsp", server, pipeline, clientSock: null,
    packetBufferName, url, host: cfg.host, port, path: cfg.path,
    outDir: null, error: null,
  };
  serveRtspAcceptLoop(state).catch((error) => {
    console.log(`rtsp accept loop failed: ${error && error.message ? error.message : error}`);
    state.error = error && error.message ? error.message : String(error);
  });
  return state;
}

// Fail fast when a helper-built pipeline needs elements this install lacks.
function requireBuilderElements(built, protocol) {
  if (built.missingElements && built.missingElements.length) {
    throw new Error(`Missing GStreamer elements for ${protocol}: ${built.missingElements.join(", ")}`);
  }
}

// Push-style bus errors: any pipeline error stops the protocol output and is
// surfaced through /api/stream/status.
function watchStreamPipeline(pipeline, state) {
  pipeline.watchBus({
    onError: (message) => {
      const detail = message.message || "GStreamer error";
      console.log(`stream pipeline error: ${detail}`);
      state.error = detail;
      if (state === streamState && state.running) stopStreamServer();
    },
    onEos: () => {
      if (state === streamState && state.running) {
        console.log("stream pipeline reached end of stream");
        stopStreamServer();
      }
    },
  });
}

// HLS / DASH: helper-built hlssink/dashsink pipeline (H.264) writing segments
// into a fixed directory served by the HTTP static mounts.
function startSegmentBranch(cfg) {
  const isHls = cfg.protocol === "hls";
  const outDir = isHls ? HLS_DIR : DASH_DIR;
  if (!outDir) throw new Error("No writable output directory (start with --out-dir= or make /tmp/wl2-stream writable).");
  resetOutputDir(outDir);
  const bufferSeconds = Math.max(SEGMENT_SECONDS * 2, Math.min(120, Number(cfg.segmentBufferSeconds) || DEFAULT_SEGMENT_BUFFER_SECONDS));
  const segmentCount = Math.max(2, Math.ceil(bufferSeconds / SEGMENT_SECONDS));
  const options = {
    outDir,
    source: sourceLaunch(cfg.source),
    width: WIDTH,
    height: HEIGHT,
    fps: FPS,
    segmentSeconds: SEGMENT_SECONDS,
    playlistLength: segmentCount,
    maxFiles: segmentCount * 2,
    url: isHls
      ? `http://${HOST}:${PORT}/hls/stream.m3u8`
      : `http://${HOST}:${PORT}/dash/stream.mpd`,
  };
  const built = isHls ? buildHlsOutput(options) : buildDashOutput(options);
  requireBuilderElements(built, cfg.protocol);
  const pipeline = parseLaunch(built.launch);
  const state = {
    running: true, protocol: cfg.protocol, server: null, pipeline, clientSock: null,
    packetBufferName: null, url: built.url, host: HOST, port: PORT, path: null,
    outDir, error: null,
  };
  watchStreamPipeline(pipeline, state);
  pipeline.play();
  return state;
}

// SRT: helper-built srtsink listener pipeline (H.264 over MPEG-TS). GStreamer
// opens the listening socket itself (parseLaunch is a trusted-input API).
function startSrtBranch(cfg) {
  const built = buildSrtOutput({
    port: cfg.port,
    publicHost: "127.0.0.1",
    source: sourceLaunch(cfg.source),
    width: WIDTH,
    height: HEIGHT,
    fps: FPS,
  });
  requireBuilderElements(built, "srt");
  const pipeline = parseLaunch(built.launch);
  const state = {
    running: true, protocol: "srt", server: null, pipeline, clientSock: null,
    packetBufferName: null, url: built.url, host: "127.0.0.1", port: cfg.port, path: null,
    outDir: null, error: null,
  };
  watchStreamPipeline(pipeline, state);
  pipeline.play();
  return state;
}

// MJPEG: JPEG frames into a membus PacketBuffer, served as a long-lived
// multipart/x-mixed-replace response from the /mjpeg streaming route.
function startMjpegBranch(cfg) {
  if (!haveElement("jpegenc")) throw new Error("GStreamer element jpegenc is not installed.");
  const packetBufferName = `${MJPEG_SHM_PREFIX}_${Date.now()}`;
  const launch = [
    sourceLaunch(cfg.source),
    "queue leaky=downstream max-size-buffers=4",
    "videoconvert",
    "videoscale",
    "videorate",
    `video/x-raw,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1`,
    "jpegenc quality=80",
    "appsink name=mjpeg_packet_sink sync=false drop=true max-buffers=2",
  ].join(" ! ");
  const pipeline = parseLaunch(launch);
  pipeline.attachPacketSink({
    packetBufferName,
    elementName: "mjpeg_packet_sink",
    create: true,
    buffers: 8,
    arenaSize: 8 * 1024 * 1024,
    maxRecord: 1048576,
    caps: "image/jpeg",
  });
  const state = {
    running: true, protocol: "mjpeg", server: null, pipeline, clientSock: null,
    packetBufferName, url: `http://${HOST}:${PORT}/mjpeg`, host: HOST, port: PORT, path: "/mjpeg",
    outDir: null, error: null,
  };
  watchStreamPipeline(pipeline, state);
  pipeline.play();
  return state;
}

// Start the protocol output for cfg.protocol. Resolves to { url }.
async function startStreamServer(cfg) {
  if (streamState.running) {
    const e = new Error(`Protocol output already running (${streamState.protocol}): ${streamState.url}`);
    e.code = "already_running";
    throw e;
  }
  let state;
  switch (cfg.protocol) {
    case "rtsp":
      state = await startRtspBranch(cfg);
      break;
    case "hls":
    case "dash":
      state = startSegmentBranch(cfg);
      break;
    case "srt":
      state = startSrtBranch(cfg);
      break;
    case "mjpeg":
      state = startMjpegBranch(cfg);
      break;
    default:
      throw new Error(`Unknown stream protocol: ${cfg.protocol}`);
  }
  streamState = state;
  console.log(`Protocol output (${state.protocol}): ${state.url}`);
  return { url: state.url };
}

// Stop the protocol output: close the pipeline/listener/client socket and clean
// protocol outputs. The page + WebRTC signaling always stay up.
function stopStreamServer() {
  if (!streamState.running) return false;
  const state = streamState;
  state.running = false;
  try { state.server && state.server.close(); } catch {}
  try { state.clientSock && state.clientSock.close(); } catch {}
  try { state.pipeline && state.pipeline.close(); } catch {}
  if (state.outDir) {
    // Drop stale segments/manifests so the next start cannot serve a mix.
    try { resetOutputDir(state.outDir); } catch {}
  }
  if (state.protocol !== "rtsp") {
    // The RTSP accept loop clears its own state when it exits.
    state.server = state.pipeline = state.clientSock = null;
    state.url = null;
    console.log(`Protocol output stopped (${state.protocol})`);
  }
  return true;
}

// When --serve-stream is on, start the server at startup (the UI can also
// start/stop it).
if (SERVE_STREAM) {
  startStreamServer({
    protocol: STREAM_PROTOCOL,
    source: rtspSelection(),
    host: RTSP_HOST,
    port: STREAM_PROTOCOL === "srt" ? SRT_PORT : RTSP_PORT,
    path: RTSP_PATH,
  }).catch((error) => console.log(`protocol output disabled: ${error && error.message ? error.message : error}`));
}

const page = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>wl2 HTTP video server</title>
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
    .hidden { display: none !important; }
    .video-wrap { background: #050607; min-height: 360px; display: grid; place-items: center; border: 1px solid #30383d; border-radius: 8px; overflow: hidden; }
    video { width: 100%; height: 100%; max-height: 72vh; object-fit: contain; background: #050607; }
    .status { margin-top: 12px; min-height: 22px; font: 13px ui-monospace, SFMono-Regular, Menlo, monospace; color: #9fb0b8; white-space: pre-wrap; }
    .metrics { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 12px; }
    .metric { border: 1px solid #30383d; border-radius: 6px; padding: 9px; background: #181c1f; }
    .metric span { display: block; color: #93a0a6; font-size: 12px; }
    .metric strong { display: block; margin-top: 2px; font-size: 16px; color: #f2f6f7; overflow-wrap: anywhere; }
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
      <h1>wl2 HTTP video server</h1>

      <label for="sourceKind">Source</label>
      <select id="sourceKind">
        <option value="test">Test pattern</option>
        <option value="camera">V4L camera</option>
        <option value="file">Video file path</option>
      </select>

      <div id="cameraField">
        <label for="camera">Camera</label>
        <select id="camera"></select>
      </div>

      <div id="fileField">
        <label for="filePath">Server-side file path</label>
        <input id="filePath" placeholder="/home/me/Videos/sample.mp4">
      </div>

      <div class="controls">
        <button id="start">Start</button>
        <button id="stop" class="secondary" disabled>Stop</button>
      </div>

      <div id="status" class="status">Loading devices...</div>

      <label for="streamProtocol">Protocol output</label>
      <select id="streamProtocol">
        <option value="rtsp">RTSP</option>
        <option value="hls">HLS</option>
        <option value="dash">DASH</option>
        <option value="srt">SRT</option>
        <option value="mjpeg">MJPEG</option>
      </select>
      <div id="streamEndpointFields" style="display:flex; gap:8px; margin-top:8px;">
        <input id="streamPort" placeholder="port" value="8554" style="flex:1;">
        <input id="streamPath" placeholder="path" value="/stream" style="flex:2;">
      </div>
      <div id="segmentBufferField">
        <label for="segmentBuffer">Segment window seconds</label>
        <input id="segmentBuffer" type="number" min="4" max="120" step="1" value="12">
      </div>
      <div class="controls">
        <button id="streamStart" class="secondary" type="button">Start</button>
        <button id="streamStop" class="secondary" type="button" disabled>Stop</button>
      </div>
      <div style="display:flex; gap:8px; margin-top:8px;">
        <input id="streamUrl" readonly placeholder="stream URL appears here">
        <button id="copyUrl" class="secondary" type="button" disabled>Copy</button>
      </div>
      <div id="streamStatus" class="status">Protocol output: stopped</div>
    </section>

    <section>
      <div class="video-wrap"><video id="video" autoplay playsinline controls muted></video></div>
      <div class="metrics">
        <div class="metric"><span>Input</span><strong id="sourceCard">test</strong></div>
        <div class="metric" data-direct-card><span>Browser connection</span><strong id="connection">idle</strong></div>
        <div class="metric" data-direct-card><span>Browser RTP packets</span><strong id="packets">0</strong></div>
        <div class="metric hidden" data-output-card><span>Protocol</span><strong id="protocolCard">stopped</strong></div>
        <div class="metric hidden" data-output-card><span>Stream URL</span><strong id="outputCard">none</strong></div>
        <div class="metric hidden" data-segment-card><span>Segment window</span><strong id="segmentCard">12s</strong></div>
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
    const cameraFieldEl = document.getElementById("cameraField");
    const cameraEl = document.getElementById("camera");
    const sourceKindEl = document.getElementById("sourceKind");
    const fileFieldEl = document.getElementById("fileField");
    const filePathEl = document.getElementById("filePath");
    const startEl = document.getElementById("start");
    const stopEl = document.getElementById("stop");
    const videoEl = document.getElementById("video");
    const connectionEl = document.getElementById("connection");
    const packetsEl = document.getElementById("packets");
    const sourceCardEl = document.getElementById("sourceCard");
    const protocolCardEl = document.getElementById("protocolCard");
    const outputCardEl = document.getElementById("outputCard");
    const segmentCardEl = document.getElementById("segmentCard");
    const logEl = document.getElementById("log");
    const copyLogEl = document.getElementById("copyLog");
    const clearLogEl = document.getElementById("clearLog");
    const streamProtocolEl = document.getElementById("streamProtocol");
    const streamStartEl = document.getElementById("streamStart");
    const streamStopEl = document.getElementById("streamStop");
    const streamEndpointFieldsEl = document.getElementById("streamEndpointFields");
    const streamPortEl = document.getElementById("streamPort");
    const streamPathEl = document.getElementById("streamPath");
    const segmentBufferFieldEl = document.getElementById("segmentBufferField");
    const segmentBufferEl = document.getElementById("segmentBuffer");
    const streamUrlEl = document.getElementById("streamUrl");
    const copyUrlEl = document.getElementById("copyUrl");
    const streamStatusEl = document.getElementById("streamStatus");

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

    function formatSourceLabel(sel) {
      if (sel.kind === "camera") return "camera";
      if (sel.kind === "file") return "file";
      return "test";
    }

    function updateSourceFields() {
      const kind = sourceKindEl.value;
      cameraFieldEl.classList.toggle("hidden", kind !== "camera");
      fileFieldEl.classList.toggle("hidden", kind !== "file");
      sourceCardEl.textContent = formatSourceLabel(selection());
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
      packetsEl.textContent = "0";
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

    // --- Protocol output start/stop ----------------------------------------
    // Which extra fields each protocol uses; the rest are disabled.
    const PROTOCOL_FIELDS = {
      rtsp: { port: 8554, path: true },
      hls: { port: 0, path: false, buffer: true },
      dash: { port: 0, path: false, buffer: true },
      srt: { port: 7001, path: false },
      mjpeg: { port: 0, path: false },
    };
    let lastProtocols = null;
    let lastRunning = null;

    // setDefaults resets the port field to the protocol default; only the
    // protocol-change handler does that, so status polls never stomp edits.
    function applyProtocolFields(setDefaults) {
      const fields = PROTOCOL_FIELDS[streamProtocolEl.value] || PROTOCOL_FIELDS.rtsp;
      streamEndpointFieldsEl.classList.toggle("hidden", !fields.port && !fields.path);
      streamPortEl.classList.toggle("hidden", !fields.port);
      streamPathEl.classList.toggle("hidden", !fields.path);
      streamPortEl.disabled = !fields.port;
      if (setDefaults && fields.port) streamPortEl.value = String(fields.port);
      streamPathEl.disabled = !fields.path;
      segmentBufferFieldEl.classList.toggle("hidden", !fields.buffer);
      segmentBufferEl.disabled = !fields.buffer;
      document.querySelectorAll("[data-segment-card]").forEach((el) => el.classList.toggle("hidden", !fields.buffer));
      segmentCardEl.textContent = (Number(segmentBufferEl.value) || 12) + "s";
      const info = lastProtocols && lastProtocols[streamProtocolEl.value];
      if (info && !info.available) {
        streamStatusEl.textContent = "Protocol output: " + streamProtocolEl.value + " unavailable (" + info.hint + ")";
      }
    }

    function setStreamUi(state) {
      const running = !!state.running;
      lastProtocols = state.protocols || lastProtocols;
      streamStartEl.disabled = running;
      streamStopEl.disabled = !running;
      streamProtocolEl.disabled = running;
      if (running) {
        streamPortEl.disabled = true;
        streamPathEl.disabled = true;
        segmentBufferEl.disabled = true;
      } else {
        applyProtocolFields(false);
      }
      streamUrlEl.value = running && state.url ? state.url : "";
      copyUrlEl.disabled = !streamUrlEl.value;
      document.querySelectorAll("[data-output-card]").forEach((el) => el.classList.toggle("hidden", !running));
      protocolCardEl.textContent = running ? state.protocol : "stopped";
      outputCardEl.textContent = running && state.url ? state.url : "none";
      if (running && state.url) {
        const extra = state.protocol === "mjpeg" ? " (multipart JPEG)" : "";
        streamStatusEl.textContent = "Protocol output (" + state.protocol + "): " + state.url + extra;
        if (lastRunning !== true) log("Protocol output running (" + state.protocol + "): " + state.url);
      } else if (state.error) {
        streamStatusEl.textContent = "Protocol output: error - " + state.error;
      } else {
        streamStatusEl.textContent = "Protocol output: stopped";
      }
      lastRunning = running;
    }

    async function streamRefresh() {
      try {
        const res = await fetch("/api/stream/status");
        setStreamUi(await res.json());
      } catch (error) {
        streamStatusEl.textContent = "Protocol output: status unavailable";
      }
    }

    streamProtocolEl.addEventListener("change", () => applyProtocolFields(true));
    sourceKindEl.addEventListener("change", updateSourceFields);
    cameraEl.addEventListener("change", updateSourceFields);
    filePathEl.addEventListener("input", updateSourceFields);
    segmentBufferEl.addEventListener("input", () => { segmentCardEl.textContent = (Number(segmentBufferEl.value) || 12) + "s"; });

    streamStartEl.addEventListener("click", async () => {
      streamStartEl.disabled = true;
      streamStatusEl.textContent = "Starting protocol output...";
      try {
        const res = await fetch("/api/stream/start", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({
            protocol: streamProtocolEl.value,
            source: selection(),
            host: "127.0.0.1",
            port: Number(streamPortEl.value) || 0,
            path: streamPathEl.value || "/stream",
            segmentBufferSeconds: Number(segmentBufferEl.value) || 12,
          }),
        });
        const data = await res.json();
        if (!res.ok || !data.ok) {
          log("Protocol output start failed: " + (data.error || res.statusText));
          streamStatusEl.textContent = "Protocol output: " + (data.error || "start failed");
          streamStartEl.disabled = false;
          return;
        }
        log("Protocol output started: " + data.url);
      } catch (error) {
        log("Protocol output start error: " + (error.message || error));
        streamStartEl.disabled = false;
      }
      await streamRefresh();
    });

    streamStopEl.addEventListener("click", async () => {
      streamStopEl.disabled = true;
      try {
        await fetch("/api/stream/stop", { method: "POST" });
        log("Protocol output stopped.");
      } catch (error) {
        log("Protocol output stop error: " + (error.message || error));
      }
      await streamRefresh();
    });

    copyUrlEl.addEventListener("click", async () => {
      if (!streamUrlEl.value) return;
      try {
        await navigator.clipboard.writeText(streamUrlEl.value);
        log("Copied stream URL: " + streamUrlEl.value);
      } catch (error) {
        log("Copy failed: " + (error.message || error));
      }
    });

    // Keep the stream UI in sync (the server can stop on its own on a source error).
    setInterval(streamRefresh, 1500);
    applyProtocolFields(true);
    updateSourceFields();
    streamRefresh();

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
    stream: { running: streamState.running, protocol: streamState.protocol, url: streamState.url },
  });
});

// --- Protocol output controls (UI-driven start/stop) ------------------------
// Per-protocol readiness so the UI can disable protocols this install cannot
// serve (missing encoder elements, no writable output dir, ...).
function streamProtocolInfo() {
  const h264 = haveElement("x264enc") && haveElement("mpegtsmux");
  return {
    rtsp: { available: haveElement("vp8enc") && haveElement("rtpvp8pay"), hint: "needs vp8enc + rtpvp8pay" },
    hls: { available: !!HLS_DIR && h264 && haveElement("hlssink"), hint: "needs x264enc + hlssink + writable out dir" },
    dash: { available: !!DASH_DIR && haveElement("x264enc") && haveElement("dashsink"), hint: "needs x264enc + dashsink + writable out dir" },
    srt: { available: h264 && haveElement("srtsink"), hint: "needs x264enc + mpegtsmux + srtsink" },
    mjpeg: { available: haveElement("jpegenc"), hint: "needs jpegenc" },
  };
}

server.route("GET", "/api/stream/status", () => json({
  running: streamState.running,
  protocol: streamState.protocol || null,
  url: streamState.url || null,
  port: streamState.port || null,
  path: streamState.path || null,
  error: streamState.error || null,
  protocols: streamProtocolInfo(),
}));

server.route("POST", "/api/stream/start", async (req) => {
  const cfg = {
    protocol: "rtsp",
    source: { kind: "test" },
    host: RTSP_HOST,
    port: RTSP_PORT,
    path: RTSP_PATH,
    segmentBufferSeconds: DEFAULT_SEGMENT_BUFFER_SECONDS,
  };
  try {
    const text = req && req.body ? req.body.toString() : "";
    if (text) {
      const body = JSON.parse(text);
      if (body && body.protocol) cfg.protocol = String(body.protocol);
      if (body && body.source) cfg.source = body.source;
      if (body && body.host) cfg.host = String(body.host);
      if (body && body.port) cfg.port = Number(body.port) || cfg.port;
      if (body && body.path) cfg.path = String(body.path);
      if (body && body.segmentBufferSeconds) cfg.segmentBufferSeconds = Number(body.segmentBufferSeconds) || cfg.segmentBufferSeconds;
    }
  } catch (error) {
    return json({ ok: false, error: "invalid JSON body: " + (error.message || error) }, 400);
  }
  // SRT defaults to its own port when the request leaves the RTSP default in place.
  if (cfg.protocol === "srt" && (!cfg.port || cfg.port === RTSP_PORT)) cfg.port = SRT_PORT;
  try {
    const { url } = await startStreamServer(cfg);
    return json({ ok: true, url });
  } catch (error) {
    return json({ ok: false, error: error && error.message ? error.message : String(error) }, 400);
  }
});

server.route("POST", "/api/stream/stop", () => {
  const stopped = stopStreamServer();
  return json({ ok: true, stopped });
});

// --- MJPEG streaming route ---------------------------------------------------
// A long-lived multipart/x-mixed-replace response fed from the MJPEG pipeline's
// PacketBuffer. Each connected client walks the ring at its own pace (newest
// frame wins; missed frames are dropped).
server.routeStream("GET", "/mjpeg", async (req, stream) => {
  const state = streamState;
  if (!state.running || state.protocol !== "mjpeg" || !state.packetBufferName) {
    await stream.respond({ status: 503, headers: { "content-type": "text/plain" } });
    await stream.write("MJPEG stream is not running. Start the protocol output with the MJPEG protocol first.");
    return;
  }
  let pb;
  try {
    pb = PacketBuffer.openExisting(state.packetBufferName);
  } catch (error) {
    await stream.respond({ status: 500, headers: { "content-type": "text/plain" } });
    await stream.write(`Could not open the MJPEG frame buffer: ${error.message || error}`);
    return;
  }
  await stream.respond({
    status: 200,
    headers: {
      "content-type": "multipart/x-mixed-replace; boundary=wl2frame",
      "cache-control": "no-store",
    },
  });
  let lastSeq = 0;
  const frameDelayMs = Math.max(10, Math.floor(1000 / FPS / 2));
  while (!stream.closed && streamState === state && state.running) {
    const meta = pb.metadata();
    if (meta.sequence > lastSeq) {
      const slot = (((meta.pointer - 1) % meta.buffers) + meta.buffers) % meta.buffers;
      let rec = null;
      try { rec = pb.record(slot); } catch {}
      if (rec && rec.sequence > lastSeq) {
        lastSeq = rec.sequence;
        const jpeg = rec.payload && rec.payload.uint8Array ? rec.payload.uint8Array() : null;
        if (jpeg && jpeg.length) {
          const head = `--wl2frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpeg.length}\r\n\r\n`;
          if (!(await stream.write(head))) break;
          if (!(await stream.write(jpeg))) break;
          if (!(await stream.write("\r\n"))) break;
        }
      }
    }
    await sleep(frameDelayMs);
  }
});

// HLS/DASH output directories, served as fixed static mounts (mounts must be
// registered before listen(); status decides whether the URLs are advertised).
if (HLS_DIR) server.static("/hls", HLS_DIR);
if (DASH_DIR) server.static("/dash", DASH_DIR);

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
console.log(`wl2 HTTP video server: http://${address.host}:${address.port}/`);
console.log(`Serving ${WIDTH}x${HEIGHT}@${FPS} VP8 RTP. Shared-memory prefix: ${SHM_PREFIX}`);
if (STREAM_OUT_ROOT) console.log(`HLS/DASH output root: ${STREAM_OUT_ROOT}`);
