#include "wl2/membus.h"
#include "wl2/wl2.h"
#include "wl2_gstreamer/wl2_gstreamer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_gstreamer test failed: " << message << '\n';
    return 1;
}

std::string unique_name(const std::string& suffix) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "/wl2gst_" + std::to_string(ticks) + "_" + suffix;
}

// Whether a frame view holds more than a single repeated byte (i.e. not blank).
bool frame_is_nonblank(const char* data, size_t size) {
    if (!data || size == 0) {
        return false;
    }
    for (size_t i = 1; i < size; ++i) {
        if (data[i] != data[0]) {
            return true;
        }
    }
    return false;
}

// The core runtime pipeline coverage (parseLaunch, lifecycle, bus, EOS) plus the
// media membus bridges. Rings are created in this process so their contents can
// be inspected after the JS bridges publish into them.
int run_gstreamer_tests() {
    const std::string vidName = unique_name("vid");
    const std::string audName = unique_name("aud");
    const std::string srcName = unique_name("src");
    const std::string dstName = unique_name("dst");
    const std::string tpName = unique_name("tp");
    const std::string pktName = unique_name("pkt");
    const std::string pktSrcName = unique_name("pkt_src");
    const std::string playbackName = unique_name("playback");
    const std::string captureName = unique_name("capture");
    const std::string udpInName = unique_name("udp_in");
    const std::string rtpInName = unique_name("rtp_in");
    const std::string tcpInName = unique_name("tcp_in");
    const std::string teeName = unique_name("tee");
    const std::string overlayName = unique_name("overlay");
    const std::string recordPath = std::string("/tmp") + unique_name("record") + ".webm";
    const std::string packetPath = std::string("/tmp") + unique_name("packet_record") + ".bin";
    const std::string teePath = std::string("/tmp") + unique_name("tee") + ".webm";
    const std::string snapshotPath = std::string("/tmp") + unique_name("snapshot") + ".png";

    // Owner rings kept open for the duration of the test so shared memory (and its
    // published contents) survives the JS bridges opening and closing their views.
    auto vid = wl2::VideoBuffer::create(vidName, 64, 48, wl2::VideoPixelFormat::Rgba32, 30, 4);
    auto aud = wl2::AudioBuffer::create(audName, 2, wl2::AudioSampleFormat::S16Le, 48000, 50, 16);
    auto src = wl2::VideoBuffer::create(srcName, 32, 16, wl2::VideoPixelFormat::Rgba32, 30, 4);
    auto dst = wl2::VideoBuffer::create(dstName, 32, 16, wl2::VideoPixelFormat::Rgba32, 30, 4);
    auto playback = wl2::VideoBuffer::create(playbackName, 32, 16, wl2::VideoPixelFormat::Rgba32, 30, 4);
    auto pktSrc = wl2::PacketBuffer::create(pktSrcName, 8, 4096, 1024, 0, 0, "application/octet-stream");
    if (!vid || !aud || !src || !dst || !playback || !pktSrc) {
        return fail("could not create owner rings");
    }
    if (auto wrote = pktSrc.value().write("packet-source-payload", wl2::PacketKind::Data, 0, 123, "{}"); !wrote) {
        return fail("could not seed packet source ring: " + wrote.error().message());
    }

    // Fill the round-trip source frame with a deterministic pattern.
    auto srcView = src.value().frame(0);
    if (!srcView || !srcView.value().data) {
        return fail("could not map source frame");
    }
    for (size_t i = 0; i < srcView.value().size; ++i) {
        srcView.value().data[i] = static_cast<char>((i * 7 + 3) & 0xff);
    }

    wl2::RuntimeOptions options;
    options.allowSharedMemory = true;
    options.sharedMemoryAllowList = {"/wl2gst_"};
    options.allowFilesystemReads = true;
    options.filesystemReadRoots = {"/tmp"};
    options.allowNetwork = true;
    options.networkAllowList = {"127.0.0.1:*"};
    options.allowListening = true;
    options.listenAllowList = {"127.0.0.1:*"};
    options.staticModules.push_back(wl2_gstreamer_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return fail("runtime initialize failed: " + init.error().message());
    }

    auto engine = wl2::createConfiguredJsEngine();

    const std::string header = std::string("const VID = \"") + vidName + "\";\n"
        + "const AUD = \"" + audName + "\";\n"
        + "const SRC = \"" + srcName + "\";\n"
        + "const DST = \"" + dstName + "\";\n"
        + "const TP = \"" + tpName + "\";\n"
        + "const PKT = \"" + pktName + "\";\n"
        + "const PKTSRC = \"" + pktSrcName + "\";\n"
        + "const PLAYBACK = \"" + playbackName + "\";\n"
        + "const CAPTURE = \"" + captureName + "\";\n"
        + "const UDPIN = \"" + udpInName + "\";\n"
        + "const RTPIN = \"" + rtpInName + "\";\n"
        + "const TCPIN = \"" + tcpInName + "\";\n"
        + "const TEE = \"" + teeName + "\";\n"
        + "const OVERLAY = \"" + overlayName + "\";\n"
        + "const RECORD_PATH = \"" + recordPath + "\";\n"
        + "const PACKET_PATH = \"" + packetPath + "\";\n"
        + "const TEE_PATH = \"" + teePath + "\";\n"
        + "const SNAPSHOT_PATH = \"" + snapshotPath + "\";\n";

    const std::string body = R"JS(
import {
  version,
  capabilities,
  listElements,
  listPlugins,
  elementInfo,
  hasProperty,
  uriHandlers,
  parseLaunch,
  testPattern,
  filePlayback,
  recordVideoBuffer,
  recordPacketBuffer,
  discoverMedia,
  DeviceMonitor,
  captureDevice,
  sendUdpPackets,
  receiveUdpPackets,
  sendRtpPackets,
  receiveRtpPackets,
  sendTcpPackets,
  receiveTcpPackets,
  streamVideoUdp,
  streamVideoTcp,
  rtspPlayback,
  requiredElementsForUri,
  canDecodeUri,
  buildHlsOutput,
  buildDashOutput,
  buildSrtOutput,
  teeVideoBuffer,
  overlayVideoBuffer,
  Caps
} from "wl2:gstreamer";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function pollToEos(pipeline, maxIterations) {
  let sawEos = false;
  for (let i = 0; i < (maxIterations || 300) && !sawEos; ++i) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 32 });
    for (const m of messages) {
      if (m.type === "eos") sawEos = true;
      if (m.type === "error") throw new Error("pipeline error: " + (m.message || ""));
    }
  }
  return sawEos;
}

