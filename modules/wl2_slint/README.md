# wl2_slint

A native Winglib2 module exported to JavaScript as `wl2:slint`, wrapping the
[Slint](https://slint.dev) declarative UI toolkit through its interpreter API.

The full interpreter binding (see `WL2-SLINT.md`). You can compile `.slint` markup
from JavaScript (inline or from a `wl2:/` resource), instantiate a component,
round-trip properties and array models, wire callbacks, open a window, and run
the Slint event loop — with host async work pumped on the UI thread so other
modules' promises keep settling while the UI is up.

```js
import { compile } from "wl2:slint";

const ui = await compile(`
  export component App {
    in-out property <string> name: "world";
    in-out property <int> count: 0;
  }
`);

const win = ui.create();
win.set("name", "winglib2");
win.set("count", win.get("count") + 1);
console.log(win.get("name"), win.get("count"));
```

## Status

Implemented:

- `compile(source, options) -> Promise<Component>`
- `compileFile(specifier, options) -> Promise<Component>` (resource/fs subject to policy)
- `Component.create() -> Instance`
- `Component.run() -> Promise<void>` / `Component.quit()` — the event loop, gated
  by the **UI capability** (`run`)
- `Instance.get(property)` / `Instance.set(property, value)` for number, string,
  bool, one-level objects (JS object ⇄ Slint struct), arrays (JS array ⇄ Slint
  model), and brush/color hex strings (`#rgb`, `#rrggbb`, `#rrggbbaa`)
- `Instance.on(callback, fn)` / `Instance.invoke(callback, ...args)`
- `Instance.colorScheme()` for `"unknown"`, `"dark"`, or `"light"` backend style
  detection
- `Instance.show()` / `Instance.hide()` — gated by the **UI capability**, which is
  denied by default
- Native dialogs: `openFileDialog`, `openFilesDialog`, `saveFileDialog`, and
  `pickFolderDialog` when nativefiledialog-extended is available
- `compileFile()` import resolution from filesystem and mounted `wl2:/` resources
- `examples/counter` — a single static executable that embeds its UI and runs it
- `examples/showcase` — a richer static executable that demonstrates standard
  widgets, light/dark/system theme state, modal popups, native and mock file
  selection, color selection, layouts, structs, array models, callbacks with
  arguments, and timers

Follow-ups: images and a co-driven event loop.

## Building

The module is gated behind `WL2_ENABLE_SLINT` (default **OFF**) and is a no-op for
default builds. Slint is consumed as a **prebuilt C++ binary package** so no Rust
toolchain is required:

```sh
cmake -B build -DWL2_ENABLE_SLINT=ON -DWL2_FETCH_DEPS=ON \
  -DWL2_SLINT_URL_HASH=SHA256=<hash-for-your-platform-package>
```

See `cmake/options.cmake` for the full provider knobs
(`WL2_SLINT_PROVIDER`, `WL2_SLINT_VERSION`, `WL2_SLINT_URL`, …). Building from
source (`WL2_SLINT_PROVIDER=source`) is opt-in and is the only path that needs
`cargo`.

Native dialogs are optional and use
[`nativefiledialog-extended`](https://github.com/btzy/nativefiledialog-extended)
when supported. `WL2_SLINT_NATIVE_DIALOGS=ON` is the default, but unsupported
embedded targets or missing desktop prerequisites disable the feature in
`auto`. Use `WL2_SLINT_NFD_PROVIDER=fetch|local|package|off` to force a provider.
Linux fetch builds use NFDe portal mode and need `dbus-1` development files.

## Tests

All default tests are headless and need no display:

- `slint.wl2_slint` — native: compile, instantiate, property and model
  round-trips, callback `on`/`invoke` marshaling, compile-error diagnostics,
  `show()` denial, and native-dialog permission denial.
- `scripts.wl2_slint_compile_properties` — the property round-trip through the runner.
- `scripts.wl2_slint_compile_file` — `compileFile()` from a mounted resource (with
  an import).
- `scripts.wl2_slint_callbacks` — `on`/`invoke` through the runner.
- `scripts.wl2_slint_permission_denied` — `show()` denied without `--allow-ui`.
- `scripts.wl2_slint_module_validate` — the dynamic module reports the same identity.

Opt-in windowed tests (label `display`, registered only with
`-DWL2_SLINT_DISPLAY_TESTS=ON`) open a real window: `scripts.wl2_slint_windowed_smoke`
and the example smoke tests. Run them under a display, e.g. `xvfb-run -a env
SLINT_BACKEND=winit-software ctest -L display`.

See `docs/` for the API, security model, and design notes.
