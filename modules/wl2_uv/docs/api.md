# `wl2:uv` API

```js
import { networkInterfaces, localNetworks, UvError, version } from "wl2:uv";
```

## `networkInterfaces(options?)`

Synchronously returns one record for every selected address assigned to a local
network interface. Options are:

- `family`: `"all"` (default), `"IPv4"`, or `"IPv6"`.
- `includeInternal`: include loopback/internal addresses; defaults to `false`.

Records contain `name`, `family`, `address`, `netmask`, `prefixLength`,
`internal`, and `mac`. `prefixLength` is `null` if the platform reports a
non-contiguous netmask.

## `localNetworks(options?)`

Synchronously returns sorted, unique IPv4 CIDRs derived from interface addresses.
It accepts `includeInternal` and `maximumHosts`, which defaults to 65,536. CIDRs
larger than the limit are omitted. The function currently rejects `family:
"IPv6"`; IPv6 network projection should be added with explicit host-limit
semantics rather than treating a 128-bit address space as an IPv4 host count.

## Errors

Failures throw `UvError` objects with `name`, `module`, `code`, `operation`,
`message`, `nativeCode`, and `nativeName`. Stable codes currently include
`uv_permission_denied` and `uv_interface_enumeration_failed`.

`version` is the linked libuv version string.
