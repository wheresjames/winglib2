// Regression test for the wl2:webrtc high-level helpers (MediaSession,
// SignalingHub). Self-contained: no browser and no GStreamer. It exercises SDP
// offer generation, a full loopback (offer/answer/ICE + real RTP flow), and the
// SignalingHub authentication/session state machine with a fake transport.
//
// Run with a shared-memory prefix that covers both the send buffers and the
// default receive prefix, e.g. --shared-memory-allow /wl2_webrtc.

import { MediaSession, SignalingHub, PeerConnection } from "wl2:webrtc";
import { PacketBuffer } from "wl2:membus";

let failures = 0;
function check(label, cond) {
  if (!cond) { failures++; console.log("FAIL: " + label); }
}

const RECV_PREFIX = "/wl2_webrtc_recv";

// 1. Helpers loaded from the module.
check("MediaSession is a class", typeof MediaSession === "function");
check("SignalingHub is a class", typeof SignalingHub === "function");

// 2. start() without a track is rejected.
{
  const s = new MediaSession({ loopbackOnly: true });
  let threw = false;
  try { s.start(); } catch (e) { threw = true; }
  check("start() without tracks throws", threw);
  s.close();
}

// 3. Offer generation carries the video track.
{
  const name = "/wl2_webrtc_helpers_off_" + Date.now();
  const pb = PacketBuffer.create(name, 64, 1 << 20, 65536, {});
  let offer = null;
  const s = new MediaSession({
    loopbackOnly: true,
    receivePacketBufferNamePrefix: RECV_PREFIX,
    signal: (m) => { if (m.type === "offer") offer = m.description; },
  });
  s.addTrack({ media: "video", codec: "VP8", payloadType: 96, sendPacketBufferName: name });
  s.start();
  // Offer generation is asynchronous (local description + ICE setup) and the very
  // first one in a process can lag while libdatachannel spins up. Poll with a
  // generous, early-exiting budget (as the loopback section below does) so this
  // does not flake; it returns immediately once the offer is emitted.
  for (let i = 0; i < 300 && !offer; i++) s.pump({ timeoutMs: 10 });
  check("offer emitted", !!offer && offer.type === "offer");
  check("offer advertises VP8 video", !!offer && /m=video/.test(offer.sdp) && /VP8/i.test(offer.sdp));
  void pb;
  s.close();
}

// 4. Full loopback: MediaSession offerer <-> raw answerer, with real RTP flow.
{
  const name = "/wl2_webrtc_helpers_lb_" + Date.now();
  const pb = PacketBuffer.create(name, 128, 4 << 20, 262144, {});
  const b = PeerConnection.create({ loopbackOnly: true, receivePacketBufferNamePrefix: RECV_PREFIX });
  let inbound = null;
  const a = new MediaSession({
    loopbackOnly: true,
    receivePacketBufferNamePrefix: RECV_PREFIX,
    signal: (m) => {
      if (m.type === "offer") b.setRemoteDescription(m.description);
      else if (m.type === "candidate") b.addIceCandidate(m.candidate);
    },
  });
  a.addTrack({ media: "video", codec: "VP8", payloadType: 96, sendPacketBufferName: name });
  a.start();

  function rtpPacket(seq) {
    const p = new Uint8Array(20);
    p[0] = 0x80; p[1] = 0x60;
    p[2] = (seq >> 8) & 0xff; p[3] = seq & 0xff;
    p[8] = 0x11; p[9] = 0x22; p[10] = 0x33; p[11] = 0x44;
    p[12] = 0x10;
    return p.buffer;
  }

  let connected = false;
  for (let i = 0; i < 300; i++) {
    for (const ev of b.poll({ timeoutMs: 10, max: 64 })) {
      if (ev.type === "local-description") a.handleSignal({ type: "answer", description: ev.description });
      else if (ev.type === "local-candidate" && ev.candidate && ev.candidate.candidate) a.handleSignal({ type: "candidate", candidate: ev.candidate });
      else if (ev.type === "track") inbound = ev.track;
    }
    a.pump();
    if (!connected && a.pc.state().connection === "connected" && b.state().connection === "connected" && a.stats().trackOpen) {
      connected = true;
      for (let s = 1; s <= 8; s++) pb.write(rtpPacket(s), { kind: "data", track: 0, pts: s });
    }
    if (connected && inbound && inbound.isOpen() && inbound.stats().receivedPackets > 0) break;
  }
  check("loopback connected", a.pc.state().connection === "connected" && b.state().connection === "connected");
  check("sender track opened", a.stats().trackOpen === true);
  check("inbound track received RTP", inbound && inbound.isOpen() && inbound.stats().receivedPackets > 0);
  void pb;
  a.close(); b.close();
}

// 5. SignalingHub authentication + session lifecycle over a fake transport.
{
  function fakeConn(id, sink) {
    return { id, closed: false, send: (t) => sink.push(JSON.parse(t)), close() { this.closed = true; } };
  }

  const authHub = new SignalingHub({
    clientIceServers: [{ urls: "stun:stun.example:3478" }],
    authenticate: (hello) => (hello.token === "good" ? "user-1" : false),
  });
  const badSink = []; const bad = fakeConn(1, badSink);
  authHub.onMessage(bad, JSON.stringify({ type: "hello", token: "nope" }));
  check("bad token rejected", badSink.some((m) => m.type === "error") && bad.closed === true);

  const okSink = []; const ok = fakeConn(2, okSink);
  authHub.onMessage(ok, JSON.stringify({ type: "hello", token: "good" }));
  const welcome = okSink.find((m) => m.type === "welcome");
  check("good token welcomed", !!welcome && welcome.userId === "user-1" && welcome.iceServers.length === 1);

  const preSink = []; const pre = fakeConn(3, preSink);
  authHub.onMessage(pre, JSON.stringify({ type: "start", request: {} }));
  check("start before hello rejected", preSink.some((m) => m.type === "error"));

  const shm = "/wl2_webrtc_helpers_hub_" + Date.now();
  const hubPb = PacketBuffer.create(shm, 64, 1 << 20, 65536, {});
  let sessionSeen = null;
  const hub = new SignalingHub({
    receivePacketBufferNamePrefix: RECV_PREFIX,
    onSession: (session) => {
      sessionSeen = session;
      session.addTrack({ media: "video", codec: "VP8", payloadType: 96, sendPacketBufferName: shm });
    },
  });
  const sink = []; const c = fakeConn(4, sink);
  hub.onMessage(c, JSON.stringify({ type: "hello" }));            // no auth -> welcome
  hub.onMessage(c, JSON.stringify({ type: "start", request: {} }));
  for (let i = 0; i < 10; i++) hub.onMessage(c, JSON.stringify({ type: "pump" }));
  check("onSession invoked", sessionSeen !== null);
  check("hub emits offer to client", sink.some((m) => m.type === "offer"));
  void hubPb;
  hub.close();
}

if (failures !== 0) throw new Error(failures + " helper check(s) failed");
console.log("wl2:webrtc helpers ok");
