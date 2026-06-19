# wl2:asio design

## Threading

The module owns a single `asio::io_context` driven by one worker thread
(`AsioRuntimeState`). It is created lazily on first `connect()` and its run loop
is restartable so sequential runtimes (e.g. `wl2 run --watch`) each get a live
worker. The `io_context` object itself is never destroyed, so socket finalizers
can always close their file descriptors safely.

All socket work happens on the worker thread. JavaScript-facing functions
marshal a request onto it with `asio::post`, and completions are marshalled back
onto the JavaScript thread through `Runtime::async().post()`, where settling a
QuickJS promise is safe.

## Operation lifetime

Each operation:

1. On the JS thread: validates arguments, checks the capability gate (connect),
   creates a `JS_NewPromiseCapability`, calls `Runtime::async().beginOperation()`
   to keep the engine event loop alive, and posts the work to the worker.
2. On the worker thread: runs the Asio async op, racing an optional
   `steady_timer` for `timeoutMs`. Because both run on the single worker thread,
   a plain `done` flag (no atomics) claims the first completer.
3. Settlement is posted back to the JS thread, which resolves/rejects the
   promise, frees the resolve/reject functions exactly once, and calls
   `endOperation()`.

## Ownership

`TcpSocketHandle` (the Asio socket plus close/in-flight flags) is held by a
`shared_ptr` shared between the JavaScript `TcpSocket` object and any in-flight
operation, so a socket cannot be destroyed mid-operation. The JS object stores
the handle via a class with a finalizer that closes the socket as a backstop.

## Shutdown

The module registers a `Runtime::async().registerShutdownHook()` that stops the
`io_context` and joins the worker thread, so it never outlives the runtime.
`Runtime::~Runtime()` invokes `async().shutdown()`, which runs the hook.

## Server

`listen()` opens the acceptor synchronously (bind/listen do not block) and
resolves a ready `TcpServer`. `accept()` follows the same worker-thread / promise
model as the socket operations, with its own `steady_timer` for `timeoutMs`.
`close()` posts `cancel()` + `close()` onto the worker thread, so a pending
accept settles with `asio_closed`.

## Not yet implemented

- Best-effort settling of still-pending operations with `asio_cancelled` at
  runtime shutdown (today they are abandoned as the loop ends; explicit `close()`
  and per-operation `timeoutMs` already give deterministic settlement).
- UDP, TLS, and Unix-domain sockets — future expansion.
