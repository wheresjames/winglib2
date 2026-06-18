# wl2_slint Showcase Example

This example is a larger `wl2:slint` application built as one static executable.
It embeds `scripts/main.js` and `ui/app.slint` as `wl2:/showcase/...` resources
and drives the Slint interpreter from JavaScript.

It demonstrates:

- runtime `compileFile()` from embedded resources
- standard widgets: buttons, check boxes, line edit, slider
- light, dark, and system/default theme modes
- custom Slint components
- layouts, rectangles, conditional UI, modal popups, and timers
- native file/folder dialogs, mock file selection, and accent color selection
- JS-owned scalar, struct, and array-model properties
- callbacks with string, integer, float, and boolean arguments
- JavaScript updates reflected back into the UI

Build with the module examples enabled:

```sh
cmake -S . -B build -DWL2_ENABLE_SLINT=ON -DWL2_BUILD_EXAMPLES=ON
cmake --build build --target wl2_slint_showcase_example
```

Run the embedded executable:

```sh
build/modules/wl2_slint/examples/showcase/wl2_slint_showcase_example
```

Or run through the `wl2` runner from the source tree:

```sh
build/app/wl2/wl2 run --allow-ui \
  --map-resource modules/wl2_slint/examples/showcase:wl2:/showcase \
  wl2:/showcase/scripts/main.js
```

Headless compile smoke check:

```sh
build/modules/wl2_slint/examples/showcase/wl2_slint_showcase_example --compile-only
```

`--theme=Light` or `--theme=Dark` starts in an explicit theme. The default
`System` mode uses `Instance.colorScheme()` so the custom palette follows the
Slint backend's reported light/dark style when it is available.

`--selftest` opens the window, drives a few callbacks from a Slint timer, and
quits. It still needs a working display backend.

Native dialog buttons require a build with nativefiledialog-extended support and
the UI capability. If the target was built without native dialog support, the
showcase reports the `slint_unsupported` error in its status line.
