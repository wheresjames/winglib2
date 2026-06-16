# Native Debugging

This guide covers debugging the wl2 runtime and native modules with GDB, LLDB,
and VSCode. It targets the C++ side of wl2: the runner, static and dynamic native
modules, and crashes. JavaScript step debugging is a separate, deferred effort
(see [JavaScript step debugging](#javascript-step-debugging)).

## Build with debug information

Native debugging needs unstripped binaries with debug info. Configure a debug
build before launching a debugger:

```sh
cmake -S winglib2 -B winglib2/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build winglib2/build --target wl2
```

The runner is built at:

```text
winglib2/build/app/wl2/wl2
```

The bundled CMake presets also provide a `debug` configuration that writes to
`winglib2/build/debug`:

```sh
cmake --preset debug
cmake --build --preset debug --target wl2
```

When debugging a dynamic native module, build it with debug info too. The
in-tree example module target is `wl2_echo`:

```sh
cmake --build winglib2/build --target wl2_echo
```

## GDB

Launch the runner under GDB with the same arguments you would pass on the
command line. Everything after the program name is passed to `wl2`:

```sh
gdb --args winglib2/build/app/wl2/wl2 \
    run --manifest winglib2/examples/js/resources/resources.yml
```

Useful first steps inside GDB:

```text
(gdb) break wl2::Runtime::runModule
(gdb) run
(gdb) bt            # backtrace at a stop
(gdb) info threads  # list runtime and script threads
(gdb) thread 2      # switch to a script thread
(gdb) continue
```

To stop where a crash happens without setting breakpoints, just `run`; GDB stops
on the faulting signal and `bt` shows the crashing frame. The runner exports its
dynamic symbols, so frames in the runtime and in loaded modules resolve to names.

To debug a script that imports a dynamic module, point the manifest or
`--load-module` at the built module and set breakpoints in the module's
registration or native functions; GDB resolves them once the library is
`dlopen`ed at startup.

## LLDB (macOS)

LLDB takes the same arguments after `--`:

```sh
lldb -- winglib2/build/app/wl2/wl2 \
    run --manifest winglib2/examples/js/resources/resources.yml
```

Common commands:

```text
(lldb) breakpoint set --name wl2::Runtime::runModule
(lldb) run
(lldb) bt
(lldb) thread list
(lldb) thread select 2
(lldb) continue
```

On macOS, dynamic modules are `.dylib`/bundle-style shared libraries; LLDB
resolves their symbols once they are loaded at startup.

## Debugging a crash

A fatal native signal can also be diagnosed without a live debugger by using the
crash report, which records the signal and the crashing thread's C++ stack. See
[crash-reports.md](crash-reports.md). To combine the two, run under the debugger
so you stop at the fault, then read the same frames the crash report would
capture.

If you prefer post-mortem core dumps, enable them in your shell
(`ulimit -c unlimited`) and open the core with the matching binary:

```sh
gdb winglib2/build/app/wl2/wl2 core
```

## VSCode

Example `launch.json` and `tasks.json` files for native debugging live in
[debug/](debug/). Copy them into a `.vscode/` directory at the root of your
checkout:

```sh
mkdir -p .vscode
cp winglib2/docs/debug/launch.json .vscode/launch.json
cp winglib2/docs/debug/tasks.json .vscode/tasks.json
```

The launch configurations build the `wl2` target first (via the `build wl2` task)
and then start `wl2 run --manifest examples/js/resources/resources.yml` under the
platform debugger:

- "Debug wl2 run (GDB)" uses GDB and is intended for Linux.
- "Debug wl2 run (LLDB)" uses LLDB and is intended for macOS.

Adjust the `args` array to point at your own manifest or script.

## JavaScript step debugging

JavaScript step debugging is deferred until V8 inspector work is active. There is
no breakpoint-level JavaScript debugger today. Until then, diagnose JavaScript
with:

- stack traces, controlled by `--stack-traces=auto|on|off`;
- source-map remapping of stack locations (see
  [resources-javascript.md](resources-javascript.md) and the README);
- crash reports for native faults (see [crash-reports.md](crash-reports.md)); and
- `console.log` style tracing from scripts.

## Windows

Windows native debugging is intentionally kept high-level until Windows support
work is active. Once a Windows build exists, debug the runner with the platform
debugger (for example the Visual Studio debugger or `windbg`) using the same
`wl2 run` argument shape shown above. Dynamic module loading on Windows is out of
scope until then.
