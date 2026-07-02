import { PeerConnection, WebSocket, SignalingServer, capabilities } from "wl2:webrtc";

// Run with loopback network and listen permission, for example:
//   wl2 run --allow-network --network-allow '127.0.0.1:*' \
//     --allow-listen --listen-allow '127.0.0.1:*' \
//     modules/wl2_webrtc/examples/js/webrtc_signaling_server.js

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function permissionHint(error) {
  if (error && error.code === "webrtc_permission_denied") {
    console.log("This example opens a loopback WebSocket signaling server and two loopback clients.");
    console.log("Run it with:");
    console.log("  wl2 run --allow-network --network-allow '127.0.0.1:*' \\");
    console.log("    --allow-listen --listen-allow '127.0.0.1:*' \\");
    console.log("    modules/wl2_webrtc/examples/js/webrtc_signaling_server.js");
  }
  throw error;
}

function waitForSocketOpen(ws, label) {
  for (let i = 0; i < 200; ++i) {
    for (const ev of ws.poll({ timeoutMs: 20, max: 32 })) {
      if (ev.type === "open") return;
      if (ev.type === "error") throw new Error(`${label}: ${ev.detail}`);
    }
  }
  throw new Error(`${label}: websocket did not open`);
}

const caps = capabilities();
assert(caps.websocket === true, "WebSocket signaling is unavailable");
assert(caps.dataChannel === true, "data channels are unavailable");
console.log(`wl2:webrtc backend=${caps.backend} provider=${caps.provider} websocket=${caps.websocket}`);

let server = null;
try {
  server = SignalingServer.listen({ host: "127.0.0.1", port: 0, path: "/signal" });
} catch (e) {
  permissionHint(e);
}
const port = server.port();
console.log(`signaling server: ws://127.0.0.1:${port}/signal`);

let sigA = null;
let sigB = null;
try {
  sigA = WebSocket.connect({ url: `ws://127.0.0.1:${port}/signal` });
  sigB = WebSocket.connect({ url: `ws://127.0.0.1:${port}/signal` });
} catch (e) {
  permissionHint(e);
}
waitForSocketOpen(sigA, "client A");
waitForSocketOpen(sigB, "client B");
console.log("signaling clients: connected");

sigA.send(JSON.stringify({ from: "a", hello: true }));
sigB.send(JSON.stringify({ from: "b", hello: true }));

const routes = {};
let relayedSignals = 0;
function pumpBroker() {
  for (const ev of server.poll({ timeoutMs: 5, max: 64 })) {
    if (ev.type === "client-connected") {
      console.log(`broker: client ${ev.clientId} connected path=${ev.detail || ""}`);
    } else if (ev.type === "client-disconnected") {
      console.log(`broker: client ${ev.clientId} disconnected`);
    } else if (ev.type === "message") {
      const msg = JSON.parse(ev.data);
      if (msg.from) routes[msg.from] = ev.clientId;
      if (msg.to && routes[msg.to]) {
        server.send(routes[msg.to], ev.data);
        relayedSignals += 1;
      }
    } else if (ev.type === "error") {
      throw new Error(ev.detail);
    }
  }
}

for (let i = 0; i < 200 && (!routes.a || !routes.b); ++i) pumpBroker();
assert(routes.a && routes.b, "broker did not learn both clients");
console.log(`broker routes: a=${routes.a} b=${routes.b}`);

const peerA = PeerConnection.create({ loopbackOnly: true });
const peerB = PeerConnection.create({ loopbackOnly: true });
let peerBChannel = null;

function signal(ws, from, to, payload) {
  ws.send(JSON.stringify({ from, to, signal: payload }));
}

function pumpPeer(peer, ws, from, to) {
  for (const ev of peer.poll({ timeoutMs: 5, max: 64 })) {
    if (ev.type === "local-description") signal(ws, from, to, { description: ev.description });
    else if (ev.type === "local-candidate") signal(ws, from, to, { candidate: ev.candidate });
    else if (ev.type === "data-channel") peerBChannel = ev.channel;
    else if (ev.type === "error") throw new Error(ev.message);
  }
}

function pumpSocket(ws, peer) {
  for (const ev of ws.poll({ timeoutMs: 5, max: 64 })) {
    if (ev.type === "message") {
      const msg = JSON.parse(ev.data);
      if (msg.signal.description) peer.setRemoteDescription(msg.signal.description);
      else if (msg.signal.candidate) peer.addIceCandidate(msg.signal.candidate);
    } else if (ev.type === "error") {
      throw new Error(ev.detail);
    }
  }
}

const peerAChannel = peerA.createDataChannel("brokered-chat");
for (let i = 0; i < 500; ++i) {
  pumpPeer(peerA, sigA, "a", "b");
  pumpPeer(peerB, sigB, "b", "a");
  pumpBroker();
  pumpSocket(sigA, peerA);
  pumpSocket(sigB, peerB);
  if (peerA.state().connection === "connected" &&
      peerB.state().connection === "connected" &&
      peerAChannel.isOpen() &&
      peerBChannel && peerBChannel.isOpen()) {
    break;
  }
}

assert(peerBChannel && peerAChannel.isOpen() && peerBChannel.isOpen(),
       "brokered data channel did not open");
console.log(`peers connected through broker: relayedSignals=${relayedSignals}`);

peerAChannel.send("ping over brokered signaling");
let received = null;
for (let i = 0; i < 200 && received === null; ++i) {
  pumpPeer(peerA, sigA, "a", "b");
  pumpPeer(peerB, sigB, "b", "a");
  pumpBroker();
  pumpSocket(sigA, peerA);
  pumpSocket(sigB, peerB);
  const messages = peerBChannel.poll({ timeoutMs: 20, max: 8 });
  if (messages.length) received = messages[0].data;
}

assert(received === "ping over brokered signaling", "unexpected data-channel payload");
console.log(`data channel message: ${received}`);
console.log(`states: a=${peerA.state().connection} b=${peerB.state().connection}`);

peerAChannel.close();
peerBChannel.close();
peerA.close();
peerB.close();
sigA.close();
sigB.close();
server.close();
console.log("wl2:webrtc signaling server ok");
