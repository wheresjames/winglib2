# Membus C++ Integration {#membus_cpp}

The C++ libmembus wrapper layer covers the libmembus `v1.2.0` surface while
keeping older local checkouts buildable.

## Implemented Wrapper Surface

The C++ API now includes wrappers for:

- `wl2::SharedBuffer` over `mmb::memmap`
- `wl2::SharedQueue` over `mmb::memmsg`
- `wl2::VideoBuffer` over `mmb::memvid`
- `wl2::AudioBuffer` over `mmb::memaud`
- `wl2::CommandChannel` over `mmb::memcmd`
- `wl2::KeyValueStore` over `mmb::memkv`
- `wl2::MembusSelector` over `mmb::select`

`wl2::libmembusHasV12Surface()` reports whether the selected dependency exposes
the full v1.2 surface. v1.2-only wrappers return clear errors when Winglib2 is
configured against an older forced local source tree.

## Build Policy

The `auto` provider now targets libmembus v1.2.0 by default. If the configured
local source tree is older and dependency fetching is enabled, CMake stages the
v1.2.0 source under `WL2_DEPS_ROOT` and builds it locally for the target.

```sh
cmake -S . -B build-v12 -DWL2_LIBMEMBUS_PROVIDER=fetch
cmake --build build-v12
ctest --test-dir build-v12 --output-on-failure -L membus
```

## KeyValueStore Alignment Guard

libmembus `memkv` stores slot sequence fields as `int64_t` values. The wrapper
validates name/value limits before creating a store so the slot stride remains
8-byte aligned. For example, `15/31` is safe because the stored strings include
trailing null bytes, while `16/32` is rejected by the wrapper.

## Tests

The membus suite has individual CTest entries for each wrapper:

- `core.membus.shared_buffer`
- `core.membus.shared_queue`
- `core.membus.video_buffer`
- `core.membus.audio_buffer`
- `core.membus.command_channel`
- `core.membus.key_value_store`
- `core.membus.selector`

The aggregate `core.membus` test still runs all membus cases.
