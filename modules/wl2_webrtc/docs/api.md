# wl2:webrtc API

`wl2:webrtc` exposes the libdatachannel WebRTC transport to JavaScript:
`PeerConnection`, data channels, RTP media tracks, and a built-in WebSocket
client/server for signaling.

```js
import {
  version, capabilities,
  PeerConnection, WebSocket, SignalingServer,
  MediaSession, SignalingHub,
} from "wl2:webrtc";
```

`PeerConnection`, `WebSocket`, and `SignalingServer` are the low-level
primitives. `MediaSession` and `SignalingHub` are higher-level JavaScript
helpers (see below) that remove the offer/answer/ICE boilerplate for the common
"stream media to a browser" case.

## Module Functions

- `version() -> { module, libdatachannel }`
- `capabilities() -> { provider, backend, tlsBackend, media, websocket, dataChannel }`

`provider` is the Winglib2 dependency provider that satisfied libdatachannel
(`local`, `package`, or `fetch`). `backend` is currently always
`libdatachannel`.

## PeerConnection

```js
const pc = PeerConnection.create({
  stunServer: "stun:host:3478",
  turnServers: ["turn:user:pass@host:3478"],
  iceTransportPolicy: "all",
  loopbackOnly: false,
  disableAutoNegotiation: false
});
```

`loopbackOnly:true` binds host candidates to `127.0.0.1` and does not configure
STUN/TURN. It is intended for deterministic tests and local deployments.

`disableAutoNegotiation:true` stops libdatachannel from auto-generating
offers/answers. The caller then drives negotiation explicitly with
`setLocalDescription()`. This is required for a deterministic manual answerer
(`setRemoteDescription` -> `addTrack({ mid })` -> `setLocalDescription("answer")`)
and for emitting exactly one offer after adding several tracks.

Methods:

- `createDataChannel(label) -> DataChannel`
- `addTrack({ media, codec, payloadType, clockRate?, track?, sendPacketBufferName, mid?, autoOffer? }) -> Track`
  - `autoOffer` (default `true`): when the connection is `stable`, immediately
    generate an offer. Set `false` to add several tracks and offer once, or to
    answer a remote offer.
  - `mid`: bind the send track to a specific m-line id (default `"video"` /
    `"audio"`). Use the remote offer's mid so an answerer can send on the
    negotiated line.
- `setRemoteDescription({ type, sdp }) -> { ok: true }`
- `setLocalDescription("offer" | "answer") -> { ok: true }`
- `addIceCandidate({ candidate, sdpMid }) -> { ok: true }`
- `localDescription() -> { type, sdp } | null`
- `state() -> { connection, ice, gathering, signaling }`
- `poll({ timeoutMs = 0, max = 128 } = {}) -> event[]`
- `stats() -> { pendingEvents, droppedEvents, bytesSent, bytesReceived, rttMs, selectedCandidatePair, closed }`
- `close()`

`poll()` drains events marshaled from libdatachannel callback threads:

- `{ type: "local-description", description: { type, sdp } }`
- `{ type: "local-candidate", candidate: { candidate, sdpMid } }`
- `{ type: "state-change", connection, ice, gathering, signaling }`
- `{ type: "data-channel", label, channel }`
- `{ type: "track", media, packetBufferName, track }`
- `{ type: "error", message }`

## DataChannel

Methods:

- `send(data) -> { ok: true, buffered }`, where `data` is a string, ArrayBuffer,
  or typed array view
- `poll({ timeoutMs = 0, max = 256 } = {}) -> message[]`
- `label() -> string`
- `isOpen() -> boolean`
- `bufferedAmount() -> number`
- `close()`

Data-channel messages are returned as `{ data, binary }`. Binary messages carry
an `ArrayBuffer`; text messages carry a string.

## Track

`addTrack()` sends RTP packets from an existing `PacketBuffer`:

```js
const track = pc.addTrack({
  media: "video",
  codec: "VP8",
  payloadType: 96,
  clockRate: 90000,
  track: 0,
  sendPacketBufferName: "/rtp_out"
});
```

Methods:

- `pump({ timeoutMs = 0, max = 64 } = {}) -> { sent, bytes, lastSequence }`
- `stats() -> { sentPackets, sentBytes, receivedPackets, receivedBytes, droppedPackets, sendErrors, closed }`
- `packetBufferName() -> string`
- `mid() -> string`
- `isOpen() -> boolean`
- `close()`

`pump()` scans the send `PacketBuffer` for records newer than the last pumped
sequence, sorted by sequence, and copies their payload bytes into the
libdatachannel track as RTP packets.

Inbound tracks arrive through `PeerConnection.poll()` as `track` events. The
module creates a receive `PacketBuffer`, writes inbound RTP payloads into it,
and returns its name as `packetBufferName`. The receive name prefix defaults to
`/wl2_webrtc_recv` and can be changed at peer creation:

```js
const pc = PeerConnection.create({
  loopbackOnly: true,
  receivePacketBufferNamePrefix: "/my_webrtc_recv"
});
```

RTP packet metadata is written as the shared Winglib2 media JSON shape with
`schema:1`, `mediaType`, `codec`, `streamFormat:"rtp"`, `alignment:"packet"`,
timestamps, and time base.

## WebSocket

```js
const ws = WebSocket.connect({ url: "ws://127.0.0.1:8080/signal" });
```

Options:

- `url`: `ws://` or `wss://` endpoint
- `timeoutMs`: optional connection timeout
- `disableTlsVerification`: optional `wss://` certificate verification override

Methods:

