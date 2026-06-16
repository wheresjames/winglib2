# Winglib2 Dependencies

Winglib2 prefers local, target-specific dependency installs over host-global
libraries. This keeps desktop, release, debug, and cross-compiled dependency
sets from colliding.

The default dependency root is:

```text
winglib2/.deps/<system>-<arch>/<build-type>
```

Examples:

```text
winglib2/.deps/linux-x86_64/debug/quickjs
winglib2/.deps/linux-x86_64/release/v8
winglib2/.deps/linux-arm-linux-gnueabihf/release/quickjs
```

CMake reports the selected path during configure:

```text
Winglib2 dependency target path: linux-x86_64/debug
Winglib2 dependency root: .../winglib2/.deps/linux-x86_64/debug
```

## Provider Model

Dependencies are split by ownership:

- **Core dependencies** are linked into `wl2_core` and remain in the global
  dependency layer: JavaScript engines and libmembus integration.
- **Module dependencies** are used by one module and live in that module's
  directory: options in `modules/<module>/cmake/options.cmake`, helper CMake next
  to it, and package contributions registered by the module.

Provider-style dependencies use this shape:

```cmake
WL2_<DEP>_PROVIDER=auto|local|package|fetch|off
```

Core providers:

- `WL2_QUICKJS_PROVIDER=auto|local|package|fetch|off`
- `WL2_LIBMEMBUS_PROVIDER=auto|local|package|fetch|off`
- `WL2_V8_PROVIDER=auto|local|package|off`

Module providers are documented by their owning module options file. For
example, the curl module declares `WL2_CURL_PROVIDER`, `WL2_CURL_VERSION`,
`WL2_CURL_URL`, `WL2_CURL_URL_HASH`, `WL2_CURL_FETCH_CMAKE_ARGS`, and
`WL2_CURL_ROOT` in `modules/wl2_curl/cmake/options.cmake`.

`auto` checks local target-specific installs first, then fetches missing
dependencies when `WL2_FETCH_DEPS=ON` and the dependency supports fetching.
Native builds may fall back to package/system dependencies with a warning.
Cross builds should use `local`, `fetch`, or explicit `package` settings so
host libraries are not accidentally linked into target binaries.

## JavaScript Engines

QuickJS is the intended default engine for embedded and cross builds:

```sh
cmake -S winglib2 -B build-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux.cmake \
  -DWL2_JS_ENGINE=quickjs \
  -DWL2_QUICKJS_PROVIDER=fetch
```

With the default `WL2_FETCH_DEPS=ON`, a normal desktop configure can fetch
QuickJS automatically:

```sh
cmake -S winglib2 -B build -G Ninja
cmake --build build
```

The build stages the fetched static library into:

```text
${WL2_DEPS_ROOT}/quickjs
```

V8 remains a supported backend, but it is better suited to desktop/server
targets unless there is a dedicated V8 cross-build workflow for the target.

## libmembus

Winglib2 targets libmembus `v1.2.0` for the membus wrapper/API update.
libmembus is important for compatibility with the existing Winglib ecosystem,
so it follows the embedded-friendly dependency policy: local source builds are
the default path when practical, while `find_package(libmembus)` remains
available for installed/package workflows.

Useful options:

```sh
-DWL2_LIBMEMBUS_PROVIDER=auto    # default: local v1.2 source, fetch target source, then package
-DWL2_LIBMEMBUS_PROVIDER=local   # require WL2_LIBMEMBUS_ROOT source tree
-DWL2_LIBMEMBUS_PROVIDER=fetch   # fetch libmembus v1.2.0 into WL2_DEPS_ROOT
-DWL2_LIBMEMBUS_PROVIDER=package # require find_package(libmembus)
-DWL2_LIBMEMBUS_PROVIDER=off     # disable libmembus wrappers
-DWL2_LIBMEMBUS_ROOT=.deps/.../libmembus
```

When `auto` sees a local source tree that is older than the target v1.2 surface
and `WL2_FETCH_DEPS=ON`, it stages libmembus v1.2.0 under `WL2_DEPS_ROOT` and
uses that target source instead. Cross builds should use `local`, `fetch`, or
an explicit package root that was built for the target.

## Module Dependencies

Module dependency helpers should call `wl2_declare_dependency(<DEP> ...)` from
`cmake/deps/WL2Dependency.cmake`. The helper declares standard cache variables,
normalizes the provider, applies a target-local default root, delegates actual
resolution to the module callback, and returns:

```cmake
WL2_<DEP>_FOUND
WL2_<DEP>_TARGET
WL2_<DEP>_IS_SYSTEM
WL2_<DEP>_PROVIDER_USED
```

If a locally built dependency must be shipped with the installed Winglib2
package, the module registers that explicitly:

```cmake
wl2_module_bundle_dependency(EXAMPLE
    TARGET ${WL2_EXAMPLE_TARGET}
    INCLUDE_DIR "${WL2_EXAMPLE_ROOT}/include"
    COMPILE_DEFINITIONS EXAMPLE_STATIC)

wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Example.cmake"
    DESTINATION modules/wl2_example)

wl2_module_package_config_fragment(CONTENT
    "if(NOT TARGET Example::example)\n    find_dependency(Example)\nendif()\n")
```

The curl module uses this provider policy:

```sh
-DWL2_CURL_PROVIDER=auto    # default: local root, fetch when WL2_FETCH_DEPS=ON, then warned package fallback for native builds
-DWL2_CURL_PROVIDER=local   # require WL2_CURL_ROOT install tree
-DWL2_CURL_PROVIDER=fetch   # fetch curl source and stage a static libcurl under WL2_CURL_ROOT
-DWL2_CURL_PROVIDER=package # require find_package(CURL)
-DWL2_CURL_PROVIDER=off     # disable curl discovery/module
-DWL2_CURL_ROOT=.deps/.../curl
-DWL2_CURL_FETCH_CMAKE_ARGS="..." # optional extra arguments for the fetched curl build
-DWL2_CURL_URL_HASH=SHA256=...    # expected source archive hash
```

`auto` refuses implicit package/system fallback while cross-compiling. Use
`WL2_CURL_PROVIDER=package` only when the active toolchain/package path resolves
to target-compatible curl libraries.

The asio module (`wl2:asio`, gated behind `WL2_ENABLE_ASIO`, default OFF) uses
the same provider policy for header-only standalone Asio:

```sh
-DWL2_ASIO_PROVIDER=auto    # default: local root, fetch when WL2_FETCH_DEPS=ON, then system headers
-DWL2_ASIO_PROVIDER=local   # require asio.hpp under WL2_ASIO_ROOT
-DWL2_ASIO_PROVIDER=fetch   # download a pinned standalone Asio release into WL2_ASIO_ROOT
-DWL2_ASIO_PROVIDER=package # require system-installed standalone asio.hpp
-DWL2_ASIO_PROVIDER=off     # disable asio discovery/module
-DWL2_ASIO_ROOT=.deps/.../asio
-DWL2_ASIO_VERSION=1.30.2         # standalone Asio release for the fetch provider
-DWL2_ASIO_URL_HASH=SHA256=...    # expected source archive hash
```

Standalone Asio is header-only, so the fetch provider only downloads and extracts
headers (no build step). Boost.Asio is intentionally not a provider.

## V8 Local Install

The V8 installer stages files into the same target-specific layout by default:

```sh
./tools/install-v8.sh --build-type release
```

Then configure with:

```sh
cmake -S winglib2 -B build-v8 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWL2_JS_ENGINE=v8
```

Override with `WL2_V8_ROOT` when using a custom install prefix.
