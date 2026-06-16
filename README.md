# Winglib2

**Write your app in JavaScript, call into fast C++ when you need it, and ship the
whole thing as a single self-contained executable.**

Winglib2 (the command-line tool is called `wl2`) is a small, embeddable runtime.
You write your program's logic in JavaScript, and when you need speed or access
to the system — networking, files, shared memory — you reach for built-in
**native modules** written in C++. Your scripts, config, and assets can be baked
directly *inside* the executable as **resources**, so a finished app is one
binary you can copy to a machine and run. No `node_modules`, no runtime to
install alongside it.

It is built on modern C++20 with [QuickJS](https://bellard.org/quickjs/) (a tiny,
fast JavaScript engine) by default, and it is designed from the start to be
cross-compiled down to small or embedded targets.

### A quick mental model

```
  Your JavaScript
  ----------------------------------------
    import { get } from "wl2:curl"        ->  native modules (HTTP, files, shared memory)
    wl2.resources.readText("wl2:/...")    ->  assets baked into the binary
    wl2.thread.spawn("wl2:/worker.js")    ->  worker threads + message passing
                    |
                    v
  +------------------------------------+
  |   wl2 runtime  (C++20 + QuickJS)   |  ->  one self-contained executable
  +------------------------------------+
```

### Who is this for?

- People who like writing application logic in JavaScript but want the
  distribution story of a compiled binary.
- Developers targeting small or embedded systems who need a runtime that builds
  statically and cross-compiles cleanly.
- Anyone who wants explicit, sandboxed native modules instead of an open-ended
  "the script can do anything" environment.

Winglib2 is **not** trying to be Node.js. It is a smaller, more explicit host:
modules and filesystem access are opt-in, and the surface is intentionally
narrow. It is a modernized successor to the original Winglib runtime, keeping the
parts that worked (self-contained executables, native modules, resources,
threads, strong binary buffers) on a smaller C++20 stack.

### What works today

The project is still early, but there is a complete, working vertical slice:

- A CMake-based build with a local, target-specific dependency layout.
- QuickJS as the default JavaScript engine (with a V8 backend kept in the
  architecture for later).
- The `wl2` command-line runner.
- Embedded resources, including a compressed representation.
- Native modules: `wl2:curl` (HTTP) and the read-only `wl2:fs` (filesystem),
  plus `wl2:membus` shared-memory primitives — all importable from JavaScript.
- JavaScript globals `wl2.runtime`, `wl2.resources`, and `wl2.thread`.
- A `wl2_add_module()` CMake helper for building your own native modules.
- A full CTest suite covering curl, membus, fs, resources, the runtime, the
  thread APIs, and runnable examples.

---

## Get Started in 5 Minutes

> **In short:** build the project once, run a script, then scaffold your own app.

**1. Build it.** From the directory that contains the `winglib2/` folder:

```sh
cmake -S winglib2 -B winglib2/build -G Ninja
cmake --build winglib2/build
```

(No Ninja? Drop `-G Ninja` and CMake uses its default generator.)

**2. Run a script.** This runs one of the bundled examples:

```sh
./winglib2/build/app/wl2/wl2 run winglib2/examples/cpp/embedded/scripts/main.js
# → hello from embedded winglib2
```

**3. Create your own app.** `wl2 init` scaffolds a complete project — a manifest,
source files, tests, a CMake build, a README, and a `.gitignore`:

```sh
wl2 init hello_wl2
cd hello_wl2
wl2 run  --manifest wl2.yml      # run your app
wl2 test --manifest wl2.yml      # run its tests
```

That's the whole loop: write JavaScript, run it with `wl2`, and (when you're
ready) build it into a standalone executable.

New to a command? Every command has built-in help:

```sh
wl2 --help                 # list all subcommands
wl2 resources --help       # help for one subcommand
```

---

## How It Fits Together

> **In short:** a few core ideas you'll see throughout the docs.

