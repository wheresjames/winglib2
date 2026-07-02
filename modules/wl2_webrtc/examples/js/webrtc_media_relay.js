import { PacketBuffer } from "wl2:membus";
import { PeerConnection } from "wl2:webrtc";

// Run with shared-memory permission, for example:
//   wl2 run --allow-shared-memory --shared-memory-allow /wl2_webrtc \
//     modules/wl2_webrtc/examples/js/webrtc_media_relay.js

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function rtpPacket(seq, marker) {
  const p = new Uint8Array(16);
  p[0] = 0x80;                    // RTP v2
  p[1] = (marker ? 0x80 : 0) | 96; // marker + dynamic VP8 payload type
  p[2] = (seq >> 8) & 0xff;
  p[3] = seq & 0xff;
  p[7] = seq;                     // tiny demo timestamp
  p[8] = 0x12; p[9] = 0x34; p[10] = 0x56; p[11] = 0x78; // SSRC
  p[12] = 0xa0 + seq; p[13] = 0xb0 + seq; p[14] = 0xc0 + seq; p[15] = 0xd0 + seq;
  return p;
}

const sendName = "/wl2_webrtc_media_relay_out";
const send = PacketBuffer.create(sendName, 16, 65536, 2048, {
  metadata: JSON.stringify({ schema: 1, mediaType: "video", codec: "vp8", streamFormat: "rtp" })
});

const packets = [rtpPacket(1, false), rtpPacket(2, true), rtpPacket(3, true)];
for (let i = 0; i < packets.length; ++i) {
  send.write(packets[i].buffer, {
    kind: "video",
    pts: (i + 1) * 3000,
    metadata: JSON.stringify({ schema: 1, mediaType: "video", codec: "vp8", streamFormat: "rtp" })
  });
}
console.log(`prepared RTP packets: ${packets.length}`);

const a = PeerConnection.create({
  loopbackOnly: true,
  receivePacketBufferNamePrefix: "/wl2_webrtc_media_relay_recv"
});
const b = PeerConnection.create({
  loopbackOnly: true,
  receivePacketBufferNamePrefix: "/wl2_webrtc_media_relay_recv"
});

let inboundTrack = null;
let receiveName = null;

function pumpSignaling(src, dst) {
  for (const ev of src.poll({ timeoutMs: 20, max: 64 })) {
    if (ev.type === "local-description") dst.setRemoteDescription(ev.description);
    else if (ev.type === "local-candidate") dst.addIceCandidate(ev.candidate);
    else if (ev.type === "track") {
      inboundTrack = ev.track;
      receiveName = ev.packetBufferName;
      console.log(`inbound ${ev.media} track -> ${receiveName}`);
    } else if (ev.type === "error") {
      throw new Error(ev.message);
    }
  }
}

const outboundTrack = a.addTrack({
  media: "video",
  codec: "VP8",
  payloadType: 96,
  clockRate: 90000,
  sendPacketBufferName: sendName
});

for (let i = 0; i < 500; ++i) {
  pumpSignaling(a, b);
  pumpSignaling(b, a);
  if (a.state().connection === "connected" && b.state().connection === "connected" &&
      outboundTrack.isOpen() && inboundTrack && inboundTrack.isOpen()) {
    break;
  }
}
assert(inboundTrack && inboundTrack.isOpen(), "WebRTC media track did not open");
console.log(`connected: a=${a.state().connection} b=${b.state().connection}`);

let sent = 0;
for (let i = 0; i < 100 && sent < packets.length; ++i) {
  pumpSignaling(a, b);
  pumpSignaling(b, a);
  sent += outboundTrack.pump({ timeoutMs: 20, max: packets.length }).sent;
}
assert(sent === packets.length, `expected to send ${packets.length}, sent ${sent}`);

const received = PacketBuffer.openExisting(receiveName);
let latest = null;
for (let i = 0; i < 200; ++i) {
  pumpSignaling(a, b);
  pumpSignaling(b, a);
  if (received.wait(0, { timeoutMs: 20 })) latest = received.latest();
  if (latest && new Uint8Array(latest.payload.arrayBuffer())[3] === 3) break;
}

assert(latest, "no RTP packet arrived");
const bytes = new Uint8Array(latest.payload.arrayBuffer());
const metadata = JSON.parse(latest.metadata.text());
console.log(`relayed packets: sent=${outboundTrack.stats().sentPackets} received=${inboundTrack.stats().receivedPackets}`);
console.log(`latest RTP: payloadType=${bytes[1] & 0x7f} sequence=${(bytes[2] << 8) | bytes[3]} codec=${metadata.codec}`);

outboundTrack.close();
inboundTrack.close();
a.close();
b.close();
send.close();
received.close();
console.log("wl2:webrtc media relay ok");
