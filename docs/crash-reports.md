# Crash Reports

When a wl2 run hits a fatal signal (`SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`, or
`SIGBUS`), the runtime can write a crash report describing what was running and
where the crashing thread was. The report is meant to make a native crash
actionable without a debugger attached, and to be easy for CI to collect.

## Enabling and choosing a destination

Crash reporting is controlled by two `wl2 run` options:

- `--crash-report=off|auto|<path>`
  - `auto` (the default) writes `crash-YYYYMMDD-HHMMSS-PID.log` into the report
    directory.
  - `off` installs no handler and writes nothing.
  - `<path>` writes the report to that exact file.
- `--crash-report-dir <dir>` sets the directory used for `auto` names. It
  defaults to the current directory. It has no effect when an explicit
  `--crash-report=<path>` is given.

```sh
# Default: write crash-*.log into the working directory on a fatal signal.
wl2 run --manifest wl2.yml

# Collect reports under a known directory in CI.
wl2 run --manifest wl2.yml --crash-report-dir ./artifacts

# Write to an explicit path.
wl2 run --manifest wl2.yml --crash-report=./crash.log

# Disable crash reporting entirely.
wl2 run --manifest wl2.yml --crash-report=off
```

## What a report contains

Each report has a human-readable section followed by a machine-readable JSON
trailer after a `--- json ---` marker:

- the signal name and number;
- a timestamp and process id;
- the executable path, working directory, and full argument vector;
- the JavaScript engine name;
- the manifest path, when a manifest drove the run;
- resource directory maps as `host -> logical` pairs;
- the module list;
- the registered wl2-managed threads; and
- the crashing thread's C++ stack.

The JSON trailer carries the same fields as a single object, including a
`stackFrameCount`, so tooling can parse it without scraping the text section.

For readable frame names in the stack section, build with exported dynamic
symbols. The `wl2` runner already exports its symbols; standalone executables
built by `wl2 app install` do as well.

## Threads

Every wl2-managed script thread registers itself while it runs, so the report
lists the host thread (`/main`) together with any active script threads. The
current report walks only the crashing thread's stack; capturing every thread's
stack is intentionally left for later work, which is why threads are registered
now.

## Limitations

- Windows crash handling is deferred to a later plan; on Windows the options are
  accepted but no handler is installed.
- Only the crashing thread's stack is captured today.
- Frame symbolization depends on the executable exporting its dynamic symbols
  and on available debug information; stripped binaries fall back to addresses.
- The report is written from inside a signal handler. It uses pre-rendered text
  and async-signal-safe writes, but a crash that has already corrupted the C
  runtime may still prevent a complete report.
