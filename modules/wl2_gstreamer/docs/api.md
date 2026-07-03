# wl2:gstreamer

`wl2:gstreamer` wraps the GStreamer library to build, run, and inspect pipelines
from JavaScript. It provides the pipeline runtime plus copy-based media membus
bridges for `VideoBuffer`, `AudioBuffer`, and `PacketBuffer`.

The module is discovered through the standard provider model (system,
target-local, or cross-sysroot via pkg-config) and is disabled when GStreamer is
not available.

## JavaScript API

```js
import {
  version,
  capabilities,
  listPlugins,
  listElements,
  parseLaunch,
  testPattern,
  filePlayback,
  recordVideoBuffer,
  recordPacketBuffer,
  discoverMedia,
  captureDevice,
  sendUdpPackets,
  receiveUdpPackets,
  sendRtpPackets,
  receiveRtpPackets,
  sendTcpPackets,
  receiveTcpPackets,
  streamVideoUdp,
  streamVideoTcp,
  rtspPlayback,
  teeVideoBuffer,
  overlayVideoBuffer,
  DeviceMonitor,
  Caps
} from "wl2:gstreamer";
```

| Export | Description |
|--------|-------------|
| `version()` | `{ module, gstreamer: { string, major, minor, micro, nano } }`. |
| `capabilities()` | `{ gstreamer, features, bridges, launchTrusted, initialized }`. `features` reports compile-time `app`/`video`/`audio`/`deviceMonitor` support. |
| `listPlugins(options?)` | `[ { name, version, description, license, source } ]`. `options.filter` is an optional substring match on the plugin name. |
| `listElements(options?)` | `[ { name, longName, klass, description } ]`. `options.filter` matches the element name. |
| `parseLaunch(description, options?)` | Build a `Pipeline` from GStreamer launch syntax. **Trusted input** — see `docs/security.md`. |
| `testPattern(options)` | Create a `videotestsrc` pipeline attached to a `VideoBuffer`. |
| `filePlayback(options)` | Decode a local file into a `VideoBuffer`, and optionally an `AudioBuffer`. |
| `recordVideoBuffer(options)` | Encode frames from a `VideoBuffer` into `outputPath` using the requested encoder and muxer. |
| `recordPacketBuffer(options)` | Copy `PacketBuffer` payloads to `outputPath`, optionally through a muxer. |
| `discoverMedia(options)` | Return duration, seekability, caps, tags, and stream metadata for a local media file. Requires `gstreamer-pbutils-1.0`. |
| `captureDevice(options)` | Capture a selected Linux v4l2 device into a `VideoBuffer`; without `device`, creates a synthetic live test source. |
| `sendUdpPackets(options)` | Stream `PacketBuffer` records to a UDP endpoint after runtime connect authorization. |
| `receiveUdpPackets(options)` | Receive UDP packets into a `PacketBuffer` after runtime listen authorization. |
| `sendRtpPackets(options)` | Stream RTP payload records from a `PacketBuffer` to UDP. |
| `receiveRtpPackets(options)` | Receive RTP packets, optionally through a depayloader, into a `PacketBuffer`. |
| `sendTcpPackets(options)` | Stream `PacketBuffer` records to a TCP client sink after runtime connect authorization. |
| `receiveTcpPackets(options)` | Listen with `tcpserversrc` and write records into a `PacketBuffer`. |
| `streamVideoUdp(options)` | Encode/payloader frames from a `VideoBuffer` and send them over UDP. |
| `streamVideoTcp(options)` | Encode/mux frames from a `VideoBuffer` and send them over TCP. |
| `rtspPlayback(options)` | Decode an RTSP stream into a `VideoBuffer` after runtime connect authorization. |
| `teeVideoBuffer(options)` | Multi-sink helper: `tee` a source into a `VideoBuffer` and, with `outputPath`, an encoded file simultaneously. |
| `overlayVideoBuffer(options)` | Capture a source through a named `textoverlay` into a `VideoBuffer`; update text live via `setOverlayText()`. |
| `DeviceMonitor.create(options?)` | Enumerate GStreamer devices. `options.classes` filters device classes. |
| `Caps.parse(text)` | Parse a caps string into `{ text, any, empty, structureCount, structures: [{ name, fields }] }`. |

### Pipeline

```js
const pipeline = parseLaunch("videotestsrc num-buffers=30 ! autovideoconvert ! fakesink");
```

