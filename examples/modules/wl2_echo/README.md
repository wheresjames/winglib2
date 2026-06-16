# wl2:echo — out-of-tree module example

`wl2_echo` is a minimal, dependency-free native module. It is the reference for
building a third-party module against an installed Winglib2 package, and the
target of the `outoftree.wl2_echo` test.

It exports one JavaScript module, `wl2:echo`:

| Function      | Returns                                  |
|---------------|------------------------------------------|
| `echo(text)`  | the given string, unchanged              |
| `shout(text)` | the given string in upper case           |

`echo()`/`shout()` with a missing argument throw an `EchoError` whose `code` is
`echo_invalid_argument`.

## Layout

```
include/wl2_echo/wl2_echo.h   public header (three entry points)
src/wl2_echo.cpp             registration, QuickJS factory, C ABI get_info
wl2.module.source.yml        source marker metadata
cmake/options.cmake          module-local build options
js/main.js                  smoke script for the project-local runner
test/wl2_echo_tests.cpp      in-tree C++ test
CMakeLists.txt              static + dynamic targets, runner, and tests
```

The example follows the self-contained module layout used by built-in modules:
its enable option lives in `cmake/options.cmake`, and the source marker declares
`provides: wl2:echo`. It is still marked `NO_INSTALL` in CMake so it remains an
example, not part of the installed Winglib2 package.

## Build in-tree

Built automatically with the rest of the examples
(`-DWL2_BUILD_EXAMPLES=ON`, the default). Run its tests:

```sh
ctest --test-dir build -R examples.modules.wl2_echo --output-on-failure
```

## Build standalone (against an installed package)

```sh
cmake --install <winglib2-build> --prefix /tmp/wl2-prefix
cmake -S examples/modules/wl2_echo -B /tmp/echo-build -DCMAKE_PREFIX_PATH=/tmp/wl2-prefix
cmake --build /tmp/echo-build
ctest --test-dir /tmp/echo-build --output-on-failure
```

This is exactly what the `outoftree.wl2_echo` test automates.

The dynamic `MODULE` target (`libwl2_echo.so` / `.dylib`) loads through the wl2
dynamic module ABI on Linux and macOS:

```sh
wl2 module validate <build>/examples/modules/wl2_echo/libwl2_echo.so
wl2 run --load-module <build>/examples/modules/wl2_echo/libwl2_echo.so app.js
```

See [docs/modules.md](../../../docs/modules.md#dynamic-module-loading) for the
ABI and limitations.
