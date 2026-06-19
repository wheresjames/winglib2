# wl2:slint — Declarative UI

`wl2:slint` is a built-in module that brings declarative UI to JavaScript by
wrapping the [Slint](https://slint.dev) toolkit's interpreter. JavaScript
compiles `.slint` markup at runtime, instantiates components, reads and writes
properties, wires callbacks, opens a window, and runs the Slint event loop.

The module is gated behind `WL2_ENABLE_SLINT` (default **OFF**) and links into the
`wl2` runner statically when enabled. Full documentation lives with the module:

- [API reference](../modules/wl2_slint/docs/api.md) — `compile`, `compileFile`,
  native dialogs, and the `Component` / `Instance` surface (`get`/`set`,
  `on`/`invoke`, `colorScheme`, `show`/`hide`, `run`/`quit`).
- [Security model](../modules/wl2_slint/docs/security.md) — the default-denied UI
  capability, native dialog path disclosure, resource/filesystem policy, and
  threading.
- [Design](../modules/wl2_slint/docs/design.md) — interpreter binding, native
  ownership, the value bridge, the callback/JS-handler lifetime, and the
  event-loop pump.
- [README](../modules/wl2_slint/README.md) and the
  [`examples/counter`](../modules/wl2_slint/examples/counter) single-executable
  example.

## Quick start

```sh
cmake -S . -B build -DWL2_ENABLE_SLINT=ON -DWL2_DEPS_SLINT=download
cmake --build build
build/app/wl2/wl2 run --allow-ui app.js
```

```js
import { compile } from "wl2:slint";

const ui = await compile(`
  export component MainWindow inherits Window {
    in-out property <string> name: "world";
    in-out property <int> count: 0;
    callback increment();
    VerticalLayout {
      Text { text: "Hello, " + root.name + " (" + root.count + ")"; }
      Button { text: "Increment"; clicked => { root.increment(); } }
    }
  }
`);

const win = ui.create();
win.set("name", "winglib2");
win.on("increment", () => win.set("count", win.get("count") + 1));
win.show();
await ui.run();   // runs until the window closes; requires --allow-ui
```

Opening a window is **denied by default**; the runtime must grant the UI
capability (CLI flag `--allow-ui`, or `RuntimeOptions::allowUi`). Compiling,
instantiating, and property/callback work need no capability, so headless tests
run ungated. See the security doc for details.

Value marshaling covers number, string, bool, one-level objects (⇄ Slint struct),
arrays (⇄ Slint model), and brush/color properties as CSS-style hex strings.
Images remain a follow-up.

## Dependency provider

Slint is consumed as a **prebuilt C++ binary package** (no Rust toolchain) through
`WL2_DEPS_SLINT` (`auto|local|system|download|off`). Set
`WL2_SLINT_FROM_SOURCE=ON` with `WL2_DEPS_SLINT=download` to build from source;
that path needs `cargo`. See [dependencies.md](dependencies.md).

## Tests

The default suite is **headless** and needs no display (compile/instantiate,
property and model round-trips, `on`/`invoke`, `compileFile` from a resource, and
the default-denied UI capability). Windowed tests that open a real window are
labeled `display`, registered only with `-DWL2_SLINT_DISPLAY_TESTS=ON`, and run
under a virtual framebuffer (e.g. `xvfb-run` with `SLINT_BACKEND=winit-software`).
See [testing.md](testing.md).