| Method | Description |
|--------|-------------|
| `state()` | `{ state, pending, result }` with GStreamer state names. |
| `setState("null"\|"ready"\|"paused"\|"playing")` | Change state; returns the same result shape. Throws `gstreamer_state_change_failed` on failure. |
| `play()` / `pause()` / `stop()` | Shorthand for PLAYING / PAUSED / NULL. |
| `queryPosition()` | `{ ok, position }` — position in nanoseconds, or `-1` when unavailable. |
| `queryDuration()` | `{ ok, duration }` — duration in nanoseconds. |
| `seek({ position, flush? })` | Seek to `position` nanoseconds; `flush` defaults to `true`. |
| `busPoll({ timeoutMs?, max? })` | Drain bus messages. Waits up to `timeoutMs` (default 0) for the first message, then returns all currently queued up to `max` (default 64). |
| `attachVideoSink(options)` | Attach an `appsink` to a `VideoBuffer`. |
| `attachAudioSink(options)` | Attach an `appsink` to an `AudioBuffer`. |
| `attachPacketSink(options)` | Attach an `appsink` to a newly created `PacketBuffer`. |
| `attachVideoSource(options)` | Attach a `VideoBuffer` to an `appsrc`. |
| `attachAudioSource(options)` | Attach an `AudioBuffer` to an `appsrc`. |
| `attachPacketSource(options)` | Attach a `PacketBuffer` to an `appsrc`. |
| `pushVideoFrame(options)` | Push one video ring slot into the attached video `appsrc`. |
| `pushAudioSamples(options)` | Push one audio ring slot into the attached audio `appsrc`. |
| `pushPacket(options)` | Push one packet record, or an explicit byte payload, into the attached packet `appsrc`. |
| `endOfStream(options?)` | Signal EOS on attached source bridges. `elementName` limits the signal to one source. |
| `stats()` | Return per-bridge counters, dropped counts, sequence values, and negotiated caps where available. |
| `snapshot(options?)` | Still-frame export from the latest video-sink sample. See below. |
| `queryLatency()` | `{ ok, supported, live, minLatency, maxLatency }` (nanoseconds; `-1` when unset) or `{ ok:false, supported:false }`. |
| `negotiatedCaps({ element, pad? })` | Current caps on a named element's pad (`pad` defaults to `src`), falling back to allowed caps with `negotiated:false`. |
| `setOverlayText({ elementName?, text })` | Update a live `textoverlay` element's text (`elementName` defaults to `wl2_overlay`). |
| `close()` | Transition to NULL and release native objects. Idempotent. |

### Advanced features

`snapshot({ elementName?, path?, format?, encoder? })` reads the most recent
sample retained by a video sink bridge and returns
`{ ok, width, height, format, caps, size, pts }`. When `path` is given the frame
is encoded to a file (`format` `"png"` (default) or `"jpeg"`, or an explicit
`encoder` such as `"pngenc"`), and `path` is echoed back. Without `elementName`
the first attached video sink is used. Throws `gstreamer_invalid_argument` when
no frame has been produced yet.

`queryLatency()` runs a GStreamer latency query against the pipeline. `supported`
is `false` when the pipeline does not answer the query (a clear unsupported
result rather than an error).

`negotiatedCaps()` and `Caps.parse()` are caps-negotiation utilities: the first
inspects a live pad's current or allowed caps for debugging negotiation
failures; the second parses a caps string into structured fields.

`teeVideoBuffer({ source?, videoBufferName, outputPath?, width?, height?, fps?, buffers?, encoder?, muxer?, format? })`
builds `<source> ! tee` with one branch publishing RGBA frames into a
`VideoBuffer` and, when `outputPath` is set, a second branch encoding to a file
(`vp8enc`/`webmmux` by default). `source` defaults to a moving test pattern.

`overlayVideoBuffer({ source?, videoBufferName, text?, width?, height?, fps?, buffers?, format? })`
inserts a `textoverlay` named `wl2_overlay` before the video sink. Drive the
overlay text at runtime with `setOverlayText()`, typically from low-rate records
read out of a `wl2:membus` `SharedQueue`.

QoS bus messages (`type: "qos"`) carry `live`, `runningTime`, `streamTime`,
`timestamp`, `duration`, `jitter`, `proportion`, `quality`, `processed`, and
`dropped` for latency/QoS diagnosis.

Bus message objects always carry `type` and `source`. `error`/`warning`/`info`
add `message`, `domain`, `gcode`, and `debug`. `state-changed` adds `oldState`,
`newState`, and `pending`.

### Helpers

`filePlayback({ path, videoBufferName, audioBufferName?, width?, height?, fps?, buffers?, format? })`
builds a `filesrc ! decodebin` pipeline and attaches `appsink` bridges. Video
defaults to `RGBA` at `320x240`, 30 fps, four ring slots.

`recordVideoBuffer({ videoBufferName, outputPath, encoder?, muxer? })` builds an
`appsrc` pipeline and defaults to `vp8enc deadline=1 ! webmmux`. Use
`listElements()` before selecting presets that may not be installed.

`recordPacketBuffer({ packetBufferName, outputPath, caps?, muxer? })` pushes
packet records from a `PacketBuffer` to a file sink. `caps` defaults to
`application/octet-stream`.

`discoverMedia({ path })` returns:

```js
{
  duration,
  seekable,
  live,
  caps,
  tags,
  streams: [
    { type, streamId, streamNumber, caps, tags, width, height, sampleRate, channels }
  ]
}
```

`captureDevice({ videoBufferName, device?, width?, height?, fps?, buffers? })`
uses `v4l2src device=...` when `device` is supplied. Without `device`, it uses a
live `videotestsrc` so tests and headless environments can exercise the helper
without hardware.

Network helpers authorize endpoints before constructing the pipeline, so denied
network access fails before any GStreamer network element can open a socket.