// --- Core runtime coverage ---

const v = version();
assert(v.gstreamer && v.gstreamer.major >= 1, "version wrong");
const caps = capabilities();
assert(caps.initialized === true, "gstreamer did not initialize");
assert(caps.features.app === true, "gstreamer-app-1.0 feature missing");
assert(caps.bridges.video === true && caps.bridges.audio === true, "raw bridges not advertised");
assert(caps.bridges.packet === true, "packet bridge not advertised");
const elements = listElements();
assert(elements.some((e) => e.name === "appsink"), "appsink not listed");
assert(listPlugins({ filter: "coreelements" }).length >= 1, "coreelements missing");
const elementNames = new Set(elements.map((e) => e.name));
const appsinkInfo = elementInfo("appsink");
assert(appsinkInfo.found === true, "appsink elementInfo not found");
assert(appsinkInfo.properties.some((p) => p.name === "max-buffers"), "appsink max-buffers property missing");
assert(appsinkInfo.padTemplates.some((p) => p.direction === "sink"), "appsink sink pad template missing");
assert(elementInfo("definitely-not-a-wl2-element").found === false, "missing elementInfo should be found:false");
assert(hasProperty("appsink", "max-buffers") === true, "hasProperty false for appsink max-buffers");
assert(hasProperty("appsink", "definitely-missing") === false, "hasProperty true for missing property");
const fileHandlers = uriHandlers({ protocol: "file" });
assert(fileHandlers.some((h) => h.direction === "src" && h.protocols.includes("file")), "file URI src handler missing");
assert(requiredElementsForUri("rtsp://example.invalid/live").includes("rtspsrc"), "RTSP requirements missing rtspsrc");
assert(requiredElementsForUri("https://example.invalid/live.m3u8").includes("hlsdemux"), "HLS requirements missing hlsdemux");
const mjpegReq = requiredElementsForUri("http://example.invalid/mjpeg");
assert(mjpegReq.includes("multipartdemux") && mjpegReq.includes("jpegparse") && mjpegReq.includes("jpegdec"), "MJPEG requirements missing multipartdemux/jpegparse/jpegdec");
const decode = canDecodeUri("https://example.invalid/live.m3u8");
assert(Array.isArray(decode.requiredElements) && Array.isArray(decode.missingElements), "canDecodeUri shape wrong");
const hls = buildHlsOutput({ outDir: "/tmp/wl2-hls-test", source: "videotestsrc num-buffers=1" });
assert(hls.launch.includes("hlssink") && hls.url.endsWith("/hls/stream.m3u8"), "buildHlsOutput wrong shape");
const smoothHls = buildHlsOutput({ outDir: "/tmp/wl2-hls-test", source: "videotestsrc num-buffers=1", segmentSeconds: 2, playlistLength: 6, maxFiles: 12 });
assert(smoothHls.launch.includes("target-duration=2 max-files=12 playlist-length=6"), "buildHlsOutput segment options missing: " + smoothHls.launch);
const dash = buildDashOutput({ outDir: "/tmp/wl2-dash-test", source: "videotestsrc num-buffers=1", segmentSeconds: 4 });
assert(dash.launch.includes("dashsink") && dash.url.endsWith("/dash/stream.mpd"), "buildDashOutput wrong shape");
assert(dash.launch.includes("target-duration=4"), "buildDashOutput segmentSeconds missing: " + dash.launch);
const srt = buildSrtOutput({ port: 7101, source: "videotestsrc num-buffers=1" });
assert(srt.launch.includes("srtsink") && srt.url === "srt://127.0.0.1:7101", "buildSrtOutput wrong shape");

