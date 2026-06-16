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
  CommandChannel,
  KeyValueStore,
  Selector,
  hasV12Surface
} from "wl2:membus";
```

`hasV12Surface` reports whether the selected libmembus dependency exposes the
full v1.2 surface. With the default provider policy this should be true because
Winglib2 stages libmembus v1.2.0 when the sibling checkout is older.

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