- `send(data)`, where `data` is a string, ArrayBuffer, or typed array view
- `poll({ timeoutMs = 0, max = 128 } = {}) -> event[]`
- `state() -> { readyState, open, remoteAddress?, path? }`
- `close()`

`poll()` returns:

- `{ type: "open" }`
- `{ type: "message", data, binary }`
- `{ type: "closed" }`
- `{ type: "error", detail }`

## SignalingServer

```js
const server = SignalingServer.listen({ host: "127.0.0.1", port: 0, path: "/signal" });
const port = server.port();
```

Options:

- `host`: bind address, default `127.0.0.1`
- `port`: bind port, `0` asks the OS for an available port
- `path`: expected signaling path for callers to use
- `timeoutMs`: optional connection timeout

Methods:

- `poll({ timeoutMs = 0, max = 128 } = {}) -> event[]`
- `send(clientId, data)`, where `data` is a string, ArrayBuffer, or typed array view
- `port() -> number`
- `close()`

Disconnected clients are removed from the server's client table. Calling
`send()` with a stale `clientId` throws `webrtc_invalid_argument`.

`poll()` returns:

- `{ type: "client-connected", clientId, detail }`
- `{ type: "message", clientId, data, binary }`
- `{ type: "client-disconnected", clientId }`
- `{ type: "error", clientId?, detail }`

These objects are intentionally transport-level. Applications can relay
`local-description` and `local-candidate` peer events through the server as JSON
and apply them with `setRemoteDescription()` / `addIceCandidate()` on the other
side.

## MediaSession

A higher-level wrapper around a single `PeerConnection` for the common
sender-offers case: add tracks, then `start()` emits one SDP offer through a
`signal` callback; feed the remote's answer and candidates back through
`handleSignal()`. Media is pumped on demand (there is no server-side timer), so
drive `pump()` from the remote's periodic `pump` ticks.

```js
const session = new MediaSession({
  loopbackOnly: true,                 // or iceServers: { stunServer, turnServers, iceTransportPolicy }
  receivePacketBufferNamePrefix: "/my_recv",
  signal: (msg) => conn.send(JSON.stringify(msg)),   // outbound to the remote
});
session.addTrack({ media: "video", codec: "VP8", payloadType: 96, sendPacketBufferName });
session.onClose(() => pipeline.close());
session.start();                      // emits { type: "offer", description }
// on inbound messages from the remote:
session.handleSignal(msg);            // "answer" | "candidate" | "pump" | "stop"
```

Methods:

- `addTrack({ media, codec, payloadType, clockRate?, sendPacketBufferName, mid? }) -> Track`
- `start() -> this` — emit exactly one offer covering all added tracks
- `handleSignal(message)` — apply `answer` / `candidate`, or `pump` / `stop`
- `pump()` — drain peer events, forward local description/candidates, send RTP
- `onState(cb)` / `onStatus(cb)` / `onPump(cb)` / `onClose(cb) -> this`
- `notify(level, message)` — send a `{ type: "status" }` to the remote
- `stats() -> { sentPackets, sentBytes, trackOpen, selectedCandidatePair }`
- `close()`
- `data` — a free-form object for the application to stash state (e.g. a pipeline)

`signal()` receives `{ type: "offer" | "candidate" | "state" | "status", ... }`
objects to forward to the remote. The connection is created with
`disableAutoNegotiation` so exactly one offer is produced.

## SignalingHub

Multiplexes many `MediaSession`s over one signaling transport (for example a
`wl2:http` WebSocket). Wire its `onMessage`/`onClose` into the server's socket
handlers. Clients send `{ type: "hello", token }` first; on success the hub
replies `{ type: "welcome", iceServers }`. On `{ type: "start", request }` it
builds a `MediaSession` and calls `onSession` so the app can attach tracks (and
a pipeline) before the offer is sent.

```js
const hub = new SignalingHub({
  loopbackOnly: true,
  receivePacketBufferNamePrefix: "/my_recv",
  clientIceServers: [],                          // RTCIceServer[] sent to the browser
  authenticate: (hello) => verifyTicket(hello.token) ? "userId" : false,
  onSession: (session, ctx) => {                 // ctx = { conn, userId, request }
    const pipeline = buildPipeline(ctx.request);
    session.data.pipeline = pipeline;
    session.addTrack({ media: "video", codec: "VP8", payloadType: 96, sendPacketBufferName });
    session.onClose(() => pipeline.close());
  },
});

server.ws("/signal", {
  onMessage: (conn, msg) => hub.onMessage(conn, msg.text()),
  onClose: (conn) => hub.onClose(conn),
});
```

Options: `authenticate(hello, record) -> userId | false` (omit to allow all),
`onSession(session, ctx)`, `onError(error, record)`, `iceServers` (native
`{ stunServer, turnServers, iceTransportPolicy }`), `clientIceServers`
(browser `RTCIceServer[]`), `loopbackOnly`, `receivePacketBufferNamePrefix`.

Methods: `onMessage(conn, text)`, `onClose(conn)`, `close()`. `conn` must expose
`.id` and `.send(string)` (the `wl2:http` WebSocket connection does). A failed
`authenticate` sends `{ type: "error" }` and closes the connection.

The browser counterpart is the reusable `wl2-webrtc-client.js` library shipped
with `examples/js/scripts/lib/` and demonstrated by
`examples/js/scripts/http_webrtc_video_streamer.js`.

## Errors

Failures throw `WebRtcError` objects with `module`, `code`, `operation`, and
`message` fields. Stable codes in this slice are:

- `webrtc_invalid_argument`
- `webrtc_permission_denied`
- `webrtc_closed`
- `webrtc_failed`
