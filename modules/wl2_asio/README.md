# wl2_asio

Asynchronous TCP networking for Winglib2, exported to JavaScript as `wl2:asio`
and backed by standalone [Asio](https://think-async.com/Asio/). It provides a
promise-based TCP **client** (`connect`) and **server** (`listen` / `accept`).

```js
import { connect } from "wl2:asio";

const socket = await connect({ host: "127.0.0.1", port: 7000, timeoutMs: 1000 });
await socket.write("ping");
const reply = await socket.read({ maxBytes: 4096, timeoutMs: 1000 });
console.log(reply.text());
socket.close();
```

## Security

Network access is **denied by default**. The host runtime must grant it before
`connect()` can succeed. From the CLI:

```sh
wl2 run --network-allow 127.0.0.1:7000 app.js
```

See [`docs/security.md`](docs/security.md) for the allow-list grammar and
[`docs/api.md`](docs/api.md) for the full API. The worker-thread / promise model
is described in [`docs/design.md`](docs/design.md).

## Building

The module is gated behind `WL2_ENABLE_ASIO` (default `OFF`). When enabled it
discovers or fetches standalone Asio via `cmake/WL2Asio.cmake`
(`WL2_ASIO_PROVIDER=auto|local|package|fetch|off`):

```sh
cmake -S . -B build -DWL2_ENABLE_ASIO=ON
cmake --build build
ctest --test-dir build -L asio --output-on-failure
```
