# wl2:uv

`wl2:uv` exposes cross-platform system and networking utilities backed by
libuv. The initial API enumerates local network-interface addresses and derives
bounded IPv4 networks without creating a libuv event loop.

## JavaScript API

```js
import { networkInterfaces, localNetworks, version } from "wl2:uv";

const addresses = networkInterfaces({
  family: "IPv4",
  includeInternal: false,
});

const networks = localNetworks({ maximumHosts: 65_536 });

console.log(version, addresses, networks);
```

`networkInterfaces()` returns one record per interface address. Records include
the interface name, address family, address, netmask, prefix length, internal
flag, and MAC address. Use `family: "IPv4"` or `family: "IPv6"` to filter the
result; the default is `"all"`.

`localNetworks()` returns sorted, unique IPv4 CIDRs. Networks containing more
than `maximumHosts` addresses are omitted, making the result suitable for
presenting bounded network choices to discovery tools. Internal interfaces are
excluded by default from both functions.

Both calls are synchronous. Native failures throw a stable `UvError`; the
linked libuv version is exported as `version`.

## Permissions

Interface inspection reveals local topology. The current API conservatively
uses Winglib2's listen authorization for `0.0.0.0:0`, although it does not open
a socket. For command-line scripts, grant it with:

```sh
wl2 run \
  --allow-listen \
  --listen-allow=0.0.0.0:0 \
  script.js
```

A dedicated `networkInspection` capability is recommended for a future runtime
permission revision. Until then, inspection remains denied by default.

## Building

Enable the module explicitly when extended modules are disabled:

```sh
cmake -S . -B build -DWL2_ENABLE_UV=ON
cmake --build build --target wl2_uv wl2_uv_static
```

`WL2_UV_PROVIDER` accepts:

- `fetch` — download the pinned, checksum-verified libuv release.
- `package` — use a system-installed libuv development package.
- `local` — use the installation under `WL2_UV_ROOT`.
- `auto` — follow Winglib2's configured dependency-provider policy.
- `off` — disable the module.

For offline builds, either use `package` or point `WL2_UV_URL` at a previously
downloaded archive:

```sh
cmake -S . -B build \
  -DWL2_ENABLE_UV=ON \
  -DWL2_UV_PROVIDER=fetch \
  -DWL2_UV_URL=file:///absolute/path/libuv-v1.52.1.tar.gz
```

The configured SHA-256 is still verified for local archives.

## Scope

`wl2:asio` remains Winglib2's TCP socket and timer API. `wl2:uv` is intended for
portable facilities not provided by standalone Asio, such as interface
inspection and future process, filesystem-watch, or system-information APIs.
Any future asynchronous libuv handles will remain private behind Winglib APIs.

See [docs/api.md](docs/api.md), [docs/design.md](docs/design.md), and
[docs/security.md](docs/security.md).
