# wl2:webrtc API

`wl2:webrtc` exposes the libdatachannel WebRTC transport to JavaScript:
`PeerConnection`, data channels, RTP media tracks, and a built-in WebSocket
client/server for signaling.

```js
import { version, capabilities, PeerConnection, WebSocket, SignalingServer } from "wl2:webrtc";
```

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
  loopbackOnly: false
});
```

`loopbackOnly:true` binds host candidates to `127.0.0.1` and does not configure
STUN/TURN. It is intended for deterministic tests and local deployments.

Methods:

- `createDataChannel(label) -> DataChannel`
- `addTrack(options) -> Track`
- `setRemoteDescription({ type, sdp }) -> { ok: true }`
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

## Errors

Failures throw `WebRtcError` objects with `module`, `code`, `operation`, and
`message` fields. Stable codes in this slice are:

- `webrtc_invalid_argument`
- `webrtc_permission_denied`
- `webrtc_closed`
- `webrtc_failed`
