# wl2:webrtc Security

WebRTC can open network paths beyond a single configured endpoint because ICE
uses local, server-reflexive, relay, and peer-discovered candidates. Embedders
should enable this module only for scripts that are allowed to create WebRTC
sessions.

## Network Policy

Configured STUN and TURN URLs are checked with
`Runtime::authorizeNetworkConnect(host, port)` before the peer is constructed.
If the endpoint is denied, `PeerConnection.create()` throws
`webrtc_permission_denied` and libdatachannel is not given the server URL.

`WebSocket.connect()` also checks
`Runtime::authorizeNetworkConnect(host, port)` before opening the signaling
socket. `SignalingServer.listen()` checks
`Runtime::authorizeNetworkListen(host, port)` before binding the server socket.
Denied signaling endpoints throw `webrtc_permission_denied` before libdatachannel
is asked to connect or listen.

When the `wl2` CLI is running interactively, denied connect/listen checks prompt
for the exact endpoint, for example `network listener on 127.0.0.1:0` and
`network connection to 127.0.0.1:<port>`. Approved endpoints are cached only for
the current run. Non-interactive hosts should pass explicit
`--allow-network`/`--network-allow` and `--allow-listen`/`--listen-allow`
grants or set the equivalent `RuntimeOptions` fields.

`loopbackOnly:true` does not configure STUN/TURN and binds host candidates to
`127.0.0.1`. This is the default mode used by tests.

ICE can still contact peer-advertised candidate addresses during normal WebRTC
operation. That reach cannot be fully enumerated before negotiation; it is the
reason this module should be treated as a WebRTC-specific capability, not just
as a generic outbound TCP grant.

## Threading

libdatachannel invokes callbacks on internal threads. The module never runs
JavaScript from those callbacks. It copies each callback payload into a bounded
native queue, and JavaScript receives it only by calling `poll()`.

## Data Handling

Data-channel and peer-event queues are bounded. When a queue is full, the oldest
item is dropped and a counter is incremented; `PeerConnection.stats()` exposes
peer-event drops. WebSocket signaling queues use the same bounded drop-oldest
policy. Applications should poll regularly for high-rate traffic.

## Shared Memory

Media tracks use `PacketBuffer` names. `addTrack()` checks
`Runtime::authorizeSharedMemory(sendPacketBufferName)` before opening the send
buffer. Inbound tracks generate a receive buffer name from
`receivePacketBufferNamePrefix` and check the same policy before creating it.
Denied access is surfaced as `webrtc_permission_denied` for `addTrack()` or as a
peer `error` event for inbound tracks.
