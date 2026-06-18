# wl2:slint counter example

A small counter UI driven from JavaScript, built as a **single static
executable** that embeds its `ui/app.slint` and `scripts/main.js` as `wl2:/`
resources (no runtime file access) and grants the UI capability in C++.

Build it with the module enabled and run it:

```sh
cmake -S . -B build -DWL2_ENABLE_SLINT=ON -DWL2_BUILD_EXAMPLES=ON
cmake --build build --target wl2_slint_counter_example
build/modules/wl2_slint/examples/counter/wl2_slint_counter_example
```

A window opens; click **Increment** to bump the counter. Pass `--selftest` to make
it auto-increment once and quit (used by the opt-in `examples.wl2_slint.counter`
smoke test, which runs under a virtual framebuffer).

To iterate on the markup without rebuilding, run the script in-tree with the
example directory mounted as the resource namespace:

```sh
wl2 run --allow-ui \
  --map-resource modules/wl2_slint/examples/counter:wl2:/counter \
  wl2:/counter/scripts/main.js
```
