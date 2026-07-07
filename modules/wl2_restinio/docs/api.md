# wl2:http

`wl2:http` is an embeddable HTTP/1.1 server built on
[RESTinio](https://github.com/Stiffstream/restinio) and standalone Asio. Each
server is an independent object with no process-global state: multiple servers
coexist and start/stop cleanly. Requests are dispatched to JavaScript handlers
that return a response object (or a Promise of one).

The backing module directory is `modules/wl2_restinio`; the design rationale and
roadmap are in `WL2-RESTINIO.md` at the repository root. This document
describes the implemented HTTP core.

## Threading model

RESTinio runs on a module-owned `io_context` driven by one worker thread. A
request is copied into plain data on that io thread and marshalled onto the
JavaScript thread through the runtime's async host, where the handler runs; the
handler's result is turned into a response and sent back on the io thread. The
JavaScript engine stays single-threaded throughout.

> Because handlers run on the JS thread, an **in-process** HTTP client that
> blocks that thread (such as `wl2:curl`) cannot call a `wl2:http` server in the
> same runtime — the server needs the thread the client is blocking. Use an
> out-of-process client, as the module's own test fixture does.

## JavaScript API

```js
import { HttpServer } from "wl2:http";

const server = new HttpServer({ host: "127.0.0.1", port: 8080, maxBodyBytes: 1 << 20 });

server.route("GET", "/users/:id", (req) => ({
  status: 200,
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ id: req.params.id, q: req.query, sid: req.cookies.sid }),
}));

server.route("POST", "/upload", (req) => `${req.files.length} file(s)`);

server.static("/assets", "/srv/www/assets", {
  cacheControl: "no-store",
  mimeTypes: { ".m3u8": "application/vnd.apple.mpegurl" },
});   // sandboxed to the root; filesystem-gated

server.ws("/socket", {
  onOpen: (conn) => conn.send("welcome"),
  onMessage: (conn, msg) => conn.send(msg.text()),
  onClose: (conn, code, reason) => {},
  maxMessageBytes: 1 << 20,
  maxBufferedBytes: 4 << 20,
});

const { host, port } = await server.listen();   // gated by the listen permission
// ... serve ...
await server.close();
```

For HTTPS, pass an `https` option with cert/key file paths:

```js
const secure = new HttpServer({ host: "127.0.0.1", port: 8443, https: { cert: "/etc/cert.pem", key: "/etc/key.pem" } });
```

### `new HttpServer(options?)`

| Option | Type | Default | Notes |
|--------|------|---------|-------|
| `host` | string | `"127.0.0.1"` | Bind address. |
| `port` | number | `0` | Bind port. |
| `maxBodyBytes` | number | `1048576` | Requests with a larger body get a `413` without invoking the handler. |
| `https` | object | — | `{ cert, key, keyPassword? }` file paths. Enables TLS (requires a TLS build). |

### `server.route(method, path, handler)`

Registers a route; returns the server (chainable). Must be called before
`listen()`. `method` is a string (`GET`, `POST`, `PUT`, `DELETE`, `PATCH`,
`HEAD`, `OPTIONS`). `path` is an express-style pattern supporting `:params` and
`*` wildcards.

`handler(req)` returns a response, or a Promise of one. A thrown error or
rejected Promise becomes a `500`.

- **`req`**: `{ method, url, path, query, params, headers, cookies, body, files,
  remoteAddr }`. `params` is an object of matched path parameters; `headers` and
  `cookies` are objects; `body` is a `wl2.Buffer` (use `req.body.text()`);
  `query` is the raw query string; `files` is an array of parsed
  `multipart/form-data` parts `{ name, filename, contentType, data }` (`data` is
  a `wl2.Buffer`).
- **response**: `{ status?, headers?, body? }`, or a plain string (sent as
  `text/plain`). `body` may be a string or an ArrayBuffer/TypedArray. A
  `content-type` header is added automatically when absent. Text-ish responses
  are gzip-compressed when the client sends `Accept-Encoding: gzip`.

### `server.routeStream(method, path, handler)`

Registers a streaming route; returns the server (chainable). Must be called
before `listen()`. Instead of returning a body, `handler(req, stream)` drives a
long-lived chunked response through the `stream` object — suitable for
`multipart/x-mixed-replace` (MJPEG), server-sent events, and progress logs.

- **`stream.respond({ status?, headers? })`** → `Promise<void>` — send the
  response head once. The body uses `Transfer-Encoding: chunked`. A
  `content-type` of `application/octet-stream` is added when absent (set it
  explicitly for SSE/MJPEG). Rejects with `http_invalid_argument` when called
  twice and `http_closed` when the client is already gone.
- **`stream.write(data)`** → `Promise<boolean>` — append one chunk (string or
  ArrayBuffer/TypedArray) and flush it. The promise resolves `true` once the
  chunk was written to the socket — awaiting each write is the backpressure
  mechanism — and `false` once the client has disconnected (the producer loop
  should stop). Unawaited writes are bounded: exceeding the internal buffer
  limit (4 MiB) closes the stream.
- **`stream.close()`** → `Promise<void>` — finish the chunked body. Idempotent.
  When the handler never called `respond()`, a bare `204` is sent instead.
- **`stream.onClose(cb)`** — `cb` fires once, when the stream ends for any
  reason (client disconnect, overflow, server close, or local close). Use it to
  cancel producers that are not currently inside a `write()` await.
- **`stream.closed`** — boolean.

The stream is finished automatically when the handler's promise settles: a
resolved handler completes the response (or sends `204` if it never responded);
a rejected handler sends a `500` when nothing was sent yet, otherwise the
chunked body is terminated. An open stream keeps the process alive until it
finishes.

```js
server.routeStream("GET", "/mjpeg", async (req, stream) => {
  await stream.respond({
    status: 200,
    headers: { "content-type": "multipart/x-mixed-replace; boundary=frame",
               "cache-control": "no-store" },
  });
  while (!stream.closed) {
    const jpeg = await nextJpegFrame();
    await stream.write(`--frame\r\ncontent-type: image/jpeg\r\ncontent-length: ${jpeg.byteLength}\r\n\r\n`);
    if (!(await stream.write(jpeg))) break;
    await stream.write("\r\n");
  }
});
```

### `server.static(mount, root, options?)`

Serves files under the URL prefix `mount` from the filesystem directory `root`
(before `listen()`; chainable). Path traversal is rejected, and each file access
is checked against the runtime's filesystem-read policy (`allowFilesystemReads` +
`filesystemReadRoots`). MIME type is inferred from the extension; a directory
request serves `index.html`.

