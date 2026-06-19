# wl2:asio — Asynchronous TCP networking

`wl2:asio` is a built-in module providing promise-based TCP networking to
JavaScript, backed by header-only standalone [Asio](https://think-async.com/Asio/).
It is the lower-level networking foundation; `wl2:curl` remains the higher-level
HTTP client.

The module is gated behind `WL2_ENABLE_ASIO` (default **OFF**) and links into the
`wl2` runner statically when enabled. Full documentation lives with the module:

- [API reference](../modules/wl2_asio/docs/api.md) — `connect`, `listen`, and the
  `TcpSocket` / `TcpServer` surface.
- [Security model](../modules/wl2_asio/docs/security.md) — default-denied policy,
  allow-list grammar, and DNS behavior.
- [Design](../modules/wl2_asio/docs/design.md) — worker thread, promise
  settlement, ownership, and shutdown.
- [README](../modules/wl2_asio/README.md) and the
  [`examples/tcp-echo`](../modules/wl2_asio/examples/tcp-echo) single-executable
  example.

## Quick start

```sh
cmake -S . -B build -DWL2_ENABLE_ASIO=ON
cmake --build build
build/app/wl2/wl2 run --network-allow 127.0.0.1:7000 app.js
```

```js
import { connect, listen } from "wl2:asio";

// client
const socket = await connect({ host: "127.0.0.1", port: 7000, timeoutMs: 1000 });
await socket.write("ping");
console.log((await socket.read({ maxBytes: 4096 })).text());
socket.close();

// server
const server = await listen({ host: "127.0.0.1", port: 0 });
const client = await server.accept({ timeoutMs: 1000 });
```

Network access is **denied by default**; the runtime must grant it (CLI flags
`--allow-network` / `--network-allow` / `--allow-listen` / `--listen-allow`, or
the equivalent `RuntimeOptions` fields). See the security doc for details.

## Dependency provider

Standalone Asio is discovered through `WL2_DEPS_ASIO`
(`auto|local|system|download|off`); see [dependencies.md](dependencies.md).
