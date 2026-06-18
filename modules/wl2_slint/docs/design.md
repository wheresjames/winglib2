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

## Dependency strategy

Slint is a Rust library, so the module **prefers Slint's prebuilt C++ binary
packages** (consumed via `find_package(Slint)`), which need no Rust toolchain.
`cmake/WL2Slint.cmake` exposes `wl2_find_slint()` with `auto/local/package/fetch/
source/off` providers; only `source` builds from source and needs `cargo`. The
single `${WL2_SLINT_TARGET}` (`Slint::Slint`) keeps the module CMake free of
platform branches, and the packaged config fragment re-discovers Slint for
out-of-tree consumers.