`root` is validated when the mount is registered, so missing directories fail
with `http_not_found` before the server starts. `options.mimeTypes` (alias
`options.mime`) overrides extension MIME types using keys with or without a
leading dot, and `options.cacheControl` adds a `Cache-Control` response header.

### `server.ws(path, handlers)`

Registers a WebSocket endpoint on a `GET` route; returns the server (chainable).
Must be called before `listen()`.

`handlers` requires `onMessage(conn, msg)` and may include `onOpen(conn)`,
`onClose(conn, code, reason)`, `maxMessageBytes`, and `maxBufferedBytes`.

- **`conn`**: `{ id, remoteAddr, bufferedAmount, send(data, opcode?), close(code?, reason?) }`.
  `send()` accepts a string or ArrayBuffer/TypedArray. Strings default to a text
  frame; buffers default to binary. `opcode` may be `"text"`, `"binary"`,
  `"ping"`, `"pong"`, or `"close"`. If sending would exceed
  `maxBufferedBytes`, the connection is closed with code `1009` and `send()`
  returns `false`.
- **`msg`**: `{ opcode, data, text() }`. `opcode` is `"text"`, `"binary"`,
  `"continuation"`, `"ping"`, `"pong"`, or `"close"`; `data` is a `wl2.Buffer`;
  `text()` decodes the payload as a JavaScript string.

Inbound messages larger than `maxMessageBytes` are rejected with close code
`1009`. Ping frames are answered automatically.

### `server.listen()` → `Promise<{ host, port }>`

Binds and starts accepting. Rejects with `http_permission_denied` when the
runtime has not granted `allowListening` + a matching `listenAllowList` entry,
or `http_listen_failed` if the bind fails. A listening server keeps the process
alive until `close()`.

### `server.close()` → `Promise<void>`

Stops accepting and releases the server. Idempotent.

## Errors

Errors use the shared module error shape with `name: "HttpError"`,
`module: "wl2_restinio"`, and a stable `code`:

| Code | Meaning |
|------|---------|
| `http_invalid_argument` | Bad argument at the JS boundary. |
| `http_permission_denied` | Listening not authorized by policy. |
| `http_listen_failed` | Bind/listen failed (e.g. port in use). |
| `http_already_listening` | `listen()` called twice. |
| `http_closed` | Operation on a closed server. |

## Build

The module is **ON by default** (`WL2_ENABLE_HTTP`). Its provider
(`WL2_HTTP_PROVIDER`) fetches a pinned RESTinio plus fmt, llhttp, and
expected-lite, and reuses the standalone Asio already fetched by `wl2:asio`; zlib
provides gzip. `WL2_HTTP_TLS` (default ON) enables HTTPS and is auto-disabled
when OpenSSL is not found. `WL2_ENABLE_HTTP=OFF` produces a dependency-minimized
build.

## Status

Implemented: HTTP/1.1 + **HTTPS/TLS** server, **WebSocket endpoints**, express
routing with path params + query, request headers/body/**cookies**/**multipart
uploads**, synchronous and Promise-returning handlers, **static file serving**
(sandboxed, filesystem-gated), **gzip** response compression, a permission-gated
`listen()`, `close()`, per-instance lifecycle (multiple servers), body-size cap
(`413`), unmatched-route `404`, and handler-error `500`.
