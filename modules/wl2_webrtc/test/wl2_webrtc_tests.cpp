#include "wl2/wl2.h"
#include "wl2_membus/wl2_membus.h"
#include "wl2_webrtc/wl2_webrtc.h"

#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_webrtc test failed: " << message << '\n';
    return 1;
}

// Two in-process PeerConnections negotiate over loopback host candidates with
// the test itself acting as the signaling channel, then exchange data-channel
// messages both ways. This exercises the real ICE/DTLS/SCTP stack with no
// browser and no external signaling server.
int run_webrtc_tests() {
    wl2::RuntimeOptions options;
    options.allowNetwork = true;
    options.networkAllowList = {"127.0.0.1:*"};
    options.allowListening = true;
    options.listenAllowList = {"127.0.0.1:*"};
    options.allowSharedMemory = true;
    options.sharedMemoryAllowList = {"/wl2_webrtc"};
    options.staticModules.push_back(wl2_membus_register_module);
    options.staticModules.push_back(wl2_webrtc_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return fail("runtime initialize failed: " + init.error().message());
    }
    auto engine = wl2::createConfiguredJsEngine();

    const std::string body = R"JS(
import { PacketBuffer } from "wl2:membus";
import { version, capabilities, PeerConnection, WebSocket, SignalingServer } from "wl2:webrtc";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// --- Capability + version surface ---
const v = version();
assert(v.module && v.libdatachannel, "version shape wrong");
const caps = capabilities();
assert(caps.dataChannel === true, "dataChannel capability missing");
assert(caps.websocket === true, "websocket capability missing");
assert(caps.backend === "libdatachannel", "backend wrong: " + caps.backend);
assert(typeof caps.provider === "string" && caps.provider.length > 0,
       "provider missing");

// --- Network authorization is enforced for configured ICE servers ---
let denied = null;
try { PeerConnection.create({ stunServer: "stun:203.0.113.1:3478" }); }
catch (e) { denied = e.code; }
assert(denied === "webrtc_permission_denied", "external STUN should be denied, got " + denied);

let deniedListen = null;
try { SignalingServer.listen({ host: "0.0.0.0", port: 45678 }); }
catch (e) { deniedListen = e.code; }
assert(deniedListen === "webrtc_permission_denied",
       "external listen should be denied, got " + deniedListen);

function waitForSocketOpen(ws) {
  for (let i = 0; i < 200; ++i) {
    for (const ev of ws.poll({ timeoutMs: 20, max: 32 })) {
      if (ev.type === "open") return { open: true };
      if (ev.type === "error") return { open: false, error: ev.detail || "" };
    }
  }
  return { open: false, error: "timed out waiting for open" };
}

function connectWebSocket(url, label) {
  let lastError = "";
  for (let attempt = 0; attempt < 20; ++attempt) {
    const ws = WebSocket.connect({ url });
    const opened = waitForSocketOpen(ws);
    if (opened.open) return ws;
    lastError = opened.error;
    ws.close();
    if (!String(lastError).includes("TCP connection failed")) break;
  }
  throw new Error(label + " websocket error: " + lastError);
}

function waitForServerClient(server) {
  for (let i = 0; i < 200; ++i) {
    for (const ev of server.poll({ timeoutMs: 20, max: 32 })) {
      if (ev.type === "client-connected") return ev.clientId;
      if (ev.type === "error") throw new Error("server websocket error: " + ev.detail);
    }
  }
  return 0;
}

// --- Built-in WebSocket signaling transport ---
const server = SignalingServer.listen({ host: "127.0.0.1", port: 0, path: "/ws" });
const serverPort = server.port();
assert(serverPort > 0, "server did not report a bound port");
const ws = connectWebSocket("ws://127.0.0.1:" + serverPort + "/ws", "client");
const serverClient = waitForServerClient(server);
assert(serverClient > 0, "server did not report connected client");

ws.send("hello");
let serverGot = null;
for (let i = 0; i < 200 && serverGot === null; ++i) {
  for (const ev of server.poll({ timeoutMs: 20, max: 32 })) {
    if (ev.type === "message" && ev.clientId === serverClient) serverGot = ev.data;
  }
}
assert(serverGot === "hello", "server did not receive websocket message: " + JSON.stringify(serverGot));

server.send(serverClient, "world");
let clientGot = null;
for (let i = 0; i < 200 && clientGot === null; ++i) {
  for (const ev of ws.poll({ timeoutMs: 20, max: 32 })) {
    if (ev.type === "message") clientGot = ev.data;
  }
}
assert(clientGot === "world", "client did not receive websocket message: " + JSON.stringify(clientGot));

ws.close();
let disconnected = false;
for (let i = 0; i < 200 && !disconnected; ++i) {
  for (const ev of server.poll({ timeoutMs: 20, max: 32 })) {
    if (ev.type === "client-disconnected" && ev.clientId === serverClient) disconnected = true;
  }
}
assert(disconnected, "server did not report disconnected websocket client");
let staleSendCode = null;
try { server.send(serverClient, "after-close"); } catch (e) { staleSendCode = e.code; }
assert(staleSendCode === "webrtc_invalid_argument",
       "server send to disconnected client should fail, got " + staleSendCode);
server.close();

// --- Two peers negotiate through the built-in WebSocket signaling broker ---
const broker = SignalingServer.listen({ host: "127.0.0.1", port: 0, path: "/signal" });
const brokerPort = broker.port();
const sigA = connectWebSocket("ws://127.0.0.1:" + brokerPort + "/signal", "signaling client A");
const sigB = connectWebSocket("ws://127.0.0.1:" + brokerPort + "/signal", "signaling client B");

sigA.send(JSON.stringify({ from: "a", hello: true }));
sigB.send(JSON.stringify({ from: "b", hello: true }));

const signalRoutes = {};
function pumpBroker() {
  for (const ev of broker.poll({ timeoutMs: 5, max: 64 })) {
    if (ev.type === "message") {
      const msg = JSON.parse(ev.data);
      if (msg.from) signalRoutes[msg.from] = ev.clientId;
      if (msg.to && signalRoutes[msg.to]) broker.send(signalRoutes[msg.to], ev.data);
    } else if (ev.type === "error") {
      throw new Error("broker error: " + ev.detail);
    }
  }
}

for (let i = 0; i < 200 && (!signalRoutes.a || !signalRoutes.b); ++i) pumpBroker();
assert(signalRoutes.a && signalRoutes.b, "broker did not learn both signaling clients");

const brokerA = PeerConnection.create({ loopbackOnly: true });
const brokerB = PeerConnection.create({ loopbackOnly: true });
let brokerBChannel = null;

function sendSignal(ws, from, to, signal) {
  ws.send(JSON.stringify({ from, to, signal }));
}

function pumpPeerSignals(pc, ws, from, to) {
  for (const ev of pc.poll({ timeoutMs: 5, max: 64 })) {
    if (ev.type === "local-description") sendSignal(ws, from, to, { description: ev.description });
    else if (ev.type === "local-candidate") sendSignal(ws, from, to, { candidate: ev.candidate });
    else if (ev.type === "data-channel") brokerBChannel = ev.channel;
    else if (ev.type === "error") throw new Error("brokered peer error: " + ev.message);
  }
}

function pumpSocketSignals(ws, pc) {
  for (const ev of ws.poll({ timeoutMs: 5, max: 64 })) {
    if (ev.type === "message") {
      const signal = JSON.parse(ev.data).signal;
      if (signal.description) pc.setRemoteDescription(signal.description);
      else if (signal.candidate) pc.addIceCandidate(signal.candidate);
    } else if (ev.type === "error") {
      throw new Error("signaling websocket error: " + ev.detail);
    }
  }
}

const brokerAChannel = brokerA.createDataChannel("brokered-chat");
let brokeredConnected = false;
for (let i = 0; i < 500 && !brokeredConnected; ++i) {
  pumpPeerSignals(brokerA, sigA, "a", "b");
  pumpPeerSignals(brokerB, sigB, "b", "a");
  pumpBroker();
  pumpSocketSignals(sigA, brokerA);
  pumpSocketSignals(sigB, brokerB);
  brokeredConnected = brokerA.state().connection === "connected" &&
                      brokerB.state().connection === "connected" &&
                      brokerAChannel.isOpen() &&
                      brokerBChannel && brokerBChannel.isOpen();
}
assert(brokeredConnected, "peers did not connect through the signaling broker");

brokerAChannel.send("brokered-ping");
let brokeredGot = null;
for (let i = 0; i < 200 && brokeredGot === null; ++i) {
  pumpPeerSignals(brokerA, sigA, "a", "b");
  pumpPeerSignals(brokerB, sigB, "b", "a");
  pumpBroker();
  pumpSocketSignals(sigA, brokerA);
  pumpSocketSignals(sigB, brokerB);
  const msgs = brokerBChannel.poll({ timeoutMs: 20, max: 8 });
  if (msgs.length) brokeredGot = msgs[0].data;
}
assert(brokeredGot === "brokered-ping",
       "brokered peer did not receive data-channel message: " + JSON.stringify(brokeredGot));
const brokerStats = brokerA.stats();
assert(typeof brokerStats.bytesSent === "number", "peer stats should include bytesSent");
assert(typeof brokerStats.bytesReceived === "number", "peer stats should include bytesReceived");
assert("rttMs" in brokerStats, "peer stats should include rttMs");
assert("selectedCandidatePair" in brokerStats, "peer stats should include selectedCandidatePair");

brokerAChannel.close();
brokerBChannel.close();
brokerA.close();
brokerB.close();
sigA.close();
sigB.close();
broker.close();

// --- Two-peer loopback data-channel round-trip ---
const a = PeerConnection.create({ loopbackOnly: true });
const b = PeerConnection.create({ loopbackOnly: true });

let bChannel = null;
// Forward one peer's signaling events to the other; capture an inbound channel.
function pump(src, dst) {
  for (const ev of src.poll({ timeoutMs: 20, max: 64 })) {
    if (ev.type === "local-description") dst.setRemoteDescription(ev.description);
    else if (ev.type === "local-candidate") dst.addIceCandidate(ev.candidate);
    else if (ev.type === "data-channel") bChannel = ev.channel;
    else if (ev.type === "error") throw new Error("peer error: " + ev.message);
  }
}

// Creating the channel on A triggers the offer (libdatachannel auto-negotiates).
const aChannel = a.createDataChannel("chat");

let connected = false;
for (let i = 0; i < 400 && !connected; ++i) {
  pump(a, b);
  pump(b, a);
  connected = a.state().connection === "connected" &&
              b.state().connection === "connected" &&
              aChannel.isOpen() && bChannel && bChannel.isOpen();
}
assert(connected, "peers did not connect / open the data channel");

// A -> B
aChannel.send("ping");
let got = null;
for (let i = 0; i < 200 && got === null; ++i) {
  pump(a, b); pump(b, a);
  const msgs = bChannel.poll({ timeoutMs: 20, max: 8 });
  if (msgs.length) got = msgs[0].data;
}
assert(got === "ping", "B did not receive 'ping', got " + JSON.stringify(got));

// B -> A
bChannel.send("pong");
let back = null;
for (let i = 0; i < 200 && back === null; ++i) {
  pump(a, b); pump(b, a);
  const msgs = aChannel.poll({ timeoutMs: 20, max: 8 });
  if (msgs.length) back = msgs[0].data;
}
assert(back === "pong", "A did not receive 'pong', got " + JSON.stringify(back));

// Binary round-trip A -> B
aChannel.send(new Uint8Array([1, 2, 3, 4]).buffer);
let bin = null;
for (let i = 0; i < 200 && bin === null; ++i) {
  pump(a, b); pump(b, a);
  const msgs = bChannel.poll({ timeoutMs: 20, max: 8 });
  if (msgs.length) bin = msgs[0];
}
assert(bin && bin.binary === true, "B did not receive a binary message");
assert(new Uint8Array(bin.data).length === 4, "binary payload length wrong");

aChannel.close();
bChannel.close();
a.close();
b.close();

// closed peers reject further use with a stable code
let closedCode = null;
try { a.createDataChannel("late"); } catch (e) { closedCode = e.code; }
assert(closedCode === "webrtc_closed", "closed peer should reject, got " + closedCode);

// --- RTP media track relay through PacketBuffer ---
const sendName = "/wl2_webrtc_test_rtp_out";
const out = PacketBuffer.create(sendName, 16, 65536, 2048, {
  metadata: JSON.stringify({ schema: 1, mediaType: "video", codec: "vp8", streamFormat: "rtp" })
});

function rtpPacket(seq, marker) {
  const p = new Uint8Array(16);
  p[0] = 0x80;
  p[1] = (marker ? 0x80 : 0) | 96;
  p[2] = (seq >> 8) & 0xff;
  p[3] = seq & 0xff;
  p[4] = 0; p[5] = 0; p[6] = 0; p[7] = seq;
  p[8] = 0x12; p[9] = 0x34; p[10] = 0x56; p[11] = 0x78;
  p[12] = 0xa0 + seq; p[13] = 0xb0 + seq; p[14] = 0xc0 + seq; p[15] = 0xd0 + seq;
  return p;
}

const firstPacket = rtpPacket(1, false);
const secondPacket = rtpPacket(2, true);
out.write(firstPacket.buffer, {
  kind: "video",
  track: 7,
  pts: 1000,
  metadata: JSON.stringify({ schema: 1, mediaType: "video", codec: "vp8", streamFormat: "rtp", payloadType: 96 })
});
out.write(secondPacket.buffer, {
  kind: "video",
  track: 7,
  pts: 2000,
  metadata: JSON.stringify({ schema: 1, mediaType: "video", codec: "vp8", streamFormat: "rtp", payloadType: 96 })
});

const c = PeerConnection.create({ loopbackOnly: true, receivePacketBufferNamePrefix: "/wl2_webrtc_recv_test" });
const d = PeerConnection.create({ loopbackOnly: true, receivePacketBufferNamePrefix: "/wl2_webrtc_recv_test" });
let inboundTrack = null;
let recvName = null;

function pumpMedia(src, dst) {
  for (const ev of src.poll({ timeoutMs: 20, max: 64 })) {
    if (ev.type === "local-description") dst.setRemoteDescription(ev.description);
    else if (ev.type === "local-candidate") dst.addIceCandidate(ev.candidate);
    else if (ev.type === "track") {
      inboundTrack = ev.track;
      recvName = ev.packetBufferName;
      assert(ev.media === "video", "inbound media should be video");
    } else if (ev.type === "error") throw new Error("media peer error: " + ev.message);
  }
}

const sendTrack = c.addTrack({
  media: "video",
  codec: "VP8",
  payloadType: 96,
  clockRate: 90000,
  track: 7,
  sendPacketBufferName: sendName
});

let mediaConnected = false;
for (let i = 0; i < 500 && !mediaConnected; ++i) {
  pumpMedia(c, d);
  pumpMedia(d, c);
  mediaConnected = c.state().connection === "connected" &&
                   d.state().connection === "connected" &&
                   sendTrack.isOpen() && inboundTrack && inboundTrack.isOpen();
}
assert(mediaConnected, "media peers did not connect / open tracks");

let pumped = { sent: 0 };
for (let i = 0; i < 100 && pumped.sent < 2; ++i) {
  pumpMedia(c, d);
  pumpMedia(d, c);
  const now = sendTrack.pump({ timeoutMs: 20, max: 2 });
  pumped.sent += now.sent;
}
assert(pumped.sent === 2, "send track did not pump two RTP packets, got " + pumped.sent);

const received = PacketBuffer.openExisting(recvName);
let rec = null;
for (let i = 0; i < 200 && rec === null; ++i) {
  pumpMedia(c, d);
  pumpMedia(d, c);
  if (received.wait(0, { timeoutMs: 20 })) {
    const latest = received.latest();
    const bytes = new Uint8Array(latest.payload.arrayBuffer());
    if (bytes.length === 16 && bytes[2] === 0 && bytes[3] === 2) rec = latest;
  }
}
assert(rec !== null, "remote PacketBuffer did not receive the second RTP packet");
assert(rec.kind === "video", "received packet kind wrong: " + rec.kind);
assert(rec.track === 0, "received track id default wrong: " + rec.track);
assert(JSON.parse(rec.metadata.text()).codec.toLowerCase() === "vp8", "received metadata codec wrong");

sendTrack.close();
inboundTrack.close();
c.close();
d.close();
out.close();
received.close();
)JS";

    auto result = engine->runModule(runtime, "wl2-webrtc-test.js", body);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    std::cout << "wl2_webrtc ok\n";
    return 0;
}

} // namespace

int main() {
    return run_webrtc_tests();
}
