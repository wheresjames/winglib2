import { PacketBuffer } from "wl2:membus";
import { parseLaunch } from "wl2:gstreamer";
import { PeerConnection } from "wl2:webrtc";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function pollPipeline(pipeline, iterations = 20) {
  for (let i = 0; i < iterations; ++i) {
    for (const message of pipeline.busPoll({ timeoutMs: 20, max: 16 })) {
      if (message.type === "error") throw new Error(message.message || "gstreamer error");
      if (message.type === "eos") return true;
    }
  }
  return false;
}

function pumpSignaling(src, dst, onTrack) {
  for (const ev of src.poll({ timeoutMs: 20, max: 64 })) {
    if (ev.type === "local-description") dst.setRemoteDescription(ev.description);
    else if (ev.type === "local-candidate") dst.addIceCandidate(ev.candidate);
    else if (ev.type === "track") onTrack(ev);
    else if (ev.type === "error") throw new Error(ev.message);
  }
}

function rtpPacket(seq) {
  const p = new Uint8Array(16);
  p[0] = 0x80;
  p[1] = 0x80 | 96;
  p[2] = (seq >> 8) & 0xff;
  p[3] = seq & 0xff;
  p[7] = seq;
  p[8] = 0x12; p[9] = 0x34; p[10] = 0x56; p[11] = 0x78;
  p[12] = 0xaa; p[13] = 0xbb; p[14] = 0xcc; p[15] = seq;
  return p;
}

const sendName = "/wl2_webrtc_gst_interop_out";
const caps = "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000";
const send = PacketBuffer.create(sendName, 8, 65536, 2048, { metadata: caps });
send.write(rtpPacket(1).buffer, {
  kind: "video",
  pts: 0,
  metadata: JSON.stringify({ schema: 1, mediaType: "video", codec: "vp8", streamFormat: "rtp" })
});

const a = PeerConnection.create({ loopbackOnly: true, receivePacketBufferNamePrefix: "/wl2_webrtc_gst_recv" });
const b = PeerConnection.create({ loopbackOnly: true, receivePacketBufferNamePrefix: "/wl2_webrtc_gst_recv" });
let inbound = null;
let recvName = null;

const outbound = a.addTrack({
  media: "video",
  codec: "VP8",
  payloadType: 96,
  clockRate: 90000,
  sendPacketBufferName: sendName
});

for (let i = 0; i < 500 && !(inbound && inbound.isOpen() && outbound.isOpen()); ++i) {
  pumpSignaling(a, b, () => {});
  pumpSignaling(b, a, (ev) => {
    inbound = ev.track;
    recvName = ev.packetBufferName;
  });
}
assert(inbound && inbound.isOpen(), "inbound WebRTC track did not open");

let sent = 0;
for (let i = 0; i < 100 && sent < 1; ++i) {
  pumpSignaling(a, b, () => {});
  pumpSignaling(b, a, () => {});
  sent += outbound.pump({ timeoutMs: 20, max: 1 }).sent;
}
assert(sent === 1, "WebRTC track did not send RTP packet");

const received = PacketBuffer.openExisting(recvName);
assert(received.wait(0, { timeoutMs: 1000 }), "WebRTC receive PacketBuffer stayed empty");

const pipeline = parseLaunch("appsrc name=wl2_packet_src caps=application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000 ! fakesink");
pipeline.attachPacketSource({ packetBufferName: recvName, caps });
const pushed = pipeline.pushPacket({ waitTimeoutMs: 1 });
assert(pushed.ok && pushed.bytes === 16, "GStreamer did not push received RTP packet");
pipeline.endOfStream();
pipeline.play();
assert(pollPipeline(pipeline), "GStreamer packet pipeline did not reach EOS");

pipeline.close();
outbound.close();
inbound.close();
a.close();
b.close();
send.close();
received.close();
console.log("wl2:webrtc gstreamer PacketBuffer interop ok");
