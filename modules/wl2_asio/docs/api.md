# wl2:asio API

All operations are promise-based. Errors reject with an `AsioError` (see
[Errors](#errors)).

## `connect(options) -> Promise<TcpSocket>`

| Option      | Type   | Default | Notes                                  |
| ----------- | ------ | ------- | -------------------------------------- |
| `host`      | string | —       | Required. Hostname or IP literal.      |
| `port`      | number | —       | Required. 1–65535.                     |
| `timeoutMs` | number | `0`     | `0` disables the connect deadline.     |

Resolves with a connected `TcpSocket`. The host is resolved first and **every**
resolved endpoint must satisfy the runtime's outbound policy; if any fails the
connect rejects with `asio_permission_denied` (the originally requested host is
kept in diagnostics). Other rejections: `asio_invalid_argument` for bad options,
`asio_resolve_failed` / `asio_connect_failed` on network failure, or
`asio_timeout` when `timeoutMs` elapses.

## `listen(options) -> Promise<TcpServer>`

| Option | Type   | Default       | Notes                                   |
| ------ | ------ | ------------- | --------------------------------------- |
| `host` | string | `"127.0.0.1"` | Bind address (IP literal or hostname).  |
| `port` | number | `0`           | `0` selects an ephemeral port.          |

Binds and listens on the address. Rejects with `asio_permission_denied` when the
runtime has not granted listening (`allowListening` + `listenAllowList`),
`asio_invalid_argument` for a bad port, or `asio_listen_failed` if the bind
fails (e.g. the port is in use).

### `TcpServer`

- `accept(options) -> Promise<TcpSocket>` — accepts one connection. `options`
  takes `timeoutMs` (`0` disables the deadline). One accept at a time; overlap
  rejects with `asio_invalid_argument`; a deadline rejects with `asio_timeout`.
- `address() -> { host, port }` — the bound local endpoint (resolves the
  ephemeral port chosen when `port: 0`).
- `close()` — idempotent; cancels a pending accept with `asio_closed`.

## `TcpSocket`

### `read(options) -> Promise<wl2.Buffer>`

| Option      | Type   | Default | Notes                                       |
| ----------- | ------ | ------- | ------------------------------------------- |
| `maxBytes`  | number | 65536   | Capped at 1 MiB. One read at a time.        |
| `timeoutMs` | number | `0`     | `0` disables the read deadline.             |

Resolves with a `wl2.Buffer` of up to `maxBytes` bytes. A **zero-length** buffer
signals EOF (the peer closed its side). Rejects with `asio_invalid_argument` if a
read is already in flight, `asio_timeout`, `asio_closed`, or `asio_read_failed`.

### `write(data, options) -> Promise<{ bytesWritten }>`

`data` may be a string (sent as UTF-8), an `ArrayBuffer`, or a `TypedArray`.
Writes the whole payload, then resolves with `{ bytesWritten }`. One write at a
time; overlapping writes reject with `asio_invalid_argument`.

| Option      | Type   | Default | Notes                            |
| ----------- | ------ | ------- | -------------------------------- |
| `timeoutMs` | number | `0`     | `0` disables the write deadline. |

### `remoteAddress() -> { host, port }` / `localAddress() -> { host, port }`

Synchronous queries of the connected peer / local endpoint. Throw an `AsioError`
(`asio_closed`) if the socket is not connected.

### `close()`

Idempotent. Closes the socket and settles any in-flight read/write with
`asio_closed`. A QuickJS finalizer closes the socket as a backstop.

## Errors

Rejections are `AsioError` objects:

```
{ name: "AsioError", module: "wl2_asio", code, operation, message,
  host?, port?, cause? }
```

Stable `code` values: `asio_permission_denied`, `asio_invalid_argument`,
`asio_resolve_failed`, `asio_connect_failed`, `asio_listen_failed`,
`asio_accept_failed`, `asio_read_failed`, `asio_write_failed`, `asio_timeout`,
`asio_closed`.