// Bad launch string fails with a stable code.
let parseCode = null;
try { parseLaunch("this-is-not-an-element ! fakesink"); } catch (e) { parseCode = e.code; }
assert(parseCode === "gstreamer_parse_failed", "bad launch should be gstreamer_parse_failed, got " + parseCode);

// --- testPattern convenience helper publishes frames ---

const tp = testPattern({ videoBufferName: TP, width: 64, height: 48, fps: 30, numBuffers: 10 });
tp.play();
assert(pollToEos(tp), "testPattern did not reach EOS");
const tpStats = tp.stats();
assert(tpStats.wl2_video_sink.frames >= 1, "testPattern published no frames");
assert(tpStats.wl2_video_sink.negotiatedCaps.indexOf("RGBA") >= 0, "testPattern caps not RGBA: " + tpStats.wl2_video_sink.negotiatedCaps);
tp.close();

// --- Video sink into a pre-created ring ---

const vp = parseLaunch("videotestsrc num-buffers=5 ! videoconvert ! video/x-raw,format=RGBA,width=64,height=48,framerate=30/1 ! appsink name=wl2_video_sink");
const vAttach = vp.attachVideoSink({ videoBufferName: VID, create: false });
assert(vAttach.ok && vAttach.width === 64 && vAttach.height === 48, "video attach metadata wrong");
vp.play();
assert(pollToEos(vp), "video sink pipeline did not reach EOS");
assert(vp.stats().wl2_video_sink.frames >= 1, "video sink published no frames");
vp.close();

// --- Audio sink into a pre-created ring ---

const ap = parseLaunch("audiotestsrc num-buffers=5 ! audioconvert ! audioresample ! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 ! appsink name=wl2_audio_sink");
ap.attachAudioSink({ audioBufferName: AUD, create: false });
ap.play();
assert(pollToEos(ap), "audio sink pipeline did not reach EOS");
assert(ap.stats().wl2_audio_sink.buffers >= 1, "audio sink published no buffers");
ap.close();

// --- Packet sink into PacketBuffer ---

