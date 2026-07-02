# wl2:webrtc

`wl2:webrtc` is a libdatachannel-backed WebRTC module for Winglib2. It provides
peer connections, reliable data channels, RTP media tracks bridged through
`wl2:membus` `PacketBuffer`, and built-in WebSocket client/server signaling.

The module is transport-focused. It does not encode or decode audio/video; codec
pipelines such as `wl2:gstreamer` or `wl2:ffmpeg` produce and consume RTP
packets through `PacketBuffer`.

## JavaScript Surface

```js
import { PeerConnection, WebSocket, SignalingServer } from "wl2:webrtc";
```

- `PeerConnection.create()` creates a libdatachannel peer, emits SDP/ICE through
  `poll()`, sends/receives data channels, and can relay RTP tracks.
- `WebSocket.connect()` connects to a signaling endpoint.
- `SignalingServer.listen()` accepts WebSocket signaling clients and exposes
  poll-based connection/message/disconnect events with per-client ids.

See [docs/api.md](docs/api.md), [docs/security.md](docs/security.md), and
[docs/design.md](docs/design.md) for the full surface and policy model.

## Examples

```sh
wl2 run modules/wl2_webrtc/examples/js/webrtc_datachannel_loopback.js
```

The signaling example opens a loopback WebSocket listener and clients. In an
interactive terminal it prompts for the requested host permissions; for
non-interactive runs, grant them explicitly:

```sh
wl2 run \
  --allow-network --network-allow '127.0.0.1:*' \
  --allow-listen --listen-allow '127.0.0.1:*' \
  modules/wl2_webrtc/examples/js/webrtc_signaling_server.js
```

The media relay example uses shared memory and needs a matching shared-memory
grant in non-interactive runs:

```sh
wl2 run \
  --allow-shared-memory --shared-memory-allow /wl2_webrtc \
  modules/wl2_webrtc/examples/js/webrtc_media_relay.js
```

## Build

The default provider fetches a pinned libdatachannel release with WebSocket and
media support enabled. Configure with `-DWL2_WEBRTC_PROVIDER=off` to exclude the
module or `-DWL2_WEBRTC_PROVIDER=fetch` to force the source build.
