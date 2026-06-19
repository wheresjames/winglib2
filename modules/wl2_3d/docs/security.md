# wl2_3d Security

`wl2:3d` treats shared-memory names, queue payloads, and readable frame headers
as **untrusted**.

## Capability gates

- **Shared memory.** Creating or attaching frame rings (`publishTo`, `resize`)
  and detection queues (`detectionSource`) requires
  `RuntimeOptions::allowSharedMemory` and a matching `sharedMemoryAllow` prefix.
  Names are namespaced per run by that required prefix; a name outside the
  allow-list is denied (`shared_memory_denied`).
- **Graphics.** GPU/context creation (when the Magnum renderer is present)
  additionally requires `RuntimeOptions::allowGraphics`.
- Create vs. attach: producers create rings/queues (`publishTo`/`resize` create a
  `memvid` writer); consumers attach read-only (`detectionSource` attaches a
  `memmsg` reader with `writable = false`).

## Untrusted-buffer validation

Every boundary that reads externally-written memory validates before use:

- **Frame rings.** Width/height/stride/size and pixel format are checked before
  any copy or GPU upload; an invalid or undersized frame is rejected
  (`3d_invalid_frame`), never read out of bounds. The RGBA8/sRGB/premultiplied/
  top-left pixel contract is asserted at each boundary, including after `resize`.
- **Detection records.** The `memmsg` decoder (`decodeDetection`) is fully
  bounds-checked: magic, schema version, every fixed field, and the
  length-prefixed class string are validated against the remaining buffer, and
  image dimensions and coordinate finiteness are sanity-checked. Any truncated,
  mis-magicked, wrong-version, or garbage payload returns `null` (counted
  `invalid`) — it never crashes. `pollDetections` also caps records processed per
  call to bound work from a hostile writer.
- **Process isolation.** On native builds the renderer may run in a separate
  process behind the same read-only `FrameRing` consumer contract. A renderer
  crash leaves the UI process alive with its last mapped frame; reconnect or ring
  recreation is handled as normal producer replacement rather than by sharing UI
  process state with the renderer.

## Dynamic resize

`resize()` recreates the ring under the same authorized name and bumps a
**generation** counter. The old handle is closed before the new ring is created
so the new shared memory is not unlinked. Consumers re-open by name (optionally
gated on the generation carried over the control channel) to pick up new
dimensions.

## Threading

Engine state (scene graph, camera, animators, particles, detections) is owned by
the JS thread and advanced only by `scene.tick()`; completion callbacks fire
there. The render thread (Magnum, when present) only reads frames under the ring
mutex and never calls into JavaScript.
