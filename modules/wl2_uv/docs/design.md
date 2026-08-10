# Design

The initial module deliberately wraps capabilities rather than exposing libuv C
handles. `uv_interface_addresses()` is synchronous and does not require a
`uv_loop_t`, so this version has no worker thread or shutdown lifecycle.

The native result is owned by an RAII guard and always released through
`uv_free_interface_addresses()`. Address formatting uses libuv's portable IPv4
and IPv6 helpers. CIDR derivation validates that netmasks are contiguous,
normalizes the network address, removes duplicates, and applies a caller-visible
host bound.

Future asynchronous libuv features should introduce one module-owned loop and
worker thread, marshal completions through `Runtime::async()`, and register a
shutdown hook. They should continue exposing Winglib concepts rather than raw
`uv_handle_t` pointers.

`wl2:asio` remains the TCP API. `wl2:uv` should initially grow into facilities
that standalone Asio does not provide, such as interface inspection, process
management, filesystem watching, and selected system information. Duplicating
TCP APIs would create two incompatible networking surfaces.
