import { HttpServer } from "wl2:http";

const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
const selftest = argv.includes("--selftest");

let port = selftest ? 0 : 18081;
const portIndex = argv.indexOf("--port");
if (portIndex >= 0 && portIndex + 1 < argv.length) {
  const parsed = Number(argv[portIndex + 1]);
  if (Number.isInteger(parsed) && parsed >= 0 && parsed < 65536) {
    port = parsed;
  }
}

const host = "127.0.0.1";
const startedAt = new Date().toISOString();
let requestCount = 0;
let wsConnections = 0;

const html = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>wl2:http showcase</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
    body { margin: 0; padding: 32px; line-height: 1.5; }
    main { max-width: 880px; margin: 0 auto; }
    h1 { margin: 0 0 8px; }
    section { border-top: 1px solid color-mix(in srgb, CanvasText 20%, transparent); padding: 20px 0; }
    button, input { font: inherit; padding: 8px 10px; }
    input { min-width: 260px; }
    pre { overflow: auto; padding: 12px; background: color-mix(in srgb, CanvasText 8%, transparent); }
  </style>
</head>
<body>
  <main>
    <h1>wl2:http showcase</h1>
    <p>This page is served by an embedded Winglib2 RESTinio server.</p>

    <section>
      <h2>HTTP JSON</h2>
      <button id="status">Fetch status</button>
      <pre id="statusOut">Waiting...</pre>
    </section>

    <section>
      <h2>POST body</h2>
      <input id="echoText" value="hello from the browser">
      <button id="echo">POST /api/echo</button>
      <pre id="echoOut">Waiting...</pre>
    </section>

    <section>
      <h2>WebSocket echo</h2>
      <input id="wsText" value="hello websocket">
      <button id="wsSend">Send</button>
      <pre id="wsOut">Connecting...</pre>
    </section>
  </main>
  <script>
    const statusOut = document.querySelector("#statusOut");
    document.querySelector("#status").onclick = async () => {
      const res = await fetch("/api/status");
      statusOut.textContent = JSON.stringify(await res.json(), null, 2);
    };

    const echoOut = document.querySelector("#echoOut");
    document.querySelector("#echo").onclick = async () => {
      const body = document.querySelector("#echoText").value;
      const res = await fetch("/api/echo", { method: "POST", body });
      echoOut.textContent = await res.text();
    };

    const wsOut = document.querySelector("#wsOut");
    const ws = new WebSocket("ws://" + location.host + "/ws");
    ws.onopen = () => { wsOut.textContent = "Connected. Try sending a message."; };
    ws.onmessage = (event) => { wsOut.textContent = event.data; };
    ws.onclose = () => { wsOut.textContent += "\\nClosed."; };
    document.querySelector("#wsSend").onclick = () => {
      ws.send(document.querySelector("#wsText").value);
    };
  </script>
</body>
</html>`;

const server = new HttpServer({ host, port, maxBodyBytes: 1 << 20 });

server.route("GET", "/", () => ({
  headers: { "content-type": "text/html; charset=utf-8" },
  body: html,
}));

server.route("GET", "/api/status", (req) => {
  requestCount += 1;
  return {
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      ok: true,
      method: req.method,
      path: req.path,
      startedAt,
      requestCount,
      wsConnections,
    }, null, 2),
  };
});

server.route("POST", "/api/echo", (req) => {
  requestCount += 1;
  return {
    status: 201,
    headers: { "content-type": "text/plain; charset=utf-8" },
    body: `server received: ${req.body.text()}`,
  };
});

server.ws("/ws", {
  maxMessageBytes: 4096,
  onOpen: (conn) => {
    wsConnections += 1;
    conn.send("connected to wl2:http");
  },
  onMessage: (conn, msg) => {
    conn.send(`echo from wl2:http: ${msg.text()}`);
  },
  onClose: () => {
    wsConnections = Math.max(0, wsConnections - 1);
  },
});

const bound = await server.listen();
console.log(`wl2:http showcase listening on http://${host}:${port}/`);
console.log("Routes: GET /, GET /api/status, POST /api/echo, WS /ws");

if (selftest) {
  await server.close();
  console.log("wl2:http showcase selftest ok");
}
