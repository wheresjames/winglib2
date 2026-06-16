# wl2:asio security model

Networking is **denied by default**. `wl2:asio` never implements policy itself;
every `connect()` calls `Runtime::authorizeNetworkConnect(host, port)`, the same
gate embedders and the CLI configure. A denial becomes an `AsioError` with code
`asio_permission_denied`, carrying the runtime's reason as `cause`.

## Granting access

From the CLI (`wl2 run`):

| Flag                          | Effect                                          |
| ----------------------------- | ----------------------------------------------- |
| `--allow-network`             | Enable outbound connects (subject to the list). |
| `--network-allow host[:port]` | Add an allow-list entry (implies the above).    |
| `--allow-listen`              | Enable listeners (used by the Phase 2 server).  |
| `--listen-allow host[:port]`  | Add a listen allow-list entry.                  |

Embedders set the equivalent `RuntimeOptions` fields (`allowNetwork`,
`networkAllowList`, `allowListening`, `listenAllowList`) directly.

## Allow-list grammar

Entries are matched literally after host normalization:

- `*` — any host, any port.
- `host` — that host, any port (e.g. `127.0.0.1`).
- `host:port` — exact host and port (e.g. `127.0.0.1:7000`).
- `*:port` — any host on that port; `host:*` — that host, any port.
- An **empty** allow-list denies access even when `--allow-network` is set.

CIDR ranges and hostname wildcards beyond `*` are not supported.

## DNS

`connect()` resolves the host first, then requires **every** resolved endpoint to
satisfy `authorizeNetworkConnect`; it denies if any address fails (no partial
allow). The originally requested host is preserved in `asio_permission_denied`
diagnostics. IP literals resolve to themselves without a DNS query.