const pp = parseLaunch("videotestsrc num-buffers=3 ! videoconvert ! video/x-raw,format=RGB,width=8,height=8,framerate=1/1 ! appsink name=wl2_packet_sink");
pp.attachPacketSink({ packetBufferName: PKT, create: true, buffers: 16, arenaSize: 1048576, maxRecord: 65536, caps: "video/x-raw", track: 2 });
pp.play();
assert(pollToEos(pp), "packet sink pipeline did not reach EOS");
const packetStats = pp.stats().wl2_packet_sink;
assert(packetStats.packets >= 1, "packet sink wrote no packets");
assert(packetStats.bytes > 0, "packet sink wrote no bytes");
pp.close();

// --- PacketBuffer / direct packet appsrc path ---

const ps = parseLaunch("appsrc name=wl2_packet_src caps=application/octet-stream ! fakesink");
ps.attachPacketSource({ packetBufferName: PKTSRC, caps: "application/octet-stream" });
const pushedPacket = ps.pushPacket({ waitTimeoutMs: 1 });
assert(pushedPacket.ok && pushedPacket.bytes > 0 && pushedPacket.pts === 123, "pushPacket from ring failed");
const directPacket = ps.pushPacket({ data: "direct-packet", pts: 456, duration: 10 });
assert(directPacket.ok && directPacket.bytes === 13, "direct pushPacket failed");
ps.endOfStream();
ps.play();
assert(pollToEos(ps), "packet source pipeline did not reach EOS");
assert(ps.stats().wl2_packet_src.pushed >= 2, "packet source push count wrong");
ps.close();

// --- Phase 3 helper APIs ---

const monitor = DeviceMonitor.create();
assert(monitor && Array.isArray(monitor.devices), "DeviceMonitor.create returned wrong shape");

const cap = captureDevice({ videoBufferName: CAPTURE, width: 16, height: 12, fps: 5, buffers: 2 });
cap.close();

const packetRecord = recordPacketBuffer({ packetBufferName: PKTSRC, outputPath: PACKET_PATH, caps: "application/octet-stream" });
packetRecord.play();
const recordedPacket = packetRecord.pushPacket({ waitTimeoutMs: 1 });
assert(recordedPacket.ok && recordedPacket.bytes > 0, "recordPacketBuffer push failed");
packetRecord.endOfStream();
assert(pollToEos(packetRecord), "recordPacketBuffer did not reach EOS");
packetRecord.close();

const udpOut = sendUdpPackets({ packetBufferName: PKTSRC, host: "127.0.0.1", port: 45101 });
udpOut.close();
const udpIn = receiveUdpPackets({ packetBufferName: UDPIN, host: "127.0.0.1", port: 45102 });
udpIn.close();
const rtpOut = sendRtpPackets({ packetBufferName: PKTSRC, host: "127.0.0.1", port: 45103 });
rtpOut.close();
const rtpIn = receiveRtpPackets({ packetBufferName: RTPIN, host: "127.0.0.1", port: 45104 });
rtpIn.close();

if (elementNames.has("tcpclientsink")) {
  const tcpOut = sendTcpPackets({ packetBufferName: PKTSRC, host: "127.0.0.1", port: 45105 });
  tcpOut.close();
}
if (elementNames.has("tcpserversrc")) {
  const tcpIn = receiveTcpPackets({ packetBufferName: TCPIN, host: "127.0.0.1", port: 45106 });
  tcpIn.close();
}
if (elementNames.has("vp8enc") && elementNames.has("rtpvp8pay")) {
  const videoUdp = streamVideoUdp({ videoBufferName: SRC, host: "127.0.0.1", port: 45107 });
  videoUdp.close();
}
if (elementNames.has("vp8enc") && elementNames.has("webmmux") && elementNames.has("tcpclientsink")) {
  const videoTcp = streamVideoTcp({ videoBufferName: SRC, host: "127.0.0.1", port: 45108 });
  videoTcp.close();
}

let deniedConnect = null;
try { sendUdpPackets({ packetBufferName: PKTSRC, host: "203.0.113.1", port: 45109 }); }
catch (e) { deniedConnect = e.code; }
assert(deniedConnect === "gstreamer_permission_denied", "network connect denial wrong: " + deniedConnect);

let deniedListen = null;
try { receiveUdpPackets({ packetBufferName: UDPIN, host: "0.0.0.0", port: 45110 }); }
catch (e) { deniedListen = e.code; }
assert(deniedListen === "gstreamer_permission_denied", "network listen denial wrong: " + deniedListen);

