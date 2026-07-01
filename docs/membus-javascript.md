# JavaScript Membus Bindings {#membus_javascript}

The C++ libmembus wrappers are exposed to JavaScript as the importable native
module `wl2:membus`.

## Import

```js
import {
  SharedBuffer,
  SharedQueue,
  VideoBuffer,
  AudioBuffer,
  PacketBuffer,
  CommandChannel,
  KeyValueStore,
  Selector,
  hasV12Surface,
  hasV21Surface
} from "wl2:membus";
```

`hasV12Surface` reports whether the selected libmembus dependency exposes the
full v1.2 surface. `hasV21Surface` reports whether the selected dependency
exposes `mempkt` / `PacketBuffer`. With the default provider policy both should
be true because Winglib2 stages libmembus v2.1.0 when the local checkout is
older.

## Handle Lifecycle

Native handles are explicit JavaScript objects. They close themselves in native
finalizers as a last resort, but examples and tests should call `close()` in a
`finally` block.

```js
const q = SharedQueue.create("/wl2_queue", 4096, true);
try {
  q.write("hello");
} finally {
  q.close();
}
```

## Payloads

Byte reads return `wl2.Buffer` objects. Byte writes accept strings, `ArrayBuffer`
values, any typed-array or `DataView` view (such as `Uint8Array`, honoring the
view's byte offset and length), and objects exposing a binary-safe
`arrayBuffer()` or `text()` method such as `wl2.Buffer`.

## PacketBuffer

`PacketBuffer` wraps libmembus `mempkt` for variable-length compressed or
packetized records. Use it when the payload is a compressed access unit, muxed
packet, metadata event, or other byte record that does not fit fixed-size video
or audio frame rings.

```js
const packets = PacketBuffer.create("/wl2_packets", 64, 1024 * 1024, 256 * 1024, {
  metadata: wl2.buffer.fromText("stream-config")
});
const nextPtr = packets.write(packetBytes, {
  kind: "video",
  track: 0,
  pts: 12345,
  metadata: headerBytes
});
const slot = (nextPtr + packets.metadata().buffers - 1) % packets.metadata().buffers;
const record = packets.record(slot);
```

## Selector

The JavaScript selector API uses non-consuming predicates:

```js
const ready = Selector.wait([
  () => queue.poll(),
  () => video.metadata().sequence > lastSeq
], { timeoutMs: 100 });
```

The returned value is the first ready predicate index, or `-1` on timeout.

## Tests

`test/scripts/wl2_membus_smoke.js` covers:

- shared-memory buffer create/attach/read/write
- message queue write/read/poll
- video buffer metadata and frame bytes
- audio buffer metadata and payload bytes
- packet buffer metadata and variable-length records
- command channel write/read
- key-value store schema, named values, indexed values, and snapshots
- selector timeout and ready-index behavior

Run it directly:

```sh
./build/app/wl2/wl2 test/scripts/wl2_membus_smoke.js
```

Or through CTest:

```sh
ctest --test-dir build --output-on-failure -R scripts.wl2_membus_smoke
```
