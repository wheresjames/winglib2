# wl2:slint Design

The closest model is `modules/wl2_asio`: a self-contained module with an
in-module dependency provider, a capability gate, QuickJS bindings over opaque
native handles, and deterministic tests that do not depend on a developer's
desktop.

## Why the interpreter

`wl2:slint` uses the **Slint interpreter** (`slint::interpreter`), which compiles
`.slint` markup at runtime and exposes a reflective property/callback API. This
is the right fit for a dynamic JavaScript binding: UIs load from markup or
resources rather than being compiled into the binary. Build-time compiled
(`slint_target`) components remain a later option for typed/embedded UIs.

## Native ownership

Native state lives in module-private classes referenced by opaque pointers from
QuickJS objects:

- `CompiledComponent` — wraps `slint::interpreter::ComponentDefinition`.
- `InstanceState` — a `slint::ComponentHandle<ComponentInstance>` plus a keepalive
  on the `CompiledComponent`.
- `SlintRuntimeState` (`slint_runtime.h`) — process-wide state (the single
  active event loop) and a shutdown hook so the loop never outlives the runtime.

QuickJS finalizers drop the native handles; the JS `Component`/`Instance` objects
own them through `shared_ptr` boxes.

## Callbacks and JS handler lifetime

`Instance.on()` stores each JS handler as a property of a hidden "handlers
holder" object that is itself a (non-enumerable) property of the instance JS
object. The handlers are therefore owned by the JS object graph and reclaimed by
normal GC together with the instance — the module holds **no** long-lived JSValue
references to free in a finalizer. This matters: an external (C++) reference to a
handler would root it and defeat QuickJS's cycle collector, so a handler that
captures the instance (a common pattern) would leak both. The Slint callback
closure registered with `set_callback` captures only a `weak_ptr` to the
callback registry plus the callback name (never a strong reference to the
instance, per Slint's requirement) and re-reads the current handler from the
holder each time it fires.

## Value bridge

`value_bridge.h` converts between `slint::interpreter::Value` and QuickJS values:
number ⇄ f64, string ⇄ `SharedString`, bool, JS object ⇄ one-level `struct`, and
JS array ⇄ model. An `allowComposite` flag keeps struct fields scalar-only while
permitting structs and models at the top level, in array elements, and in
callback arguments. Brushes/colors read and write as CSS hex strings. Images
remain a follow-up. Unsupported conversions are rejected with `slint_type_error`.

## Event loop

First-version model: **Slint owns the loop**. `Component.run()` enters
`slint::run_event_loop()` on the JS/main thread and blocks until the last window
closes or `quit()` is called. For the duration of `run()`, a short recurring
`slint::Timer` (~5 ms) runs the same two steps the engine's own event loop uses —
`Runtime::async().drain()` followed by `JS_ExecutePendingJob()` — on the UI
thread. Because the pump is identical to the engine loop, any async module's
completions (and the JS promise reactions they enqueue) settle while the UI is
up, and Slint callbacks invoke JavaScript directly on the UI thread where
touching the QuickJS context is safe.

`quit()` maps to `slint::quit_event_loop()`, and a runtime shutdown hook also
quits the loop, so it never outlives the runtime. A fully co-driven loop (a
custom `slint::platform` so neither side blocks) remains a documented follow-up.

## Off-screen rendering (UI-on-3D)

For UI-on-3D the module renders a component into a CPU buffer instead of a
window. `slint_offscreen.h` installs a custom `slint::platform::Platform` whose
`WindowAdapter` owns a `SoftwareRenderer` (the documented "bring your own
renderer" path). `renderOffscreenTo()` software-renders the component to an
`Rgb8Pixel` scratch buffer, expands it to the RGBA8/top-left/premultiplied
FrameRing contract, and advances a libmembus `memvid` ring; `injectPointer`/
`injectKey` dispatch synthetic events to the window and re-render.

Because Slint's platform is process-global and one-shot (`set_platform`), the
off-screen platform is mutually exclusive with the windowed backend used by
`run()`/`show()`. `useOffscreenRendering()` makes the choice explicit and must
precede `compile()`/`create()` — otherwise Slint has already initialized the
default backend and `set_platform` aborts. Each `wl2 run` is its own process, so
a script picks one mode. This is **Topology A** (the libmembus copy path);
sampling the UI ring as a GL texture (**Topology B**, Magnum adopting the femtovg
context) is the renderer-side view, gated behind the `wl2:3d` Magnum provider.

## Dependency strategy

Slint is a Rust library, so the module **prefers Slint's prebuilt C++ binary
packages** (consumed via `find_package(Slint)`), which need no Rust toolchain.
`cmake/WL2Slint.cmake` exposes `wl2_find_slint()` with `auto/local/package/fetch/
source/off` providers; only `source` builds from source and needs `cargo`. The
single `${WL2_SLINT_TARGET}` (`Slint::Slint`) keeps the module CMake free of
platform branches, and the packaged config fragment re-discovers Slint for
out-of-tree consumers.

`WL2_USE_FETCHED_DEPS` pins the default to the prebuilt `fetch` package but no
longer clobbers an explicit `-DWL2_SLINT_PROVIDER=source`, so building Slint from
source is a first-class, selectable option. The `source` provider caps Rust
dependency lints to warnings (`RUSTFLAGS=--cap-lints=warn`, baked into Corrosion's
cargo invocation) so a pinned Slint release keeps building on newer Rust
toolchains where lints such as `dangerous_implicit_autorefs` became
deny-by-default.
