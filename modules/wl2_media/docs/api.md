# wl2:media

`wl2:media` is the backend-agnostic media schema module. It owns the shared
descriptors, packet metadata, timestamp conversion, backpressure profile names,
and media error codes used by `wl2:gstreamer`, `wl2:ffmpeg`, and future media
backends. It is pure schema and validation code: it has no pipelines, codecs,
devices, or shared-memory transport of its own, and no external dependencies.

The canonical wire form is a JSON-compatible string. Native backends serialize
and parse the same schema through the C++ helpers in
`include/wl2_media/schema.h`, so metadata stored in a libmembus `PacketBuffer`
by one backend can be read by another.

## JavaScript API

```js
import {
  schemaVersion,
  StreamDescriptor,
  validateStreamDescriptor,
  PacketMetadata,
  validatePacketMetadata,
  normalizePacketMetadata,
  VideoFormat,
  AudioFormat,
  backpressureProfiles,
  errorCodes,
  Timestamp,
} from "wl2:media";
```

| Export | Description |
|--------|-------------|
| `schemaVersion()` | `{ packet, stream }` schema major versions. |
| `StreamDescriptor(descriptor)` | Validate and normalize a stream descriptor; alias of `validateStreamDescriptor`. |
| `validateStreamDescriptor(descriptor)` | Same as above; throws `MediaError` on invalid input. |
| `PacketMetadata(metadata)` | Validate and normalize packet metadata; alias of `validatePacketMetadata`. |
| `validatePacketMetadata(metadata)` | Validate packet metadata; throws `MediaError` on invalid input. |
| `normalizePacketMetadata(metadata, options?)` | Fill defaults and validate; returns a normalized object. |
| `VideoFormat(format)` | Normalize `{ format, width, height, framerate }`. |
| `AudioFormat(format)` | Normalize `{ format, rate, channels, layout }`. |
| `backpressureProfiles()` | `["record", "transcode", "preview", "relay"]`. |
| `errorCodes()` | Stable error code constants keyed by name. |
| `Timestamp.convert(value, fromTimeBase, toTimeBase)` | Convert a timestamp between `"num/den"` time bases. |

### Descriptor shapes

`StreamDescriptor`:

```js
{ schema: 1, mediaType: "video", codec: "h264", caps: "...", streamFormat: "byte-stream", alignment: "au", track: 0 }
```

`PacketMetadata`:

```js
{
  schema: 1, mediaType: "video", codec: "h264", caps: "...", streamFormat: "byte-stream",
  alignment: "au", track: 0, pts: 0, dts: 0, duration: 0, timeBase: "1/1000000000",
  flags: 0, discontinuity: false, sideData: ""
}
```

`mediaType` is one of `video`, `audio`, `subtitle`, `data`. Timestamps are
nanoseconds at the JavaScript boundary; `timeBase` records the unit so scripts do
not need to know which backend produced a packet. Backend-specific fields belong
inside `sideData` under a namespaced object (`gstreamer`, `ffmpeg`), never at the
top level.

## Errors

Errors use the shared Winglib2 module error shape with `name: "MediaError"`,
`module: "wl2_media"`, and a stable `code`:

| Code | Meaning |
|------|---------|
| `media_invalid_argument` | Missing or wrong-typed argument. |
| `media_invalid_schema` | Unsupported schema major version. |
| `media_unsupported_media_type` | `mediaType` is not a recognized value. |
| `media_invalid_time_base` | Time base is not a valid `"num/den"` string. |
| `media_parse_failed` | Wire string is not valid schema JSON. |

## C++ helpers

`include/wl2_media/schema.h` exposes `wl2::media::StreamDescriptor`,
`wl2::media::PacketMetadata`, `serialize()`, `parseStreamDescriptor()`,
`parsePacketMetadata()`, `validate()`, `parseTimeBase()`, and
`convertTimestamp()`. Link the `wl2_media_static` target to reuse them.
