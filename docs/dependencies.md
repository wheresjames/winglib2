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

For a fully target-local dependency build that avoids package/system
dependencies, configure with:

```sh
cmake -S . -B build-local-deps \
  -DWL2_DEPS=download
```

Set `WL2_DEPS_ROOT` explicitly to override that location.

## Provider Model

Dependencies are split by ownership:

- **Core dependencies** are linked into `wl2_core` and remain in the global
  dependency layer: JavaScript engines and libmembus integration.
- **Module dependencies** are used by one module and live in that module's
  directory: options in `modules/<module>/cmake/options.cmake`, helper CMake next
  to it, and package contributions registered by the module.

Provider-style dependencies use one global default and per-dependency overrides:

```cmake
WL2_DEPS=auto|local|system|download|off
WL2_DEPS_<DEP>=inherit|auto|local|system|download|off
```

Core dependency names:

- `WL2_DEPS_QUICKJS`
- `WL2_DEPS_LIBMEMBUS`

Module dependency names are documented by their owning module options file. The
current names include `WL2_DEPS_CURL`, `WL2_DEPS_ASIO`, `WL2_DEPS_SLINT`,
`WL2_DEPS_SLINT_NFD`, and `WL2_DEPS_MAGNUM`.

`auto` checks local target-specific installs first, then downloads pinned
dependencies when supported, then falls back to package/system dependencies with
a warning on native builds. Cross builds refuse implicit package/system fallback;
use `system` only when the active toolchain/package path resolves target
libraries.

Explicit values are strict:

- `local`: require the target-local root.
- `system`: require an installed/CMake-found package.
- `download`: fetch the pinned dependency into `WL2_DEPS_ROOT` and use it.
- `off`: disable the dependency and any feature that requires it.

The old `WL2_FETCH_DEPS`, `WL2_USE_FETCHED_DEPS`, and
`WL2_<DEP>_PROVIDER=package|fetch` spellings are compatibility inputs. New
configurations should use `system` and `download`.

## JavaScript Engines

QuickJS is the intended default engine for embedded and cross builds:

```sh
cmake -S winglib2 -B build-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux.cmake \
  -DWL2_JS_ENGINE=quickjs \
  -DWL2_DEPS_QUICKJS=download
```

With the default `WL2_DEPS=auto`, a normal desktop configure can download
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
-DWL2_DEPS_LIBMEMBUS=auto     # local v1.2 source, download target source, then system
-DWL2_DEPS_LIBMEMBUS=local    # require WL2_LIBMEMBUS_ROOT source tree
-DWL2_DEPS_LIBMEMBUS=download # download libmembus v1.2.0 into WL2_DEPS_ROOT
-DWL2_DEPS_LIBMEMBUS=system   # require find_package(libmembus)
-DWL2_DEPS_LIBMEMBUS=off      # disable libmembus wrappers
-DWL2_LIBMEMBUS_ROOT=.deps/.../libmembus
```

When `auto` sees a local source tree that is older than the target v1.2 surface
it stages libmembus v1.2.0 under `WL2_DEPS_ROOT` and uses that target source
instead. Cross builds should use `local`, `download`, or
an explicit package root that was built for the target.

## Module Dependencies

Module dependency helpers should call `wl2_declare_dependency(<DEP> ...)` from
`cmake/deps/WL2Dependency.cmake`. The helper declares standard cache variables,
normalizes the dependency mode, applies a target-local default root, delegates actual
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
-DWL2_DEPS_CURL=auto     # local root, download, then warned system fallback for native builds
-DWL2_DEPS_CURL=local    # require WL2_CURL_ROOT install tree
-DWL2_DEPS_CURL=download # download curl source and stage a static libcurl under WL2_CURL_ROOT
-DWL2_DEPS_CURL=system   # require find_package(CURL)
-DWL2_DEPS_CURL=off      # disable curl discovery/module
-DWL2_CURL_ROOT=.deps/.../curl
-DWL2_CURL_FETCH_CMAKE_ARGS="..." # optional extra arguments for the fetched curl build
-DWL2_CURL_URL_HASH=SHA256=...    # expected source archive hash
```

`auto` refuses implicit package/system fallback while cross-compiling. Use
`WL2_DEPS_CURL=system` only when the active toolchain/package path resolves
to target-compatible curl libraries.

The asio module (`wl2:asio`, gated behind `WL2_ENABLE_ASIO`, default OFF) uses
the same provider policy for header-only standalone Asio:

```sh
-DWL2_DEPS_ASIO=auto     # local root, download, then system headers
-DWL2_DEPS_ASIO=local    # require asio.hpp under WL2_ASIO_ROOT
-DWL2_DEPS_ASIO=download # download a pinned standalone Asio release into WL2_ASIO_ROOT
-DWL2_DEPS_ASIO=system   # require system-installed standalone asio.hpp
-DWL2_DEPS_ASIO=off      # disable asio discovery/module
-DWL2_ASIO_ROOT=.deps/.../asio
-DWL2_ASIO_VERSION=1.30.2         # standalone Asio release for the fetch provider
-DWL2_ASIO_URL_HASH=SHA256=...    # expected source archive hash
```

Standalone Asio is header-only, so the fetch provider only downloads and extracts
headers (no build step). Boost.Asio is intentionally not a provider.

The slint module (`wl2:slint`, gated behind `WL2_ENABLE_SLINT`, default OFF) wraps
the [Slint](https://slint.dev) UI toolkit. Slint is a Rust library, so the
provider model deliberately **prefers Slint's prebuilt C++ binary package**
(consumed via `find_package(Slint)`), which needs **no Rust toolchain**:

```sh
-DWL2_DEPS_SLINT=auto     # local root, download prebuilt package, then system package
-DWL2_DEPS_SLINT=local    # require an installed Slint package under WL2_SLINT_ROOT
-DWL2_DEPS_SLINT=download # download a pinned prebuilt C++ binary package into WL2_SLINT_ROOT
-DWL2_DEPS_SLINT=system   # require find_package(Slint)
-DWL2_DEPS_SLINT=off      # disable slint discovery/module
-DWL2_SLINT_FROM_SOURCE=ON # with WL2_DEPS_SLINT=download, build from source via FetchContent
-DWL2_SLINT_ROOT=.deps/.../slint
-DWL2_SLINT_VERSION=1.8.0          # Slint release for the fetch/source providers
-DWL2_SLINT_URL=<platform package> # prebuilt C++ binary package archive URL
-DWL2_SLINT_URL_HASH=SHA256=...    # expected package archive hash
```

Only the opt-in `source` provider needs `cargo`/Rust; every other provider
consumes the prebuilt package, so default builds and CI stay free of a Rust
toolchain. The default `WL2_SLINT_URL` targets the Linux x86_64 package; override
`WL2_SLINT_URL` and `WL2_SLINT_URL_HASH` together for other platforms.

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