let deniedRtsp = null;
try { rtspPlayback({ uri: "rtsp://203.0.113.1:8554/test", videoBufferName: PLAYBACK }); }
catch (e) { deniedRtsp = e.code; }
assert(deniedRtsp === "gstreamer_permission_denied", "rtsp denial wrong: " + deniedRtsp);

if (elementNames.has("vp8enc") && elementNames.has("webmmux")) {
  const recorder = recordVideoBuffer({ videoBufferName: SRC, outputPath: RECORD_PATH, encoder: "vp8enc deadline=1", muxer: "webmmux" });
  recorder.play();
  const recFrame0 = recorder.pushVideoFrame({ slot: 0, pts: 0, duration: 33333333 });
  const recFrame1 = recorder.pushVideoFrame({ slot: 0, pts: 33333333, duration: 33333333 });
  assert(recFrame0.ok && recFrame1.ok, "recordVideoBuffer push failed");
  recorder.endOfStream();
  assert(pollToEos(recorder), "recordVideoBuffer did not reach EOS");
  recorder.close();

  if (caps.features.deviceMonitor) {
    const metadata = discoverMedia({ path: RECORD_PATH });
    assert(metadata && metadata.duration >= 0 && Array.isArray(metadata.streams), "discoverMedia returned wrong shape");
    assert(metadata.streams.length >= 1, "discoverMedia found no streams");
  }

  const player = filePlayback({ path: RECORD_PATH, videoBufferName: PLAYBACK, width: 32, height: 16, fps: 30, buffers: 4 });
  player.play();
  assert(pollToEos(player), "filePlayback did not reach EOS");
  assert(player.stats().wl2_video_sink.frames >= 1, "filePlayback published no frames");
  player.close();
}

// --- Video appsrc round-trip: ring -> appsrc -> videoconvert -> appsink -> ring ---

const rt = parseLaunch("appsrc name=wl2_video_src ! videoconvert ! video/x-raw,format=RGBA,width=32,height=16,framerate=30/1 ! appsink name=wl2_video_sink");
rt.attachVideoSource({ videoBufferName: SRC });
rt.attachVideoSink({ videoBufferName: DST, create: false });
rt.play();
const pushed = rt.pushVideoFrame({ slot: 0 });
assert(pushed.ok, "pushVideoFrame failed");
rt.endOfStream();
assert(pollToEos(rt), "round-trip pipeline did not reach EOS");
assert(rt.stats().wl2_video_sink.frames >= 1, "round-trip sink got no frames");
rt.close();

// --- Shared-memory authorization is enforced ---

const dp = parseLaunch("videotestsrc num-buffers=1 ! videoconvert ! video/x-raw,format=RGBA,width=8,height=8 ! appsink name=wl2_video_sink");
let denied = null;
try { dp.attachVideoSink({ videoBufferName: "/not_allowed_name", create: true, width: 8, height: 8 }); }
catch (e) { denied = e.code; }
assert(denied === "gstreamer_permission_denied", "unauthorized ring should be denied, got " + denied);

// --- Unsupported format fails clearly ---

let badFmt = null;
try { dp.attachVideoSink({ videoBufferName: VID, create: false, format: "NOTAFORMAT" }); }
catch (e) { badFmt = e.code; }
assert(badFmt === "gstreamer_invalid_argument", "bad format should be invalid_argument, got " + badFmt);
dp.close();

// --- Advanced pipeline features ---

// Caps.parse: structured caps negotiation utility.
const parsedCaps = Caps.parse("video/x-raw,format=RGBA,width=32,height=16,framerate=30/1");
assert(parsedCaps.structureCount === 1, "Caps.parse structure count wrong");
assert(parsedCaps.structures[0].name === "video/x-raw", "Caps.parse name wrong");
assert(parsedCaps.structures[0].fields.width === "32", "Caps.parse width field wrong: " + parsedCaps.structures[0].fields.width);
let badCaps = null;
try { Caps.parse("!!!not-a-caps"); } catch (e) { badCaps = e.code; }
assert(badCaps === "gstreamer_invalid_argument", "bad caps should be invalid_argument, got " + badCaps);

