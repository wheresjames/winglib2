# wl2:slint Security

## UI capability

Opening a window is a host-resource action, like network or filesystem access.
It is governed by a single capability:

- `RuntimeOptions::allowUi` (denied by default) gates `Runtime::authorizeUi()`.
- `wl2:slint` calls `authorizeUi()` before `show()`/`hide()`, `run()`, and
  native dialog calls. A denial surfaces as a `SlintError` with code
  `slint_permission_denied`.
- **Compiling and instantiating a component requires no capability**, so headless
  property/callback work and CI tests run ungated.

### Granting the capability

- CLI: `wl2 run --allow-ui script.js`
- Manifest:
  ```yaml
  capabilities:
    ui: true
  ```
- Embedding: set `RuntimeOptions::allowUi = true`.

`wl2 config` reports `capabilities.ui` as denied by default, or allowed when a
manifest requests it.

## Native dialogs

Native file/folder dialogs are optional at build time. Unsupported embedded
targets compile the API surface but dialog calls reject with `slint_unsupported`.
When available, dialogs are gated by the UI capability because they open host UI
and disclose user-selected host paths.

Returned paths are strings. They do not grant broad filesystem access and do not
bypass `compileFile()` or other module read/write policies; code that opens a
returned path must still go through the appropriate runtime filesystem
authorization.

## Resource loading

`compileFile()` resolves `wl2:/` specifiers through the runtime resource store.
Host filesystem paths are authorized with `Runtime::resolveFilesystemReadPath()`
and confined to the configured read roots. Filesystem reads are denied by
default.

## Shared memory (FrameRing bridge)

The 3D-in-UI consumer (`setImageFromFrameRing`) and the UI-on-3D producer
(`renderOffscreenTo`/`injectPointer`/`injectKey`) cross the sandbox via libmembus
shared memory, so both require `RuntimeOptions::allowSharedMemory` and a matching
`sharedMemoryAllow` prefix; a name outside the allow-list is denied
(`slint_permission_denied`). Frames read from a ring are treated as untrusted:
width/height/stride/size and the RGBA32 format are validated before any copy into
a `SharedPixelBuffer`, and only `RGBA32` rings are accepted. The producer writes
RGBA8/sRGB/premultiplied/top-left frames (the shared pixel contract). Off-screen
rendering opens no window and needs no `allowUi`; it is purely a shared-memory
operation.

## Threading

All interpreter and instance calls run on the JS/main thread; the module spawns
no worker threads. This avoids cross-thread access to the QuickJS context and
matches Slint's single-threaded UI requirement. Off-screen rendering and event
injection likewise run synchronously on the JS thread.