| Concept | Plain-English meaning |
| --- | --- |
| **The runner (`wl2`)** | The command that runs your scripts and apps, and builds standalone executables. |
| **The manifest (`wl2.yml`)** | A small YAML file describing your app: its entry script, resources, required modules, and tests. One file drives both `wl2 run` during development and the embedded build. |
| **Native modules** | C++ libraries your JavaScript can `import`, like `wl2:curl`. Some are built in; you can write your own. |
| **Resources** | Files (scripts, config, images) embedded inside the executable, read at runtime through `wl2.resources` using `wl2:/...` paths. |
| **Scopes** | Where modules and apps get installed: `local` (this project), `user` (your account), or `system` (the whole machine). |
| **The thread tree** | Worker scripts arranged in a tree, talking to each other with messages and request/reply calls. |

A fuller table of contents:

- [Get Started in 5 Minutes](#get-started-in-5-minutes)
- [How It Fits Together](#how-it-fits-together)
- [Building and Installing](#building-and-installing)
- [Dependencies](#dependencies)
- [JavaScript Engine Backends](#javascript-engine-backends)
- [Running Scripts](#running-scripts)
- [Native Modules](#native-modules)
- [The `wl2:curl` Module](#the-wl2curl-module)
- [The `wl2:fs` Module](#the-wl2fs-module)
- [Embedded Resources](#embedded-resources)
- [Runtime JavaScript API](#runtime-javascript-api)
- [Thread Tree JavaScript API](#thread-tree-javascript-api)
- [Installing Apps](#installing-apps)
- [Tests](#tests)
- [Repository Layout](#repository-layout)
- [Documentation](#documentation)
- [Project Direction and Limitations](#project-direction-and-limitations)

---

## A Taste of the API

> **In short:** here's what real Winglib2 JavaScript looks like.

```js
import { get, post, CurlClient } from "wl2:curl";

const response = await get("https://example.com/", {
  timeoutMs: 5000,
  headers: {
    "User-Agent": "winglib2/0.1"
  }
});

console.log(response.status, response.body.text());
```

The `wl2:curl` response body is a `wl2.Buffer` (an efficient binary buffer type).

---

## Building and Installing

> **In short:** configure with CMake, build, test, and (optionally) install so
> other projects can find Winglib2.

**Build and test:**

```sh
cmake -S winglib2 -B winglib2/build -G Ninja
cmake --build winglib2/build
ctest --test-dir winglib2/build --output-on-failure
```

**Install** (for a release build you can put on a system):

```sh
cmake -S winglib2 -B winglib2/build -DCMAKE_BUILD_TYPE=Release
cmake --build winglib2/build --parallel
sudo cmake --install winglib2/build --prefix /usr/local
```

Installing to `/usr/local` usually needs `sudo`. For a per-user install that
doesn't, point the prefix somewhere you own:

```sh
cmake --install winglib2/build --prefix "$HOME/.local"
```

**Uninstall** from the build tree, or with the installed script if the build tree
is gone:

```sh
cmake --build winglib2/build --target uninstall
cmake -P "$HOME/.local/lib/cmake/winglib2/winglib2Uninstall.cmake"
```

**Use it from another project.** After installing, downstream CMake projects find
Winglib2 with `find_package(winglib2 CONFIG REQUIRED)`:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build
```

The install ships the `wl2` runner, public headers, CMake package files, helper
modules such as `WL2Resources.cmake`, and imported targets such as
`winglib2::wl2_core`, `winglib2::wl2`, `winglib2::wl2_curl_static`, and
`winglib2::wl2_membus_static` when those modules are available.

**Project and build metadata.** `PROJECT.txt` is the preferred source for the
top-level project name, semantic version, description, URL, and optional
`build` value. When `build` is omitted, CMake generates a timestamp in
`YYYY.MM.DD.HHMM` form. The semantic version remains the compatibility version;
the build stamp is exposed separately so different builds of the same version can
be distinguished without changing version policy. In-tree builds can fall back to
source-root defaults when `PROJECT.txt` is absent; standalone projects should
ship their own metadata so missing critical fields fail during configure.

## Dependencies

> **In short:** Winglib2 fetches and builds its own dependencies into a local,
> per-target folder, so your system stays clean and cross-builds stay
> reproducible.

By default, dependencies live under a target-specific root:

```text
winglib2/.deps/<system>-<arch>/<build-type>
# e.g. winglib2/.deps/linux-x86_64/debug/quickjs
```

QuickJS is fetched and built locally when missing (the default, controlled by
`WL2_FETCH_DEPS=ON`). Each dependency follows the same provider pattern — `auto`,
`local`, `package`, `fetch`, or `off` — where `auto` prefers the local root and
refuses to silently fall back to system packages while cross-compiling.

The most useful configure options:

```sh
-DWL2_JS_ENGINE=quickjs        # default engine
-DWL2_JS_ENGINE=v8             # optional V8 backend
-DWL2_FETCH_DEPS=ON            # default: fetch missing deps where supported
-DWL2_QUICKJS_PROVIDER=auto    # auto, local, package, fetch, off
-DWL2_CURL_PROVIDER=auto       # auto, local, package, fetch, off
-DWL2_BUILD_SHARED_MODULES=OFF # default; dynamic module loading is experimental
-DWL2_ENABLE_STRESS_TESTS=OFF  # default; opt in to stress CTest entries
-DWL2_ENABLE_LIBMEMBUS=ON      # default
```

Defaults are centralized in `cmake/WL2Options.cmake`. See
[docs/dependencies.md](docs/dependencies.md) for the full story.

## JavaScript Engine Backends

> **In short:** QuickJS is the default and recommended engine; V8 is supported in
> the design but is heavier and optional.

### QuickJS (default)

QuickJS is small, builds statically, and fits the project's cross-build goals. If
it's missing, CMake fetches and builds it for you:

```sh
cmake -S winglib2 -B winglib2/build -DWL2_JS_ENGINE=quickjs
```

### V8 (optional)

V8 stays in the architecture for the future, but it has a much heavier build and
dependency story, so it isn't the default. To build against a V8 install:

```sh
cmake -S winglib2 -B winglib2/build-v8 -G Ninja \
  -DWL2_JS_ENGINE=v8 \
  -DWL2_V8_ROOT=/path/to/v8
```

The helper script [tools/install-v8.sh](tools/install-v8.sh) can build and stage
a local V8 monolith, but QuickJS is the recommended path for now.

## Running Scripts

> **In short:** `wl2 run` executes a script or a whole manifest, with helpful
> diagnostics, a watch mode, and crash reporting.

Run a plain script file:

```sh
./winglib2/build/app/wl2/wl2 run winglib2/examples/cpp/embedded/scripts/main.js
# → hello from embedded winglib2
```

Inspect a native module's API, or pass your own arguments after the script:

```sh
./winglib2/build/app/wl2/wl2 showapi wl2:curl
./winglib2/build/app/wl2/wl2 run script.js alpha beta   # wl2.runtime.argv === ["alpha", "beta"]
```

**Helpful errors.** Stack traces are on by default; use `--stack-traces=off` for
compact errors. If a generated script ends with a `//# sourceMappingURL=...`
comment, Winglib2 remaps stack locations back to your original source (external
map files and inline base64 maps are both supported).

```sh
./winglib2/build/app/wl2/wl2 run --stack-traces=off winglib2/test/scripts/failing.js
```

**See your configuration** before running — manifest, engine, modules, resource
maps, filesystem policy, and dependency state. Add `--json` for a stable,
machine-readable form:

```sh
./winglib2/build/app/wl2/wl2 config --manifest wl2.yml
./winglib2/build/app/wl2/wl2 config --manifest wl2.yml --json
```

**Watch mode** re-runs your app as you edit. It tracks the manifest, lockfile,
mapped resources, and scripts; changes to native/module libraries are reported as
`rebuild-needed` (watch mode doesn't rebuild native code):

```sh
./winglib2/build/app/wl2/wl2 run --manifest wl2.yml --watch
```

**Shebang scripts** work when `wl2` is on your `PATH`:

```js
#!/usr/bin/env -S wl2 run
console.log("hello from wl2");
```

### Crash Reports

If the runtime hits a fatal native signal (a segfault, abort, etc.), it can write
a crash report — the signal, process info, engine, manifest, loaded modules, and
the crashing thread's C++ stack, plus a machine-readable JSON trailer:

```sh
# Default: writes crash-YYYYMMDD-HHMMSS-PID.log in the working directory.
./winglib2/build/app/wl2/wl2 run --manifest wl2.yml

# Or choose a directory, an explicit path, or turn it off.
./winglib2/build/app/wl2/wl2 run --manifest wl2.yml --crash-report-dir ./artifacts
./winglib2/build/app/wl2/wl2 run --manifest wl2.yml --crash-report=./crash.log
./winglib2/build/app/wl2/wl2 run --manifest wl2.yml --crash-report=off
```

Windows crash handling is deferred, and only the crashing thread's stack is
captured today. Details: [docs/crash-reports.md](docs/crash-reports.md).

### Native Debugging

Debug the runner and native modules with GDB or LLDB by handing them the usual
`wl2 run` arguments:

```sh
cmake -S winglib2 -B winglib2/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build winglib2/build --target wl2
gdb --args winglib2/build/app/wl2/wl2 \
    run --manifest winglib2/examples/js/resources/resources.yml
```

Ready-to-copy VSCode `launch.json` and `tasks.json` live in
[docs/debug/](docs/debug/). JavaScript step debugging is deferred until V8
inspector work; until then, lean on stack traces, source maps, and crash reports.
Full guide: [docs/native-debugging.md](docs/native-debugging.md).

## Native Modules

> **In short:** modules are C++ libraries your JavaScript can import. Prefer
> linking them in statically; on Linux/macOS you can also load them dynamically.

Static modules are the recommended path: they're linked into `wl2` (or into a
standalone executable you generate) and registered with the runtime at startup.
You build them with the `wl2_add_module()` helper from `WL2Modules.cmake`, which
creates the static target, optionally a dynamic one (`WL2_BUILD_SHARED_MODULES=ON`),
and wires up include paths, dependencies, and packaging. Conventions and a
checklist for writing one are in [docs/modules.md](docs/modules.md).

**Dynamic loading (Linux/macOS).** A module can also be a shared library loaded
through a small versioned C ABI. Loading is deliberately limited: modules load
only from explicit paths (never by scanning directories), there is no unload, and
Windows is out of scope for now.

```sh
# Inspect a dynamic module's metadata without running anything.
./winglib2/build/app/wl2/wl2 module validate path/to/libwl2_echo.so
# prints version and, for current modules, build.

# Load a module by explicit path, then run a script that imports it.
./winglib2/build/app/wl2/wl2 run --load-module path/to/libwl2_echo.so app.js
```

**Pinning and installing modules.** A manifest can declare module dependencies
under `dependencies.modules`, pinned to a `tag` or `commit` (floating branches
are rejected). `wl2 deps` resolves and fetches them; `wl2 module` installs built
libraries into a scope:

```sh
wl2 deps lock      # resolve tags to commits, write wl2.lock.yml
wl2 deps fetch     # clone/checkout into ./.wl2/deps
wl2 deps status    # show locked/fetched state

wl2 module install path/to/libwl2_widgets.so --scope local   # or user / system
wl2 module list --scope all
wl2 module uninstall wl2:widgets --scope local
```

At runtime, `wl2 run` resolves a manifest's required/optional modules in priority
order (manifest/lockfile → local → user → system → built-ins), and `wl2 config`
flags anything shadowed by a higher-priority scope.

**Modules can live outside this tree.**
[examples/modules/wl2_echo](examples/modules/wl2_echo) is a complete,
dependency-free reference module that builds standalone against an installed
package with `find_package(winglib2 CONFIG REQUIRED)`. The `outoftree.wl2_echo`
CTest case installs Winglib2 into a throwaway prefix and builds and tests the
module there — proof that third-party modules work. See
[docs/modules.md](docs/modules.md) for the dependency, lockfile, scope, and
resolution details.

## The `wl2:curl` Module

> **In short:** make HTTP requests from JavaScript, backed by libcurl, with safe
> defaults.

```js
import { get, post, CurlClient } from "wl2:curl";

const a = await get("https://example.com/", {
  timeoutMs: 5000,
  followRedirects: true,
  headers: { "User-Agent": "winglib2/0.1" }
});

const b = await post("https://example.com/api", JSON.stringify({ ok: true }), {
  timeoutMs: 5000,
  headers: { "Content-Type": "application/json" }
});

const client = new CurlClient();
const c = await client.request({ method: "GET", url: "https://example.com/" });
```

A response looks like this:

```js
{
  status: 200,
  url: "https://example.com/",
  headers: { "content-type": "text/html" },
  body: wl2.Buffer.fromText("..."),
  timing: { totalMs: 12.3 }
}
```

**Safe by default:** TLS peer and host verification are on, redirects are off
unless you ask for them, HTTP/HTTPS are the intended protocols, and the module
deliberately can't fetch-and-execute a URL.

## The `wl2:fs` Module

> **In short:** read files from the host, but only inside directories the embedder
> has explicitly allowed.

Paths here are real host filesystem paths (not `wl2:/` resource paths):

```js
import { readText, readBytes, exists, stat, list, walk } from "wl2:fs";

const text = readText("data/config.json");
const bytes = readBytes("data/logo.bin");        // returns a wl2.Buffer

if (exists("data")) {
  const info = stat("data");                      // { path, exists, isFile, isDirectory, isSymlink, size }
  for (const entry of list("data")) {             // direct children
    console.log(entry.name, entry.isDirectory ? "dir" : entry.size);
  }
  for (const entry of walk("data")) {             // recursive; paths relative to the root
    console.log(entry.path);
  }
}
```

**It's locked down by default.** Reads are denied unless the embedder turns them
on *and* lists the directories that may be read. Anything outside those roots —
including `..` traversal or symlinks that escape — is rejected with
`code: "fs_permission_denied"`. The module is read-only: no writes, no deletes.

```cpp
wl2::RuntimeOptions options;
options.allowFilesystemReads = true;
options.filesystemReadRoots = {"/srv/app/data"};
options.staticModules.push_back(wl2_fs_register_module);
```

Errors are thrown as `Error` objects with a stable shape — `name` (`"FsError"`),
`module` (`"wl2_fs"`), `code`, `operation`, `path`, and `message` — with codes
such as `fs_permission_denied`, `fs_not_found`, and `fs_read_failed`.

The `wl2` runner registers `wl2:fs` but sets no read roots, so reads are denied
out of the box. [examples/js/fs-inspect](examples/js/fs-inspect) shows how an
embedder enables reads for its own data directory.

## Embedded Resources

> **In short:** bake your scripts and assets into the executable, then read them
> from JavaScript with `wl2:/...` paths — and map a host folder during
> development so you don't have to rebuild on every edit.

**At build time**, the `wl2_add_embedded_resources()` CMake helper turns a
directory of files into embedded resources:

```cmake
wl2_add_embedded_resources(wl2_resources_example
  ROOT ${CMAKE_CURRENT_SOURCE_DIR}/files
  PREFIX wl2:/resources
  AUTO_DIRECTORIES assets
  EXCLUDE_PATTERNS "*.tmp"
  STORE_FILES config.json
  COMPRESS_FILES repeated.txt)
```

**At runtime**, scripts read that tree through `wl2.resources` (and native code
can open a `ResourceHandle` for a stable read-only pointer):

```js
const text = wl2.resources.readText("wl2:/resources/config.json");
const bytes = wl2.resources.readBytes("wl2:/resources/logo.bin");

for (const entry of wl2.resources.walk("wl2:/resources/assets")) {
  console.log(entry.path, entry.size);
}
```

**During development**, skip the rebuild: map a host directory straight into the
`wl2:/` namespace, and optionally trace every lookup:

```sh
./winglib2/build/app/wl2/wl2 run \
  --map-resource winglib2/examples/js/resources/files:wl2:/resources \
  winglib2/examples/js/resources/files/main.js

# add --trace-resources to see hits and misses
```

You can inspect mapped resources from the command line too:

```sh
./winglib2/build/app/wl2/wl2 resources list \
  --map-resource winglib2/examples/js/resources/files:wl2:/resources wl2:/resources
./winglib2/build/app/wl2/wl2 resources read \
  --map-resource winglib2/examples/js/resources/files:wl2:/resources wl2:/resources/config.json
```

### Driving it all from one manifest

For real projects, describe the resource tree once in a manifest. The same file
runs directly through `wl2` *and* drives the embedded build:

```yaml
schema: wl2.resources.v1
prefix: wl2:/resources
root: files
entry: main.js

resources:
  store:
    files:
      - main.js
      - config.json
  compress:
    files:
      - repeated.txt
  auto:
    directories:
      - assets

exclude:
  - "*.tmp"
```

```sh
./winglib2/build/app/wl2/wl2 run    --manifest winglib2/examples/js/resources/resources.yml
./winglib2/build/app/wl2/wl2 config --manifest winglib2/examples/js/resources/resources.yml
```

```cmake
wl2_add_javascript_executable(wl2_resources_js
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/resources.yml)
```

Manifests are validated strictly: loading fails with a specific message for an
unknown key, an unsupported `schema`, a resource path that is absolute or escapes
`root`, a missing file or directory, or a duplicate logical path.

### Inspecting a finished executable

A standalone executable built this way carries an inspectable resource table, so
you (or CI) can list, read, and extract its embedded resources:

```sh
WL2=./winglib2/build/app/wl2/wl2
EXE=./winglib2/build/examples/js/resources/wl2_resources_js
$WL2 resources list    $EXE
$WL2 resources read    $EXE wl2:/resources/config.json
$WL2 resources extract $EXE --out extracted-resources
```

`read` and `extract` return decompressed content by default; pass `--raw` for the
stored bytes. See [docs/resources-executable.md](docs/resources-executable.md).

### Requiring modules from a manifest

A manifest can also list the native modules an app needs:

```yaml
modules:
  require:
    - wl2:echo
  optional:
    - wl2:curl
```

Required modules are checked *before* the entry script runs: `wl2 run --manifest`
fails with `module_required_missing` if one is absent, and the script never
executes. Optional modules are used when present and ignored when missing.

There are runnable examples to learn from:
[examples/js/hello](examples/js/hello) (a minimal script),
[examples/js/resources](examples/js/resources) (stored, compressed, nested, and
excluded resources), and [examples/cpp/resources](examples/cpp/resources) (the
C++ counterpart).

## Runtime JavaScript API

> **In short:** read basic runtime info — version, engine, modules, arguments,
> and allow-listed environment variables — through the `wl2.runtime` object.

```js
wl2.runtime.version      // Winglib2 version string, always available
wl2.runtime.build        // generated or PROJECT.txt build stamp
wl2.runtime.engine       // "quickjs" or "v8", always available
wl2.runtime.modules()    // array of { name, version, build, stableId, summary }
wl2.runtime.argv         // arguments forwarded after the script path
wl2.runtime.env(name)    // read an allow-listed environment variable
```

`argv` holds whatever the host forwards after the script path (the `wl2` command
forwards everything after it; generated standalone executables forward nothing
unless they opt in).

Environment access is **off by default**. `wl2.runtime.env(name)` throws unless
the embedder both enables it and allow-lists the name:

```cpp
wl2::RuntimeOptions options;
options.allowEnvironment = true;
options.environmentAllowList = {"PATH", "HOME"};
```

An allowed-and-set variable returns its value; allowed-but-unset returns `null`;
anything else throws.

## Thread Tree JavaScript API

> **In short:** spawn worker scripts and talk to them with one-way messages or
> bounded request/reply calls.

```js
const worker = wl2.thread.spawn("wl2:/workers/worker.js", { name: "worker" });
worker.post({ type: "hello", payload: "fire and forget" });

const reply = worker.request({ type: "compute", payload: "input" }, {
  timeoutMs: 1000
});

const pending = worker.requestPending({ type: "compute-later", payload: "input" }, {
  timeoutMs: 1000
});
const later = pending.poll() || pending.wait({ timeoutMs: 1000 });

worker.close();
```

See [examples/js/thread-tree](examples/js/thread-tree) for small samples and
[examples/cpp/foundations](examples/cpp/foundations) for a fuller example that combines
resources, a worker thread, request/reply, and `wl2:membus`.

## Installing Apps

> **In short:** install a developer app or internal tool straight from a Git
> source, then run it by name.

```sh
wl2 app install /path/to/repo.git#v1.0.0:apps/hello --scope local
wl2 app list --scope all
wl2 app run hello
wl2 app uninstall hello --scope local
```

`wl2 app install` is a convenience **source** installer — it needs Git, CMake, a
compiler, and Ninja or Make on the machine. For non-technical end users, prefer
shipping a prebuilt standalone executable or a platform package instead. See
[docs/apps.md](docs/apps.md) for scopes, launchers, and limits.

## Tests

> **In short:** one command builds and runs the whole suite; labels let you run
> just the part you care about.

```sh
cmake --build winglib2/build
ctest --test-dir winglib2/build --output-on-failure
```

The default fast suite (67 checks today) covers C++ unit tests, JavaScript smoke
scripts, integration checks, and runnable examples. Run a focused slice by label:

```sh
ctest --test-dir winglib2/build --output-on-failure -L js
ctest --test-dir winglib2/build --output-on-failure -L membus
ctest --test-dir winglib2/build --output-on-failure -L resources
ctest --test-dir winglib2/build --output-on-failure -L thread
ctest --test-dir winglib2/build --output-on-failure -L examples
```

Stress tests are opt-in so they don't slow the default gate:

```sh
cmake -S winglib2 -B winglib2/build -DWL2_ENABLE_STRESS_TESTS=ON
ctest --test-dir winglib2/build --output-on-failure -L stress
```

## Repository Layout

> **In short:** core runtime in `src/` and `include/wl2/`, the runner in
> `app/wl2/`, native modules in `modules/`, runnable samples in `examples/`, and
> guides in `docs/`.

```text
winglib2/
  CMakeLists.txt
  CMakePresets.json
  cmake/                 # build helpers: WL2Modules, WL2Resources, WL2Options, deps/
  include/wl2/           # public headers (wl2.h, runtime.h, resources.h, module.h, ...)
  src/
    core/                # runtime, resources, modules, buffers, thread tree, crash reports
    js/                  # JavaScript engine bindings (QuickJS, V8)
  app/wl2/               # the wl2 command-line runner (main.cpp)
  modules/
    wl2_curl/            # HTTP module
    wl2_fs/              # read-only filesystem module
    wl2_membus/          # shared-memory primitives
  examples/              # runnable cpp/ and js/ samples, plus modules/wl2_echo
  test/                  # core C++ tests, JavaScript smoke scripts, fixtures
  tools/                 # helper scripts (e.g. install-v8.sh)
  docs/                  # the guides linked throughout this README
```

## Documentation

> **In short:** this README is the tour; the `docs/` folder has the deep dives.

- [docs/modules.md](docs/modules.md) — writing native modules (naming,
  registration, errors, buffers, tests), with a checklist.
- [docs/testing.md](docs/testing.md) — the test API and `wl2 init` / `wl2 module
  new` scaffolds.
- [docs/apps.md](docs/apps.md) — app install/uninstall, scopes, and launchers.
- [docs/dependencies.md](docs/dependencies.md) — the dependency/provider model.
- [docs/resources-javascript.md](docs/resources-javascript.md) and
  [docs/resources-executable.md](docs/resources-executable.md) — resources from
  JavaScript and inside executables.
- [docs/crash-reports.md](docs/crash-reports.md) and
  [docs/native-debugging.md](docs/native-debugging.md) — diagnosing problems.

Public headers carry Doxygen comments. Generate the local HTML API reference
with:

```sh
cmake --build winglib2/build --target docs
# → winglib2/build/docs/doxygen/html/index.html
```

## Project Direction and Limitations

> **In short:** Winglib2 is a young but working prototype, built with
> cross-compilation in mind.

It's being shaped for cross builds from the start: dependencies live under
target-specific roots, QuickJS is the default because it builds statically for
embedded targets, and system libraries are treated as a developer convenience
rather than the default embedded path. A future-style cross configure looks like:

```sh
cmake -S winglib2 -B build-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux.cmake \
  -DWL2_JS_ENGINE=quickjs \
  -DWL2_QUICKJS_PROVIDER=fetch
```

**Known limitations** (this is still a prototype):

- `CurlClient` constructor options aren't persisted yet.
- Dynamic module loading is disabled by default.
- The V8 backend needs a complete, compatible V8 install to link.
- JavaScript thread requests are synchronous at the native boundary;
  event-loop-backed promise scheduling is still future work.
- Crash reporting captures only the crashing thread's stack and isn't implemented
  on Windows yet.
- JavaScript step debugging waits on V8 inspector work; native (C++) debugging
  works today.

**Likely next steps:** persist `CurlClient` defaults, add event-loop-backed async
scheduling for thread requests, grow the foundations example into a small app
template, and bring V8 bindings up to parity with QuickJS.

---

Winglib2 intentionally does not try to be Node.js — it's a small, embeddable
JavaScript host with explicit native modules and clear resource rules. The older
`winglib`, `_wl2`, and `libmembus` trees are useful references, but `winglib2` is
meant to grow into a smaller, clearer runtime.
