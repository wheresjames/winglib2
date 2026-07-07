// Starts a wl2:http server and keeps the process alive (a listening server holds
// an outstanding async op). The native fixture picks the port, exports it as
// WL2_HTTP_TEST_PORT, drives requests as an out-of-process client, then
// terminates this process. An in-process client cannot be used because the
// wl2:curl client blocks the JS thread the server also needs.
import { HttpServer } from "wl2:http";
import { setTimeout as delayTimeout } from "wl2:asio";

const sleep = (ms) => new Promise((resolve) => delayTimeout(resolve, ms));

// The fixture passes the chosen port (numeric) and the static root (a path) as
// script arguments.
const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
let port = 0;
let staticRoot = "";
for (const arg of argv) {
  const n = Number(arg);
  if (Number.isInteger(n) && n > 0 && n < 65536) {
    port = n;
  } else if (typeof arg === "string" && arg.includes("/")) {
    staticRoot = arg;
  }
}
if (!(port > 0)) {
  throw new Error("server port not provided as a script argument");
}

const server = new HttpServer({ host: "127.0.0.1", port });

// Path params + query, explicit status/headers, object response.
server.route("GET", "/hello/:name", (req) => ({
  status: 200,
  headers: { "content-type": "text/plain" },
  body: `hi ${req.params.name} q=${req.query}`,
}));

// Synchronous string return (no object, no Promise).
server.route("GET", "/plain", () => "plain-ok");

// Async handler echoing the request body.
server.route("POST", "/echo", async (req) => ({ status: 201, body: req.body.text() }));

// Cookie parsing: echo back the "sid" cookie.
server.route("GET", "/cookie", (req) => ({ status: 200, body: `sid=${req.cookies.sid || ""}` }));

// A long, compressible body to exercise gzip when the client accepts it.
server.route("GET", "/big", () => "X".repeat(500));

// Multipart upload: echo the first file's name, filename, and contents.
server.route("POST", "/upload", (req) => {
  const f = req.files[0];
  return f ? `${f.name}:${f.filename}:${f.data.text()}` : "no-files";
});

// WebSocket echo: prove open/message/close callbacks and server-side send().
server.ws("/socket", {
  maxMessageBytes: 64,
  onOpen: (conn) => conn.send("welcome"),
  onMessage: (conn, msg) => conn.send(`echo:${msg.text()}`),
  onClose: () => {},
});

// Streaming route: several chunks over one long-lived chunked response. The
// handler returns without calling close() to prove auto-finish on settlement.
server.routeStream("GET", "/events", async (req, stream) => {
  await stream.respond({ status: 200, headers: { "content-type": "text/event-stream" } });
  for (const word of ["one", "two", "three"]) {
    const ok = await stream.write(`data: ${word}\n\n`);
    if (!ok) return;
  }
});

// Endless drip stream: proves a client disconnect surfaces as write() -> false
// (and onClose), so the producer loop can stop. /drip-status reports what the
// producer observed.
let dripState = "idle";
server.routeStream("GET", "/drip", async (req, stream) => {
  dripState = "streaming";
  stream.onClose(() => { dripState = "closed"; });
  await stream.respond({ status: 200, headers: { "content-type": "application/octet-stream" } });
  for (let i = 0; i < 10000 && !stream.closed; ++i) {
    const ok = await stream.write(`tick ${i}\n`);
    if (!ok) break;
    await sleep(10);
  }
});
server.route("GET", "/drip-status", () => dripState);

// respond() twice is rejected with a stable code.
server.routeStream("GET", "/respond-twice", async (req, stream) => {
  await stream.respond({ status: 200 });
  let code = "";
  try { await stream.respond({ status: 200 }); } catch (e) { code = e.code; }
  await stream.write(`code=${code}`);
});

// Static file serving under /assets, sandboxed to the provided root.
if (staticRoot) {
  server.static("/assets", staticRoot, {
    cacheControl: "no-store",
    mimeTypes: {
      ".m3u8": "application/vnd.apple.mpegurl",
    },
  });
}

await server.listen();
console.log(`wl2:http listening on 127.0.0.1:${port}`);
