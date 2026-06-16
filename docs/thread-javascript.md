# JavaScript Thread Tree {#thread_javascript}

The QuickJS runtime exposes the core thread tree as `wl2.thread`. Scripts use
paths instead of shared JavaScript state: each spawned worker runs in its own
engine instance and communicates through mailbox messages or bounded
request/reply calls.

```js
const worker = wl2.thread.spawn("wl2:/workers/worker.js", {
  name: "worker"
});

worker.post({ type: "hello", payload: "one-way" });

const reply = worker.request({ type: "work", payload: "input" }, {
  timeoutMs: 1000
});

const pending = worker.requestPending({ type: "other-work", payload: "input" }, {
  timeoutMs: 1000
});
const maybeReply = pending.poll() || pending.wait({ timeoutMs: 1000 });

worker.close();
```

The global thread API is:

- `wl2.thread.path`
- `wl2.thread.parent`
- `wl2.thread.children()`
- `wl2.thread.spawn(script, options)`
- `wl2.thread.post(destination, message)`
- `wl2.thread.request(destination, message, options)`
- `wl2.thread.requestPending(destination, message, options)`
- `wl2.thread.recv(options)`
- `wl2.thread.requests()`
- `wl2.thread.shutdown(options)`

Worker handles expose:

- `worker.path`
- `worker.post(message)`
- `worker.request(message, options)`
- `worker.requestPending(message, options)`
- `worker.wait(options)`
- `worker.close()`

Pending reply handles expose `id`, `done`, `poll()`, `wait(options)`, and
`cancel()`. They are also thenable, so `await pending` resolves successful
replies and rejects timeout, rejection, cancellation, or unreachable outcomes
with a `ThreadReplyError`.

Received request objects contain `id`, `source`, `destination`, `type`,
`payload`, and `expectsReply`, plus `reply(payload, type)` and
`reject(error)`. A request that is dropped on the C++ side is rejected so callers
do not wait forever.

Blocking calls accept `timeoutMs`. Timeout and cleanup behavior is covered by
the `core.script_thread.javascript_api` CTest entry and by the thread-tree
examples.