```js
sendUdpPackets({ packetBufferName, host, port, caps? });
receiveUdpPackets({ packetBufferName, host?, port, caps?, buffers?, arenaSize?, maxRecord? });
sendRtpPackets({ packetBufferName, host, port, caps? });
receiveRtpPackets({ packetBufferName, host?, port, caps?, depay? });
sendTcpPackets({ packetBufferName, host, port, caps? });
receiveTcpPackets({ packetBufferName, host?, port, caps? });
streamVideoUdp({ videoBufferName, host, port, encoder?, payloader? });
streamVideoTcp({ videoBufferName, host, port, encoder?, muxer? });
rtspPlayback({ uri, videoBufferName, width?, height?, fps?, buffers?, latency? });
```

Receive helpers default `host` to `127.0.0.1`. Packet helpers default `caps` to
`application/octet-stream`, except RTP helpers which default to
`application/x-rtp,media=video,encoding-name=H264,payload=96`. Video UDP
defaults to `vp8enc deadline=1 ! rtpvp8pay pt=96`; video TCP defaults to
`vp8enc deadline=1 ! webmmux streamable=true`.

## MembuS Bridges

Bridge methods require `gstreamer-app-1.0`. `capabilities().bridges` reports
which bridge families are available in the current build; packet bridges also
require `wl2::libmembusHasV21Surface()`.

Video sink:

```js
const pipeline = parseLaunch("videotestsrc ! videoconvert ! video/x-raw,format=RGBA,width=640,height=480,framerate=30/1 ! appsink name=wl2_video_sink");
pipeline.attachVideoSink({ videoBufferName: "/frames", create: true, width: 640, height: 480, fps: 30, buffers: 4 });
```

Supported video caps formats are `RGBA`, `BGRA`, `RGB`, `BGR`, `GRAY8`, `YUY2`,
and `UYVY`. Negotiated caps must match the target ring dimensions and format.

Audio sink:

```js
pipeline.attachAudioSink({ audioBufferName: "/pcm", create: true, sampleRate: 48000, channels: 2, format: "S16LE", fps: 50, buffers: 16 });
```

`S16LE` is the default. `U8`, `S24LE`, `S32LE`, and `F32LE` are accepted when
the configured libmembus surface supports those formats.

Source bridges:

```js
pipeline.attachVideoSource({ videoBufferName: "/frames" });
pipeline.pushVideoFrame({ slot: 0, pts: 0 });
pipeline.pushVideoFrame({ latest: true });
pipeline.attachAudioSource({ audioBufferName: "/pcm" });
pipeline.pushAudioSamples({ slot: 0 });
pipeline.pushAudioSamples({ latest: true });
```

`pushVideoFrame()` and `pushAudioSamples()` generate fixed-rate timestamps by
default. `pts` and `duration` override the generated values. Use explicit `slot`
for manually addressed buffers, or `latest: true` to push the latest committed
slot from a live producer ring.

Packet bridges:

```js
const sink = parseLaunch("... ! appsink name=wl2_packet_sink");
sink.attachPacketSink({
  packetBufferName: "/packets",
  create: true,
  buffers: 32,
  arenaSize: 1048576,
  maxRecord: 262144,
  caps: "video/x-h264,stream-format=byte-stream,alignment=au"
});

const source = parseLaunch("appsrc name=wl2_packet_src ! fakesink");
source.attachPacketSource({ packetBufferName: "/packets", caps: "application/octet-stream" });
source.pushPacket({ waitTimeoutMs: 10 });
source.pushPacket({ data: new Uint8Array([1, 2, 3]), pts: 0, duration: 1000000 });
```

Packet sinks currently require `create: true` because libmembus exposes
read-only `PacketBuffer::openExisting()` but no writable attach surface for an
already-created packet ring. Packet sources open existing rings read-only.
Per-record metadata is stored as JSON containing caps, nanosecond timestamps,
duration, flags, and discontinuity state.

## Errors

Errors use the shared module error shape with `name: "GstreamerError"`,
`module: "wl2_gstreamer"`, and a stable `code`:

| Code | Meaning |
|------|---------|
| `gstreamer_invalid_argument` | Missing or wrong-typed argument. |
| `gstreamer_init_failed` | GStreamer failed to initialize. |
| `gstreamer_parse_failed` | `parseLaunch()` could not parse the description. |
| `gstreamer_not_a_pipeline` | The launch description did not produce a pipeline. |
| `gstreamer_state_change_failed` | A state transition failed. |
| `gstreamer_seek_failed` | The pipeline did not handle a seek. |
| `gstreamer_closed` | A method was called on a closed pipeline. |
| `gstreamer_element_not_found` | A named app element was not found in the pipeline. |
| `gstreamer_membus_failed` | A membus ring could not be opened, read, or written. |
| `gstreamer_permission_denied` | Runtime shared-memory, filesystem, or network policy denied the requested resource. |
| `gstreamer_push_failed` | `appsrc` rejected a pushed buffer. |
| `gstreamer_unsupported` | The build or dependency surface lacks a requested bridge. |

Error, warning, and info bus messages also surface the original GStreamer
`domain`, `gcode`, and `debug` fields for diagnosis.
