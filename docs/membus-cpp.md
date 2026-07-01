# Membus C++ Integration {#membus_cpp}

The C++ libmembus wrapper layer targets libmembus `v2.1.0`, including the
`mempkt` compressed-packet ring, while keeping older forced local checkouts
buildable with reduced surface detection.

## Implemented Wrapper Surface

The C++ API now includes wrappers for:

- `wl2::SharedBuffer` over `mmb::memmap`
- `wl2::SharedQueue` over `mmb::memmsg`
- `wl2::VideoBuffer` over `mmb::memvid`
- `wl2::AudioBuffer` over `mmb::memaud`
- `wl2::PacketBuffer` over `mmb::mempkt`
- `wl2::CommandChannel` over `mmb::memcmd`
- `wl2::KeyValueStore` over `mmb::memkv`
- `wl2::MembusSelector` over `mmb::select`

`wl2::libmembusHasV12Surface()` reports whether the selected dependency exposes
the full v1.2 surface. v1.2-only wrappers return clear errors when Winglib2 is
configured against an older forced local source tree.

`wl2::libmembusHasV21Surface()` reports whether the selected dependency exposes
the v2.1 `mempkt` surface. `PacketBuffer` returns a clear unavailable/surface
error when that support is not compiled in.

## Build Policy

The `auto` dependency mode targets libmembus v2.1.0 by default. If the
configured local source tree is older, CMake stages the v2.1.0 source under
`WL2_DEPS_ROOT` and builds it locally for the target.

```sh
cmake -S . -B build-membus -DWL2_DEPS_LIBMEMBUS=download
cmake --build build-membus
ctest --test-dir build-membus --output-on-failure -L membus
```

## PacketBuffer Records

`wl2::PacketBuffer` is the wrapper for libmembus `mempkt`. It stores
variable-length compressed or packetized payloads in a shared-memory arena with
a descriptor ring for sequence, PTS, kind (`data`, `video`, `audio`), track, and
per-record metadata. It is intended for compressed media access units, muxed
packet streams, and live transcode handoffs that do not fit fixed-size
`memvid`/`memaud` rings.

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
- `core.membus.packet_buffer`
- `core.membus.command_channel`
- `core.membus.key_value_store`
- `core.membus.selector`

The aggregate `core.membus` test still runs all membus cases.