// tee helper: one source feeds a VideoBuffer and (when the encoder is present) a
// file at the same time. Also exercises negotiatedCaps, queryLatency, snapshot.
const teeHasFile = elementNames.has("vp8enc") && elementNames.has("webmmux");
const tee = teeVideoBuffer({
  videoBufferName: TEE,
  outputPath: teeHasFile ? TEE_PATH : undefined,
  source: "videotestsrc num-buffers=12 pattern=ball",
  width: 32, height: 16, fps: 15
});
tee.play();
// Run to EOS first (a single owner of the bus, so no poll race can swallow the
// EOS). The negotiated caps, latency, and retained snapshot sample all stay
// valid until close(), so they are inspected after the stream finishes.
assert(pollToEos(tee), "tee pipeline did not reach EOS");
assert(tee.stats().wl2_video_sink.frames >= 1, "tee published no frames to VideoBuffer");

const neg = tee.negotiatedCaps({ element: "wl2_video_sink", pad: "sink" });
assert(neg.negotiated === true, "tee sink caps should be negotiated");
assert(neg.text.indexOf("RGBA") >= 0, "negotiatedCaps missing RGBA: " + neg.text);

const latency = tee.queryLatency();
assert(typeof latency.ok === "boolean" && typeof latency.supported === "boolean", "queryLatency shape wrong");

if (elementNames.has("pngenc")) {
  const snap = tee.snapshot({ path: SNAPSHOT_PATH });
  assert(snap.ok && snap.width === 32 && snap.height === 16, "snapshot metadata wrong");
  assert(snap.format === "RGBA" && snap.size > 0 && snap.path === SNAPSHOT_PATH, "snapshot fields wrong");
} else {
  const snapMeta = tee.snapshot({});
  assert(snapMeta.ok && snapMeta.width === 32, "snapshot metadata-only wrong");
}
tee.close();

// negotiatedCaps on a missing element is a stable error.
const np = parseLaunch("videotestsrc num-buffers=1 ! fakesink");
let negMissing = null;
try { np.negotiatedCaps({ element: "does_not_exist" }); } catch (e) { negMissing = e.code; }
assert(negMissing === "gstreamer_element_not_found", "negotiatedCaps missing element wrong: " + negMissing);
np.close();

// overlay helper + live setOverlayText driven like a SharedQueue consumer would.
const overlay = overlayVideoBuffer({
  videoBufferName: OVERLAY,
  text: "start",
  source: "videotestsrc num-buffers=8",
  width: 32, height: 16, fps: 15
});
const setText = overlay.setOverlayText({ text: "updated" });
assert(setText.ok && setText.text === "updated", "setOverlayText result wrong");
let overlayMissing = null;
try { overlay.setOverlayText({ elementName: "nope", text: "x" }); } catch (e) { overlayMissing = e.code; }
assert(overlayMissing === "gstreamer_element_not_found", "setOverlayText missing element wrong: " + overlayMissing);
overlay.play();
assert(pollToEos(overlay), "overlay pipeline did not reach EOS");
assert(overlay.stats().wl2_video_sink.frames >= 1, "overlay published no frames");
overlay.close();

// --- Push-style bus watch (watchBus / unwatchBus) ---

// A finite pipeline delivers EOS through the watch without any polling.
{
  const wb = parseLaunch("videotestsrc num-buffers=5 ! fakesink");
  let messages = 0;
  let errors = 0;
  const eos = new Promise((resolve, reject) => {
    const installed = wb.watchBus({
      onEos: () => resolve(true),
      onError: (m) => { errors++; reject(new Error("watchBus error: " + (m.message || ""))); },
      onMessage: () => { messages++; },
    });
    assert(installed.ok === true, "watchBus install failed");
  });
  // A second watch on the same pipeline is rejected while one is active.
  let dupWatch = null;
  try { wb.watchBus({ onEos: () => {} }); } catch (e) { dupWatch = e.code; }
  assert(dupWatch === "gstreamer_invalid_state", "duplicate watchBus should fail, got " + dupWatch);
  wb.play();
  assert((await eos) === true, "watchBus did not deliver EOS");
  assert(messages >= 1, "watchBus onMessage saw no messages");
  assert(errors === 0, "watchBus saw unexpected errors");
  // While the watch is active it consumes the bus, so busPoll stays empty.
  assert(wb.busPoll({ timeoutMs: 0 }).length === 0, "busPoll should be empty while watch is active");
  wb.unwatchBus();
  wb.close();
}

