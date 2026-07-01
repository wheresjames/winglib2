# Runtime Foundations {#foundations}

This page records the shared decisions behind the membus, resources, and
thread-tree work: API direction, build policy, test labels, and documentation
structure. The behavior itself is documented on the membus, resources, and
thread-tree pages.

## Doxygen Groups

The foundational runtime work is organized around these groups:

@defgroup wl2_membus Membus
Shared-memory IPC wrappers and JavaScript bindings for libmembus.

@defgroup wl2_resources Resources
Embedded resource manifests, compressed/uncompressed storage, and read-only
resource access.

@defgroup wl2_threading Thread Tree
Script-thread launch, tree-addressed messaging, and request/reply coordination.

## libmembus Target

Winglib2 targets the upstream libmembus `v2.1.0` API surface. That surface
includes:

- `memmap`
- `memmsg`
- `memvid`
- `memaud`
- `mempkt`
- `memcmd`
- `memkv`
- `select`

The C++ wrappers cover every type listed above. See @ref membus_cpp for the
wrapper details.

## Dependency Policy

Cross-builds, especially embedded targets, are a first-class requirement.
Winglib2 should build required core dependencies locally from source by default
when that is reasonable. This avoids accidental host-library linkage and keeps
target dependency trees reproducible.

The libmembus provider model is:

```cmake
WL2_DEPS_LIBMEMBUS=inherit|auto|local|system|download|off
WL2_LIBMEMBUS_ROOT=.deps/.../libmembus
WL2_LIBMEMBUS_TARGET_VERSION=2.1.0
```

`auto` prefers local source from `WL2_LIBMEMBUS_ROOT` when it exposes the target
v2.1 surface, downloads the target source when the local checkout is missing or
older, then uses installed packages only when native-build fallback is
acceptable. `local` requires local source. `system` requires an installed
`find_package(libmembus)` configuration. `download` stages the target source
under `WL2_DEPS_ROOT`.

## JavaScript Namespace Defaults

The namespace decisions are:

- `wl2:membus` for the importable membus module.
- `wl2.resources` for core embedded-resource APIs.
- `wl2.thread` for core thread-tree APIs.

## Test Labels

CTest labels for this work are:

- `unit`
- `js`
- `integration`
- `membus`
- `resources`
- `thread`
- `stress`
- `examples`

Fast deterministic C++ and JavaScript tests should run by default. Stress,
hardware, long-running, or external-environment tests should be opt-in via
labels. `WL2_ENABLE_STRESS_TESTS=OFF` is the default configure policy; enabling
it registers stress-labeled CTest entries without mixing them into the normal
fast gate.

The cross-cutting gate includes:

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -L examples
```

The combined foundation example at `examples/foundations` embeds `main.js`,
`worker.js`, and `config.json`, stores and compresses resources, spawns a
worker through `wl2.thread`, exchanges a bounded request/reply, and uses
`wl2:membus` as the shared-memory handoff.

## Foundation Checklist

- libmembus target version is named in CMake.
- libmembus provider options exist.
- Membus, resource, and thread tests are labeled.
- JavaScript tests are labeled.
- Example tests are labeled.
- Stress tests are opt-in through `WL2_ENABLE_STRESS_TESTS`.
- Doxygen includes the docs directory.
- Doxygen groups exist for membus, resources, and thread tree.
- The namespace and dependency decisions above are settled and shared across the
  membus, resources, and thread-tree work.
