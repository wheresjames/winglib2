# JavaScript Tests and Scaffolding

`wl2 test` runs JavaScript tests declared by a project manifest:

```sh
wl2 test --manifest wl2.yml
wl2 test --manifest wl2.yml --filter auth
wl2 test --manifest wl2.yml --json
```

The manifest controls test discovery:

```yaml
schema: wl2.project.v1
prefix: wl2:/app
root: files
entry: main.js

tests:
  roots:
    - tests
  pattern: "*.test.js"
```

If `tests.roots` is omitted, `wl2 test` looks in `tests/`. If
`tests.pattern` is omitted, it matches `*.test.js`.

## Test API

The harness provides two globals:

```js
test("sync example", () => {
  assert(1 + 1 === 2, "math failed");
});

test("async example", async () => {
  const value = await Promise.resolve("ok");
  assert(value === "ok", "promise failed");
});
```

`test(name, fn)` registers a test. `fn` may return a promise. `assert(condition,
message)` throws an `Error` when `condition` is false.

`--filter text` runs only tests whose names contain `text`. `--json` prints a
stable summary:

```json
{"total":1,"passed":1,"failed":0,"tests":[{"name":"sync example","status":"passed"}]}
```

Current scope: test files are concatenated into one generated module before
execution. Keep test files focused on registering tests with the provided
globals; arbitrary filesystem `import` between test files is not implemented.

## App Scaffolding

Create a runnable app:

```sh
wl2 init hello_wl2
cd hello_wl2
wl2 run --manifest wl2.yml
wl2 test --manifest wl2.yml
```

The generated tree contains:

```text
wl2.yml
files/main.js
tests/main.test.js
CMakeLists.txt
README.md
.gitignore
```

The generated `CMakeLists.txt` builds a standalone executable when configured
against an installed Winglib2 package:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/winglib2/install
cmake --build build
ctest --test-dir build --output-on-failure
```

## Module Scaffolding

Create a native module skeleton:

```sh
wl2 module new widgets
cd wl2_widgets
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/winglib2/install
cmake --build build
ctest --test-dir build --output-on-failure
```

The module scaffold includes public headers, a source file with static and
dynamic module metadata entry points, a C++ smoke test, CMake, README, and
`.gitignore`. It compiles out of tree against `find_package(winglib2 CONFIG
REQUIRED)` and is intended as the starting point for adding real QuickJS exports.

## Networking Tests

Tests that exercise sockets (for example `wl2:asio`, see [asio.md](asio.md)) must
bind only to loopback (`127.0.0.1`) and ephemeral ports, and must not reach the
public internet, so they stay deterministic and remain in the default suite. A
test grants the capability it needs with `wl2 run --network-allow 127.0.0.1[:port]`
/ `--allow-listen --listen-allow 127.0.0.1`, or sets the equivalent
`RuntimeOptions` fields in a C++ fixture. Any test that must reach outside
loopback is labeled `external-network` and excluded from the default run.

## UI Tests

UI tests (for example `wl2:slint`, see [slint.md](slint.md)) split into two
groups. The default suite is **headless**: it compiles and instantiates
components, round-trips properties and models, wires `on`/`invoke` callbacks, and
loads markup through `compileFile()` — all without opening a window, so it runs
in CI with no display. Opening a window (`show()`/`run()`) is gated by the UI
capability (`wl2 run --allow-ui` or `RuntimeOptions::allowUi`), and a test that
asserts the default-denied behavior needs no display either.

Tests that open a real window are labeled `display`, registered only when
`-DWL2_SLINT_DISPLAY_TESTS=ON`, and kept out of the default run (like
`external-network` for `wl2:asio`). They are deterministic under a virtual
framebuffer — run them with `xvfb-run` and the software backend, for example:

```sh
xvfb-run -a env SLINT_BACKEND=winit-software ctest --test-dir build -L display
```
