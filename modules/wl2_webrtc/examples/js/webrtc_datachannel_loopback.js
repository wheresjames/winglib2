import { PeerConnection, capabilities } from "wl2:webrtc";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const caps = capabilities();
assert(caps.dataChannel === true, "data channels are unavailable");
console.log(`wl2:webrtc backend=${caps.backend} provider=${caps.provider} tls=${caps.tlsBackend}`);

const a = PeerConnection.create({ loopbackOnly: true });
const b = PeerConnection.create({ loopbackOnly: true });
let bChannel = null;

function pump(src, dst) {
  for (const ev of src.poll({ timeoutMs: 20, max: 64 })) {
    if (ev.type === "local-description") dst.setRemoteDescription(ev.description);
    else if (ev.type === "local-candidate") dst.addIceCandidate(ev.candidate);
    else if (ev.type === "data-channel") bChannel = ev.channel;
    else if (ev.type === "error") throw new Error(ev.message);
  }
}

const aChannel = a.createDataChannel("chat");

for (let i = 0; i < 400; ++i) {
  pump(a, b);
  pump(b, a);
  if (a.state().connection === "connected" &&
      b.state().connection === "connected" &&
      aChannel.isOpen() && bChannel && bChannel.isOpen()) {
    break;
  }
}

assert(bChannel && aChannel.isOpen() && bChannel.isOpen(), "data channel did not open");
console.log(`connected: a=${a.state().connection} b=${b.state().connection} channel=${aChannel.label()}`);

aChannel.send("ping");
let reply = null;
let received = null;
for (let i = 0; i < 200 && reply === null; ++i) {
  pump(a, b);
  pump(b, a);
  const messages = bChannel.poll({ timeoutMs: 20, max: 8 });
  if (messages.length) {
    assert(messages[0].data === "ping", "unexpected payload");
    received = messages[0].data;
    bChannel.send("pong");
  }
  const back = aChannel.poll({ timeoutMs: 20, max: 8 });
  if (back.length) reply = back[0].data;
}

assert(reply === "pong", "loopback reply failed");
console.log(`message: a->b=${received} b->a=${reply}`);
console.log(`stats: a=${JSON.stringify(a.stats())} b=${JSON.stringify(b.stats())}`);
a.close();
b.close();
console.log("wl2:webrtc datachannel loopback ok");
