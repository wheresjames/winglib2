# Winglib2 Module Conventions

This document is the reference for adding native modules to Winglib2. A module
is self-contained: adding one should mean adding a directory under `modules/`
or passing one through `WL2_EXTRA_MODULE_DIRS`, with its marker, options,
dependencies, packaging contributions, examples, and tests.

Modules can be linked in **statically** (registered through
`RuntimeOptions::staticModules`) or loaded **dynamically** from a shared library
on Linux and macOS through the small versioned C ABI described in
[Dynamic Module Loading](#dynamic-module-loading). Static registration is still
the simplest path; dynamic loading is a Linux/macOS MVP and never scans
arbitrary directories — a module is loaded only from an explicit path.

The supported JavaScript backend today is **QuickJS**. V8 is architectural and
experimental until it has equivalent module tests and examples; see
[Backend Support Policy](#backend-support-policy).

## Naming

| Thing                         | Convention                          | Example                         |
|-------------------------------|-------------------------------------|---------------------------------|
| JavaScript module specifier   | `wl2:<name>`                         | `wl2:fs`                         |
| Source directory              | `modules/wl2_<name>/`                | `modules/wl2_fs/`               |
| Public header                 | `include/wl2_<name>/wl2_<name>.h`     | `include/wl2_fs/wl2_fs.h`        |
| Source file                   | `src/wl2_<name>.cpp`                 | `src/wl2_fs.cpp`                |
| Static registration function  | `wl2_<name>_register_module`         | `wl2_fs_register_module`        |
| QuickJS module factory        | `wl2_<name>_quickjs_module_factory`  | `wl2_fs_quickjs_module_factory` |
| Static CMake target           | `wl2_<name>_static`                  | `wl2_fs_static`                 |
| Dynamic CMake target (MODULE) | `wl2_<name>`                         | `wl2_fs`                        |
| CMake feature switch          | `WL2_ENABLE_<NAME>`                  | `WL2_ENABLE_FS`                 |

Use descriptive names in code. Do not encode `static` into the module name and
do not use roadmap milestone labels in file, function, or comment names.

## Source Layout

Built-in modules are discovered from directories that contain both
`CMakeLists.txt` and `wl2.module.source.yml`:

```text
modules/wl2_<name>/
  wl2.module.source.yml
  CMakeLists.txt
  cmake/options.cmake
  cmake/<dependency helpers>.cmake
  include/wl2_<name>/wl2_<name>.h
  src/wl2_<name>.cpp
  test/CMakeLists.txt
```

The marker supplies discovery and default option metadata:

```yaml
schema: 1
provides: wl2:<name>
version: 0.1.0
category: core      # or extended
```

Core modules default to enabled. Extended modules default from
`WL2_ENABLE_EXTENDED_MODULES`. `WL2_ENABLE_ALL_MODULES=ON` forces every
discovered module option on, including modules that normally default off.
`WL2_DISABLE_MODULES` skips by directory name or provided module name, and
`WL2_EXTRA_MODULE_DIRS` adds discovery roots without editing global CMake.

Put module cache options in `cmake/options.cmake`. This includes the enable
switch and any module dependency provider defaults, source URLs, versions,
hashes, and build knobs.

## Header Layout

A module header declares exactly three entry points and includes only
`wl2/module.h`:

```cpp
#pragma once

#include "wl2/module.h"

wl2::ModuleInfo wl2_fs_register_module(wl2::Runtime& runtime);

extern "C" int wl2_module_get_info(wl2_module_info* out);
extern "C" int wl2_module_register(const wl2_module_host* host);
extern "C" void* wl2_fs_quickjs_module_factory(void* context, const char* moduleName);
```

- `wl2_<name>_register_module` is the **static** entry point, called by the host
  during `Runtime::initialize()` for every entry in
  `RuntimeOptions::staticModules`. It registers the QuickJS factory and returns
  the module's `ModuleInfo`. Define it only in the static build (guard with
  `#if WL2_<NAME>_STATIC_MODULE`).
- `wl2_<name>_quickjs_module_factory` is the QuickJS module loader callback. It
  builds the `JSModuleDef`, adds its exports, and is reached through the void
  pointer ABI in `wl2/module.h`.
- `wl2_module_get_info` and `wl2_module_register` are the C ABI entry points for
  the **dynamic** build only. `get_info` reports metadata (and is what
  `wl2 module validate` reads); `register` attaches the module's factories to the
  host through the `wl2_module_host` callback table. Guard both so multiple static
  modules can be linked into one executable without a duplicate-symbol error:

  ```cpp
  #if !WL2_FS_STATIC_MODULE
  extern "C" int wl2_module_get_info(wl2_module_info* out) { /* ... */ }
  extern "C" int wl2_module_register(const wl2_module_host* host) {
      if (!host || !host->register_quickjs_module) return 1;
      host->register_quickjs_module(host->host, "wl2:fs", wl2_fs_quickjs_module_factory);
      return 0;
  }
  #endif
  ```

  The static target defines `WL2_<NAME>_STATIC_MODULE=1`
  (`STATIC_PRIVATE_COMPILE_DEFINITIONS` below), so these symbols exist only in
  the dynamic module where the loader needs them.

Document public module headers as the module is developed. Doxygen comments
should cover registration functions, dynamic ABI entry points, public data
types, lifecycle rules, and security-sensitive behavior visible to embedders.
Module-local Markdown under `modules/wl2_<name>/docs/` should describe the
JavaScript API, security model, and examples so generated Doxygen output has a
useful module page rather than only bare symbols.

## ModuleInfo

Fill every field. Use a fresh UUID for `stableId` so tooling and manifests have
a name-independent identifier.

```cpp
return wl2::ModuleInfo{
    .abiVersion = wl2::ModuleAbiVersion,
    .name = "wl2:fs",
    .version = WL2_VERSION,                       // or the backing library version
    .build = WL2_BUILD,                           // generated build stamp
    .stableId = "b3a1f2d4-6c8e-4a17-9b2f-0d5e7c1a4e90",
    .summary = "Read-only host filesystem module gated by runtime policy.",
    .api = FsApi,                                // multi-line string shown by `wl2 showapi`
    .unloadSafe = true,
};
```

`version` is semantic compatibility or the backing library version. `build` is a
separate provenance stamp for distinguishing two builds of the same version.
`wl2 showapi wl2:<name>` prints `name`, `version`, `build`, `summary`, and `api`,
so keep `api` a concise, accurate description of the exported surface and any
security defaults.

## CMake With wl2_add_module()

`wl2_add_module()` owns target creation, include interfaces, link dependencies,
compile definitions, header install, export registration, and builtin static
module registry metadata. A module `CMakeLists.txt` should include
`cmake/options.cmake`, return when disabled, resolve any module-owned
dependencies, call `wl2_add_module()`, and add its own examples/tests.

A dependency-free module:

```cmake
include(cmake/options.cmake)

if(NOT WL2_ENABLE_EXAMPLE)
    return()
endif()

wl2_add_module(wl2_example
    MODULE_NAME wl2:example
    REGISTER_FUNCTION wl2_example_register_module
    STABLE_ID <uuid>
    FEATURE WL2_ENABLE_EXAMPLE
    SOURCES src/wl2_example.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_EXAMPLE_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")
```

A module with an external library resolves and packages that dependency in
module-owned CMake:

```cmake
include(cmake/options.cmake)
include(cmake/WL2ExampleDep.cmake)
wl2_find_example_dep()

wl2_add_module(wl2_example
    MODULE_NAME wl2:example
    REGISTER_FUNCTION wl2_example_register_module
    FEATURE WL2_ENABLE_EXAMPLE
    SOURCES src/wl2_example.cpp
    PUBLIC_LINK_LIBRARIES wl2_core ${WL2_EXAMPLE_DEP_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_EXAMPLE_DEP_TARGET})

wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2ExampleDep.cmake"
    DESTINATION modules/wl2_example)
wl2_module_package_config_fragment(CONTENT "...")
```

`wl2_add_module()` builds the static target when `WL2_BUILD_STATIC_MODULES=ON`
(default) and the dynamic `MODULE` target when `WL2_BUILD_SHARED_MODULES=ON`.
Install, export, runner linkage, and static registration are driven from the
module registry. The root build discovers modules with `wl2_add_modules()`, so
new modules should not be manually wired in the root build or `app/wl2/`.
Module-specific examples and tests should be added by the owning module, using
`wl2_add_module_tests()` for a module-local `test/` directory.

### Static Dependency Graphs

A built-in static module can declare dependencies on other built-in static
modules with `REQUIRES_MODULES` and `OPTIONAL_MODULES` (canonical module names):

```cmake
wl2_add_module(wl2_asio
    MODULE_NAME wl2:asio
    REGISTER_FUNCTION wl2_asio_register_module
    SOURCES src/wl2_asio.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    REQUIRES_MODULES wl2:threads
    OPTIONAL_MODULES wl2:metrics)
```

These declarations should mirror the module's C++ `ModuleInfo::dependencies`; the
`core.static_module_metadata` test fails the build if the CMake-declared and
C++-declared dependency name sets drift apart.

`wl2_resolve_builtin_modules()` topologically sorts the registered static modules
by these edges, and `wl2_generate_static_module_registry()` emits the runner's
link targets and registration calls in dependency-first order, so a dependency
is always linked and registered before its dependents. Resolution rules:

- A required dependency that is not an enabled built-in static module **fails
  configure** (`is not an enabled built-in static module`). This also prevents a
  single-executable build from requiring a dynamic-only module.
- An optional dependency that is absent is **skipped** with a status message.
- A dependency **cycle** fails configure.

Embedded single-executable apps built with `wl2_add_javascript_executable()` can
still pass an explicit `STATIC_MODULES` list to override the generated registry.

## Module Dependency Graphs

Native modules can declare runtime dependencies in their `ModuleInfo`:

```cpp
info.dependencies.push_back(wl2::ModuleDependencyRequirement{
    .name = "wl2:threads",
    .versionRange = ">=0.1.0 <0.2.0",
    .stableId = "f2a1c0d9-...",
    .kind = wl2::ModuleDependencyKind::Required,
});
```

The resolver consumes available providers in this priority order:

1. Explicit `--load-module` paths.
2. Fetched project module sources under `.wl2/deps/`.
3. Installed local modules.
4. Installed user modules.
5. Installed system modules.
6. Built-in static modules.

Required dependencies fail resolution when unavailable, incompatible, or part of
a cycle. Optional dependencies produce diagnostics and are skipped when they
cannot be resolved. The initial version range grammar is intentionally small:
empty, exact `=x.y.z`, or bounded `>=x.y.z <a.b.c`.

Use `wl2 module graph` to inspect the resolved plan for a project manifest:

```sh
wl2 module graph --manifest wl2.yml
wl2 module graph --manifest wl2.yml --json --load-module ./build/wl2_example.so
```

The text form prints dependency-first load order. The JSON form reports roots,
selected providers, provider source, path, optional status, and diagnostics.

Source-fetched modules should include `wl2.module.source.yml` at the dependency
checkout root or the manifest dependency `path`:

```yaml
schema: wl2.module-source.v1
provides: wl2:asio
version: 0.1.0
stableId: 6e1d9f7a-0e49-4c8c-9f4f-2e9b2e4c8c11
path: .
dependencies:
  required:
    - name: wl2:threads
      version: ">=0.1.0 <0.2.0"
sourceDependencies:
  modules:
    - name: wl2_threads
      git: https://example.com/winglib2-threads.git
      tag: v0.1.0
```

`sourceDependencies.modules` keeps Git dependencies pinned by tag or commit.
Floating branch dependencies are rejected. The `provides` key is accepted there
for forward compatibility, but manifest `dependencies.modules` still uses
`name`, `git`, `tag` or `commit`, and optional `path`.

## Error Objects

Throw `Error` objects with a stable shape so scripts and tooling can branch on
fields instead of message text:

| Field       | Required | Notes                                              |
|-------------|----------|----------------------------------------------------|
| `name`      | yes      | `"<Pascal>Error"`, e.g. `"FsError"`, `"CurlError"` |
| `message`   | yes      | Human-readable; not a stable contract              |
| `code`      | yes      | Stable, machine-readable; see below                |
| `module`    | yes      | The C target name, e.g. `"wl2_fs"`                   |
| `operation` | optional | The exported function, e.g. `"readText"`           |
| `path`      | optional | Filesystem path involved                           |
| `url`       | optional | URL involved                                       |
| `cause`     | optional | Underlying error when wrapping                      |

```cpp
JSValue throw_fs_error(JSContext* ctx, const char* code, const char* operation,
                       const std::string& path, const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "FsError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_fs"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "path", JS_NewString(ctx, path.c_str()));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}
```

### Stable Error Codes

Codes are `snake_case` and prefixed with the module name. They are part of the
public contract and must not be renamed once shipped. `wl2:fs` defines:

`fs_permission_denied`, `fs_not_found`, `fs_not_a_file`, `fs_not_a_directory`,
`fs_read_failed`, `fs_invalid_argument`.

Argument-validation failures may use `JS_ThrowTypeError` directly; everything a
script is expected to catch and branch on should carry a stable `code`.

## Buffer Ownership And Conversion

- Return binary data as a `wl2.Buffer`, never a raw `ArrayBuffer` or string.
- Build buffers through the host bridge: copy bytes into an `ArrayBuffer` with
  `JS_NewArrayBufferCopy`, then call `wl2.buffer.fromArrayBuffer`. The bytes are
  **copied** at the boundary; the module keeps no ownership of script memory and
  the script does not alias native buffers.
- Scripts inspect buffers with `wl2.buffer.isBuffer(value)`, `buffer.byteLength`
  (alias `buffer.size`), and `buffer.text()`.

See `make_wl2_buffer` in `modules/wl2_curl/src/wl2_curl.cpp` and
`modules/wl2_fs/src/wl2_fs.cpp` for the reference helper.

## Async, Timeout, And Cancellation

- Native calls run synchronously at the C boundary; a call that performs I/O
  blocks the calling JavaScript thread until it returns.
- Operations that can block should accept a `timeoutMs` option and enforce it in
  native code (`wl2:curl` uses `CURLOPT_TIMEOUT_MS`).
- Results that must integrate with `await` resolve through the engine job queue;
  the runtime drains pending jobs after the entry module returns.
- There is no per-call cancellation token yet. Cooperative cancellation today is
  coarse: the runtime can interrupt a script thread at safe checkpoints, but a
  native call already in progress is not interruptible. Do not design an API that
  depends on fine-grained mid-call cancellation.

## Resource Cleanup And Close Semantics

- Objects that hold a native resource expose an explicit `close()` and release
  the resource in a QuickJS finalizer as a backstop.
- `close()` is idempotent and safe to call more than once; calls after close
  either no-op or throw a stable `code`, never crash.
- Prefer returning plain value objects (like the `wl2:fs` stat record or the
  `wl2:curl` response) when there is no long-lived native handle to manage.

## Capability Policy

Modules that expose host capabilities ask the runtime to authorize the operation
instead of deciding policy themselves. `wl2:fs` consults
`Runtime::resolveFilesystemReadPath`; network-capable modules consult the
network gates:

```cpp
if (auto ok = runtime.authorizeNetworkConnect(host, port); !ok) {
    return throw_module_error(ctx, ok.error().code(), /* ... */);
}
```

- `Runtime::authorizeNetworkConnect(host, port)` and
  `Runtime::authorizeNetworkListen(host, port)` return a `Result<void>` and fail
  with the stable codes `network_connect_denied` / `network_listen_denied`.
- Capabilities are **denied by default**. They are permitted only when the
  matching `RuntimeOptions` switch (`allowNetwork` / `allowListening`) is set and
  the endpoint matches an allow-list entry. An **empty allow-list denies all**,
  even with the switch on.
- Allow-list entries are `host:port`, `host` (any port), `host:*`, `*:port`, or
  `*` (any).
- `wl2 config` reports the effective capability policy; `wl2 config --json`
  includes a `capabilities` object.

Authorization lives in the core runtime, never inside a module, so policy is
uniform and inspectable. New host capabilities (process, IPC, filesystem writes)
should follow this same deny-by-default, allow-list, stable-error pattern.

## Async Host Support

Native async modules use the shared async host (`Runtime::async()`,
`wl2/async_host.h`) instead of each re-implementing promise scheduling and
shutdown. The host provides a thread-safe completion queue, outstanding-operation
tracking that keeps the engine event loop alive, and shutdown hooks.

The pattern for an operation that completes off-thread:

```cpp
runtime.async().beginOperation();            // keep the event loop running
worker = std::thread([&runtime, resolve] {
    /* ... native work off the JS thread ... */
    runtime.async().post([resolve] {         // runs on the JS thread
        /* resolve/reject the QuickJS promise here */
    });
    runtime.async().endOperation();
});
```

Lifecycle rules for native async resources:

- Create the JavaScript promise on the JS thread (`JS_NewPromiseCapability`); only
  touch the context and promise from completions, which `post()` runs on the JS
  thread.
- A module that owns worker threads registers a shutdown hook with
  `registerShutdownHook()`. `Runtime`'s destructor calls `async().shutdown()`,
  which runs hooks (stop and **join** worker threads) and drains remaining
  completions — module workers never outlive the runtime.
- Public objects holding native async resources expose an explicit `close()`;
  finalizers are a backstop, not the primary lifecycle API.
- Pending operations settle with a stable error on close, timeout, cancellation,
  or shutdown — never left dangling.

The engine event loop drains posted completions alongside engine jobs and keeps
running while operations are outstanding, so an awaited promise resolved from a
worker thread settles without busy-waiting.

## Tests And Fixtures

Modules should own their module-specific tests under `modules/wl2_<name>/test`
and include them from the module `CMakeLists.txt` with `wl2_add_module_tests()`.
Core runtime tests remain under `test/core`.

- **Labels.** Apply a per-area label plus role labels. Module label is the short
  name (`curl`, `fs`, `membus`, `asio`, `slint`). Role labels in use: `unit`,
  `js`, `integration`, `smoke`, `dynamic`, `examples`, `stress`, `display`.
  Network tests that reach the public internet must be labeled `external-network`
  and excluded from the default suite; `wl2:asio` tests bind only to loopback and
  stay in the default suite. UI tests that open a real window must be labeled
  `display` and excluded from the default suite; `wl2:slint`'s headless tests
  (compile/instantiate, properties, callbacks) stay in the default suite.
- **Fixtures.** Do not add Python or other runtime dependencies for fixtures.
  Use a small in-process C++ fixture (see
  `modules/wl2_curl/test/wl2_curl_fixture.cpp` for a local HTTP server). Keep
  timeout and failure tests deterministic.
- **No source-tree mutation.** Tests that touch the filesystem use temporary
  directories and files and clean up after themselves; they never write into the
  source tree. The `wl2:fs` tests build a temp root and enable
  `RuntimeOptions::filesystemReadRoots` against it.
- **Policy coverage.** When a module is gated by `RuntimeOptions`, test both the
  default-denied path (reachable from the `wl2` runner) and the enabled path
  (reachable from a C++ harness that sets the option), as in
  `modules/wl2_fs/test`.

## Examples For Copied Standalone Builds

Each module should ship a small example that also builds standalone against the
installed package, so copied/installed behavior is exercised.

- Generate a JavaScript executable with `wl2_add_javascript_executable()`,
  embedding the script as a resource. Keep generated standalone executables
  filesystem-off by default; opt into capabilities explicitly (for example
  `FILESYSTEM_READ_ROOTS` for `wl2:fs`).
- Begin the example `CMakeLists.txt` with the standalone fallback block so it can
  configure either inside the tree or against an installed prefix:

  ```cmake
  if(NOT TARGET wl2_core)
      cmake_minimum_required(VERSION 3.24)
      project(wl2_fs_inspect_js LANGUAGES CXX)
      find_package(winglib2 CONFIG REQUIRED)
      enable_testing()
      set(WL2_BUILD_TESTING ON)
  endif()
  ```

- Resolve the module target as either `wl2_<name>_static` or
  `winglib2::wl2_<name>_static`, and register an `examples.js.<name>_standalone`
  CTest entry labeled `examples;js;<name>`.

See `examples/js/fs-inspect` and `examples/js/curl-resources`.

## Declaring Module Requirements

A project manifest (`wl2.yml` / `wl2.resources.v1` or `wl2.project.v1`) declares the
modules an application depends on:

```yaml
modules:
  require:
    - wl2:echo
  optional:
    - wl2:curl
```

- `modules.require` lists modules that must be registered before the entry
  script runs. If any required module is missing, `wl2 run --manifest ...` fails
  with `module_required_missing` **before the script executes**; nothing in the
  entry module runs.
- `modules.optional` lists modules used when present and ignored when absent.
  A missing optional module never blocks startup.

A module name may appear at most once and never in both lists; the manifest
loader rejects duplicates with `manifest_duplicate_module`. `wl2 config
--manifest ...` echoes the declared `modules.require` and `modules.optional`
lists. The same enforcement is available to embedders through
`RuntimeOptions::requiredModules` and `RuntimeOptions::optionalModules`.

## Out-Of-Tree Modules

A third-party module does not need to live in this repository. It can be built
against an installed Winglib2 package with `find_package(winglib2 CONFIG
REQUIRED)`, using the same `wl2_add_module()` helper and naming conventions as
in-tree modules. [`examples/modules/wl2_echo`](../examples/modules/wl2_echo) is the
reference for this shape and is exercised by the `outoftree.wl2_echo` test, which
installs the package into a throwaway prefix and configures, builds, and tests
the example standalone.

Conventions specific to out-of-tree modules:

- Begin `CMakeLists.txt` with the standalone fallback block so the module
  configures either in-tree or against an installed prefix:

  ```cmake
  if(NOT TARGET wl2_core)
      cmake_minimum_required(VERSION 3.24)
      project(wl2_echo VERSION 0.1.0 LANGUAGES CXX)
      find_package(winglib2 CONFIG REQUIRED)
      enable_testing()
      set(WL2_BUILD_TESTING ON)
  endif()
  ```

- Resolve the core target as either `wl2_core` (in-tree) or `winglib2::wl2_core`
  (installed). Pass `NO_INSTALL` to `wl2_add_module()` for an example module so it
  is never installed or exported as part of `winglib2`.
- Provide both a static and a dynamic target. The dynamic `MODULE` target does
  not link `wl2_core` — the host provides those symbols — so it consumes only the
  installed wl2 headers through `DYNAMIC_INCLUDE_DIRS`. The dynamic target exists
  to prove the build shape and to be loadable by the dynamic module loader.
- Ship a project-local JavaScript runner with `wl2_add_javascript_executable()`
  and a C++ test, so the module is verified without the global `wl2` runner.

The installed package ships the QuickJS headers and library so that a module's
source (which includes `<quickjs.h>`) can be compiled out-of-tree, not just
linked against.

## Dynamic Module Loading

On Linux and macOS a native module can be built as a shared library and loaded
at runtime through a small versioned C ABI (`wl2/module.h`). This is a Linux/macOS
MVP with deliberate limitations.

### The C ABI

A dynamic module exports two C entry points:

- `int wl2_module_get_info(wl2_module_info* out)` — fills metadata: `abi_version`,
  `name`, `version`, `build`, `stable_id`, `summary`, `api`, `unload_safe`,
  `required_wl2_version` (the wl2 version the module needs, e.g. `"0.1.0"`), and,
  for ABI v3+, an optional `dependencies` array plus `dependency_count`.
- `int wl2_module_register(const wl2_module_host* host)` — registers the module's
  factories with the host via the `wl2_module_host` callback table (today,
  `register_quickjs_module`). Returns 0 on success.

The current ABI version is `wl2::ModuleAbiVersion` (4). Set
`abi_version = wl2::ModuleAbiVersion` in `get_info`. ABI v3 added ABI-safe
dependency metadata: each `wl2_module_dependency_info` carries a `name`,
`version_range`, `stable_id`, and `required` flag. ABI v4 added the optional
`build` string. The host reads dependencies only when `abi_version >= 3` and
reads `build` only when `abi_version >= 4`; older ABI v2/v3 modules remain
valid and are treated as having no build stamp and, for v2, no dependencies (the
host accepts the range `wl2::ModuleMinAbiVersion`..`wl2::ModuleAbiVersion`).
Malformed dependency metadata (an empty name or an invalid version range) is rejected by
`wl2 module validate` and `wl2 module install` with `module_dependency_invalid`.

### Loading and validation

The host validates a module before it is usable and fails with a specific,
stable error code:

| Situation                                   | Error code                     |
|---------------------------------------------|--------------------------------|
| Library file is missing                     | `module_library_not_found`     |
| `dlopen` failed                             | `module_load_failed`           |
| No `wl2_module_get_info`                      | `module_missing_get_info`      |
| No `wl2_module_register`                      | `module_missing_register`      |
| `abi_version` != host                       | `module_abi_mismatch`          |
| `required_wl2_version` not satisfied by host | `module_wl2_version_mismatch`   |
| Name already registered (no shadow allowed) | `module_duplicate_name`        |

The wl2 version check requires the host to share the module's **major** version
and be at least as new overall. A module name may shadow an already-registered
name (for example a built-in) only when shadowing is explicitly allowed
(`--allow-module-shadow`, or `ModuleShadowPolicy::Allow`); otherwise a collision
is a `module_duplicate_name` error.

### Commands

```sh
# Report a module's metadata without loading it into a running app.
wl2 module validate path/to/libwl2_echo.so
# Output includes version and build when the module reports one.

# Load a dynamic module by explicit path, then run a script that imports it.
wl2 run --load-module path/to/libwl2_echo.so app.js

# Show the resolved dynamic library path and ABI information.
wl2 config --load-module path/to/libwl2_echo.so
```

Module *resolution* by name comes from manifest requirements and installed
module indexes. Explicit `--load-module` paths remain available as a low-level
override, mirroring `--map-resource`.

### Limitations

- **Linux and macOS only.** Loading uses `dlopen`/`dlsym`. Windows dynamic
  loading is not implemented.
- **No unload.** A loaded module stays loaded until the process exits; there is
  no unload path. `unload_safe` is advisory metadata only.
- **Host symbol export.** A dynamic module that calls the JavaScript engine
  resolves those symbols from the host at load time. The host executable must
  export its dynamic symbols (`ENABLE_EXPORTS`, i.e. `-rdynamic`); the `wl2`
  runner does. The module itself does not link the engine.
- **No directory scanning.** Modules are loaded only from explicit paths, never
  by scanning directories.

See [`examples/modules/wl2_echo`](../examples/modules/wl2_echo), whose dynamic
target loads and registers through this ABI and is exercised by the
`scripts.wl2_module_validate` and `scripts.wl2_run_load_module` tests.

## Module Dependencies and the Lockfile

A project manifest can depend on Git-pinned modules under
`dependencies.modules`. Each dependency names a Git source and pins an immutable
ref — a `tag` (resolved to a commit) or an explicit `commit`. Floating `branch`
pins are rejected (`manifest_floating_branch`).

```yaml
dependencies:
  modules:
    - name: wl2_widgets
      git: https://example.com/wl2_widgets.git
      tag: v1.2.0
    - name: wl2_audio
      git: https://example.com/wl2_audio.git
      commit: 9f1c0c2e5b7a4d3f8e6a1b2c3d4e5f60718293a4
      path: modules/wl2_audio        # optional subdirectory
```

An optional `provides` maps a repository to the canonical module name it
provides when they differ (for example repository `wl2_widgets` providing
`wl2:widgets`).

`wl2 deps` manages these dependencies relative to the manifest:

| Command            | Effect                                                                 |
|--------------------|------------------------------------------------------------------------|
| `wl2 deps lock`    | Resolve each tag to a commit, fetch sources to discover **transitive** `sourceDependencies`, record a content checksum, and write `wl2.lock.yml` (`wl2.lock.v2`). |
| `wl2 deps fetch`   | Clone/checkout each locked module into `./.wl2/deps/<name>` at its commit. |
| `wl2 deps build`   | Configure and build each fetched module in dependency-first order under `./.wl2/build/modules/<name>`. |
| `wl2 deps install` | Install each built module library into the project-local scope (`./.wl2/modules`). |
| `wl2 deps status`  | Report each dependency's tag, locked commit, fetched, built, and installed state. |

Transitive source dependencies are declared in a module's
`wl2.module.source.yml` under `sourceDependencies.modules`; `wl2 deps lock`
fetches each source to read that metadata and adds the discovered modules to the
locked closure. Fetch/build/install never run during `wl2 run`.

`wl2 deps build` builds each fetched module out-of-tree against an installed
Winglib2 package; pass the package prefix with `--prefix <dir>` (or the
`CMAKE_PREFIX_PATH` environment variable), and optionally `--generator` and
`--build-type`. It refuses to build a checkout that does not match the locked
commit (`deps_commit_mismatch`), the locked tree checksum (`deps_tree_mismatch`),
or that has local modifications (`deps_tree_dirty`). `wl2 deps install` installs into the local scope
by default; user/system installs require explicit `wl2 module install`. Once
installed, an app that requires only the top module loads its transitive module
dependencies automatically through the resolver.

The lockfile is deterministic (modules sorted by name) so it is stable across
runs and reviewable in version control:

```yaml
schema: wl2.lock.v2
modules:
  - name: wl2_widgets
    git: https://example.com/wl2_widgets.git
    tag: v1.2.0
    commit: 1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b
    tree: 7e8f9a0b1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d
    provides: wl2:widgets
```

The `tree` field is the Git tree object id of the locked commit, captured when
`wl2 deps lock` fetches the source. It is a content checksum: `wl2 deps build`
re-checks it (and that the working tree is clean) before building, so a tampered
or locally modified checkout is rejected. Older `wl2.lock.v1` lockfiles have no
`tree` field and are still accepted; they simply skip checksum verification.

Project-local layout:

```text
.wl2/
  deps/<name>/              # fetched source checkouts
  build/modules/<name>/     # out-of-tree build trees
  modules/<name>/           # installed module libraries (local scope)
```

### Source Trust Policy

Because source dependencies fetch code from remote repositories, every Git
source is checked against a trust policy before any clone or fetch:

- Local filesystem paths and `file://` URLs are allowed (you control your own
  filesystem). Set `WL2_DEPS_DENY_LOCAL=1` to forbid them for stricter,
  reproducible CI (`deps_untrusted_local`).
- Secure transports (`https://`, `ssh://`, and scp-style `user@host:path`) are
  allowed.
- Cleartext transports (`http://`, `git://`) are rejected
  (`deps_untrusted_transport`) unless `WL2_DEPS_ALLOW_INSECURE=1`.
- `WL2_DEPS_ALLOWED_HOSTS=host1,host2` restricts remote sources to a host
  allow-list (`deps_untrusted_host`); local paths are exempt.

Integrity today rests on pinned commits, lockfile tree checksums, and installed
library checksums, which together make project-local builds reproducible and
detect tampering. Cryptographically **signed** module metadata and a full
backtracking **semver solver** are intentionally not implemented yet: the
current single-version-per-name selection and content checksums are sufficient
for first-party and pinned third-party modules. They can be added if real module
graphs ever require multi-version resolution or signed provenance.

## Installing Modules: Scopes and Resolution Order

`wl2 module install <library> --scope <scope>` installs a built module library
into one of three scopes:

| Scope    | Location                                                              |
|----------|-----------------------------------------------------------------------|
| `local`  | `./.wl2/modules`                                                       |
| `user`   | `$XDG_DATA_HOME/wl2/modules` (Linux) / `~/Library/Application Support/wl2/modules` (macOS) |
| `system` | `/usr/local/share/wl2/modules`                                         |

Each installed module gets its own directory holding the payload library and a
`wl2.module.yml` (`wl2.module.v2`) metadata file; the scope also has an
`index.yml` and a hidden `.cache` directory holding the build artifact. The
library is validated through the dynamic module ABI at install time, and its
declared dependencies (read from the validated ABI metadata) are written into
both the per-module metadata and the scope `index.yml` so a provider graph can
be built without reopening each library. The older `wl2.module.v1` files remain
readable and are treated as declaring no dependencies. Installing a module that
is already provided by a higher-priority scope prints a shadowing warning.

Install also records a `sha256` content checksum of the library in
`wl2.module.yml` and `index.yml`. Before `wl2 run` loads a selected installed
module, its library is re-hashed and compared to the recorded checksum; a stale
or tampered library is rejected with `module_checksum_mismatch` (or
`module_library_missing` when the file is gone). A required module aborts the
run; an optional one is skipped with a diagnostic. Legacy installs without a
recorded checksum are loaded without verification.

`wl2 module list --scope all` shows every installed module across scopes; omit
`--scope` for the same default, or pass `local`, `user`, or `system` to filter
one scope. `wl2 module uninstall <name>` removes one. When a name is installed
in more than one scope, uninstall requires `--scope`.

Module **resolution order** is: explicit `--load-module`, project source
dependencies, then `local`, `user`, `system`, then built-in (statically linked)
modules. A module found in an earlier source shadows the same name in a later
one. `wl2 run` resolves the modules a manifest declares in
`modules.require`/`modules.optional` — **and their transitive module
dependencies** — through the graph resolver, then loads the selected dynamic
modules in dependency-first (topological) order. An app that requires module A
therefore loads A's installed dependency B automatically. `wl2 config` lists
installed modules and marks any that are **shadowed** by a higher-priority
scope; `wl2 config --json` and `wl2 module graph` report the resolved graph and
stable dependency diagnostics.

### Uninstall Safety

- Uninstall **refuses** to remove a module referenced by the project's manifest
  or lockfile unless `--force` is given (`module_referenced`).
- Uninstall requires an explicit `--scope` when the module exists in multiple
  scopes (`module_scope_ambiguous`).
- Uninstall keeps the module's build **cache** by default so a later reinstall is
  fast; pass `--purge-cache` to remove it as well.

## Backend Support Policy

- QuickJS is the supported backend. Module bindings target QuickJS today.
- V8 is experimental. A module is not considered V8-supported until it has
  equivalent runtime/module tests and examples for the same public JavaScript
  APIs it provides under QuickJS. Until then, document QuickJS as the supported
  backend.

## New Module Checklist

1. Create `modules/wl2_<name>/` with `include/wl2_<name>/wl2_<name>.h` and
   `src/wl2_<name>.cpp` following the naming table.
2. Declare and implement `wl2_<name>_register_module`,
   `wl2_<name>_quickjs_module_factory`, and a guarded `wl2_module_get_info`.
3. Return a complete `ModuleInfo` with a fresh `stableId` UUID and an `api`
   string for `wl2 showapi`.
4. Add `wl2.module.source.yml` and `cmake/options.cmake`; declare
   `WL2_ENABLE_<NAME>` with `wl2_module_option()`.
5. Add `CMakeLists.txt` using `wl2_add_module()`; resolve module-owned
   dependencies from module-local CMake helpers.
6. Do not edit the root build or `app/wl2/`; module discovery, install/export,
   runner linkage, and static registration are registry-driven.
7. Use the standard error shape and stable, prefixed error codes.
8. Return binary data as `wl2.Buffer`; gate any host access through
   `RuntimeOptions`.
9. Add tests under `modules/wl2_<name>/test` with proper labels; use temp dirs
   and C++ fixtures, never the source tree or Python.
10. Add a standalone JS example under `examples/js/<name>/` with a
    `examples.js.<name>_standalone` test.
11. Document the module in `README.md` and link back to this file.
12. Build the default and `-DWL2_BUILD_SHARED_MODULES=ON` configurations, then run
    the suite.