// An erroring pipeline reports through onError, and re-watching after
// unwatchBus() works on the same pipeline.
{
  const failing = parseLaunch("filesrc location=/nonexistent/wl2-gst-watch.bin ! fakesink");
  failing.watchBus({ onMessage: () => {} });
  failing.unwatchBus();
  const failure = new Promise((resolve) => {
    failing.watchBus({ onError: (m) => resolve(m) });
  });
  try { failing.play(); } catch (e) { /* state change may fail synchronously; the bus still errors */ }
  const failMsg = await failure;
  assert(failMsg && failMsg.type === "error" && typeof failMsg.message === "string",
    "watchBus onError message shape wrong");
  failing.close();
  // unwatchBus after close is a harmless no-op.
  failing.unwatchBus();
}

// watchBus argument validation.
{
  const wv = parseLaunch("videotestsrc num-buffers=1 ! fakesink");
  let badArgs = null;
  try { wv.watchBus({}); } catch (e) { badArgs = e.code; }
  assert(badArgs === "gstreamer_invalid_argument", "watchBus without callbacks should fail, got " + badArgs);
  wv.close();
}

// Dynamic pads: decodebin resolves its src pad at runtime. Only when a recorded
// file exists to decode.
if (teeHasFile) {
  const decode = parseLaunch("filesrc location=" + JSON.stringify(TEE_PATH) + " ! decodebin name=dec dec. ! queue ! videoconvert ! video/x-raw,format=RGBA,width=32,height=16 ! appsink name=wl2_video_sink sync=false");
  decode.attachVideoSink({ videoBufferName: TEE + "_dec", create: true, width: 32, height: 16, fps: 15, buffers: 4 });
  decode.play();
  assert(pollToEos(decode), "decodebin pipeline did not reach EOS");
  assert(decode.stats().wl2_video_sink.frames >= 1, "decodebin dynamic pad produced no frames");
  decode.close();
}
)JS";

    auto result = engine->runModule(runtime, "wl2-gstreamer-test.js", header + body);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    // --- Verify published ring contents from the owning process ---

    if (vid.value().sequence() <= 0) {
        return fail("video ring sequence did not advance");
    }
    auto vidFrame = vid.value().frame(vid.value().pointer(-1));
    if (!vidFrame || !frame_is_nonblank(vidFrame.value().data, vidFrame.value().size)) {
        return fail("video ring frame is blank");
    }

    if (aud.value().sequence() <= 0) {
        return fail("audio ring sequence did not advance");
    }
    auto audView = aud.value().buffer(aud.value().pointer(-1));
    if (!audView || !frame_is_nonblank(audView.value().data, static_cast<size_t>(aud.value().bufferSize()))) {
        return fail("audio ring buffer is silent");
    }

    // Round-trip: the pushed source frame should arrive byte-identical in dst
    // slot 0 (RGBA passes through videoconvert unchanged).
    if (dst.value().sequence() <= 0) {
        return fail("round-trip destination ring did not advance");
    }
    auto dstFrame = dst.value().frame(0);
    if (!dstFrame || !dstFrame.value().data) {
        return fail("could not read destination frame");
    }
    const size_t compareBytes = std::min(srcView.value().size, dstFrame.value().size);
    if (std::memcmp(srcView.value().data, dstFrame.value().data, compareBytes) != 0) {
        return fail("round-trip pixels do not match source");
    }

    if (!std::filesystem::exists(packetPath) || std::filesystem::file_size(packetPath) == 0) {
        return fail("recordPacketBuffer did not write output");
    }

    std::error_code ec;
    std::filesystem::remove(packetPath, ec);
    std::filesystem::remove(recordPath, ec);
    std::filesystem::remove(teePath, ec);
    std::filesystem::remove(snapshotPath, ec);

    std::cout << "wl2_gstreamer ok\n";
    return 0;
}

} // namespace

int main() {
    return run_gstreamer_tests();
}
