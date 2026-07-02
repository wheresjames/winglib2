# wl2:webrtc Design

The module is a thin libdatachannel binding with Winglib2-style module metadata,
structured errors, and explicit polling.

## Ownership

`PeerConnection`, `DataChannel`, `Track`, `WebSocket`, and `SignalingServer`
JavaScript objects hold shared native handles. Finalizers call `close()`
best-effort, and explicit `close()` resets callbacks before closing the
libdatachannel object so late native events cannot enter a destroyed JavaScript
wrapper.

## Event Marshaling

Callbacks such as local-description, candidate, state-change, inbound channel,
WebSocket client/server events, and inbound messages run on libdatachannel
threads. Each callback copies only POD payload data or a shared libdatachannel
handle into a mutex-protected queue and notifies a condition variable. `poll()`
drains those queues on the JavaScript thread.

The queue policy is bounded drop-oldest. This keeps a stalled script from
unbounded native memory growth while preserving forward progress.

## Signaling Boundary

SDP and ICE are exposed as plain JavaScript values. Tests and applications can
broker them through any signaling transport, including the built-in WebSocket
client and many-client signaling server. The server is deliberately a routing
primitive: it assigns client ids, emits connection/message/disconnect events,
and lets JavaScript decide how rooms, peer matching, and authorization work.

## Media Boundary

libdatachannel transports RTP but does not encode or decode media. `Track.pump()`
copies RTP records from a send `PacketBuffer` into a libdatachannel track.
Inbound RTP is copied by the native track callback into a receive
`PacketBuffer`. Each record carries the shared media packet metadata JSON shape
so codec modules can consume the packets without depending on `wl2:webrtc`.

The module does not build jitter buffering, packet loss concealment, or codec
parsing. Those stay in codec pipelines such as `wl2:gstreamer` or `wl2:ffmpeg`.
