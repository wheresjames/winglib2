# wl2:gstreamer Security Model

## Launch strings are trusted input

`parseLaunch(description)` executes GStreamer launch syntax. A launch string can
name any installed element, open files and devices, and construct network
sources and sinks. **Treat launch strings like code.** They must come from
trusted sources only, never from untrusted script input or remote data.

A future release will gate `parseLaunch()` behind a dedicated runtime capability
(for example `gstreamer.launch.trusted`) and prefer structured convenience
helpers for untrusted workflows. Until that capability exists, embedders that run
untrusted scripts should not expose this module, or should wrap it.

## Host access is separately authorized

A successful pipeline parse does not grant access to any resource. File, device,
and network access performed by pipeline elements remains subject to the host
runtime policy:

- **Shared memory.** Named membus objects must pass the runtime shared-memory
  allow-list before bridges create or attach rings.
- **Filesystem.** File playback and media discovery helpers call
  `Runtime::resolveFilesystemReadPath()` before constructing file pipelines.
- **Devices.** Device helpers expose installed GStreamer devices. Hosts should
  only grant this module to scripts that may inspect or open local devices.
- **Network.** Network helpers call `Runtime::authorizeNetworkConnect()` or
  `Runtime::authorizeNetworkListen()` before constructing socket-opening
  pipelines.

## Plugin loading

The module does not set GStreamer plugin search paths from JavaScript. Plugin
discovery uses the ambient GStreamer registry configured for the process before
module initialization. `capabilities()` reports compile-time feature support and
whether GStreamer initialized; `listPlugins()` reports runtime plugin
availability.

## Lifecycle and cleanup

Every `Pipeline` exposes an explicit `close()` that transitions the pipeline to
`NULL` before releasing GStreamer objects. A QuickJS finalizer performs the same
teardown as a backstop, so a pipeline that goes out of scope without an explicit
`close()` is still driven to `NULL` and unreferenced. `close()` is idempotent,
and any method called after close fails with the stable code `gstreamer_closed`
rather than crashing.
