// Starts an HTTPS wl2:http server and keeps the process alive. The TLS fixture
// passes the port (numeric) plus the cert and key file paths as script args, and
// drives it as an out-of-process TLS client.
import { HttpServer } from "wl2:http";

const argv = (globalThis.wl2 && wl2.runtime && wl2.runtime.argv) || [];
let port = 0;
let cert = "";
let key = "";
for (const arg of argv) {
  const n = Number(arg);
  if (Number.isInteger(n) && n > 0 && n < 65536) {
    port = n;
  } else if (arg.endsWith("cert.pem")) {
    cert = arg;
  } else if (arg.endsWith("key.pem")) {
    key = arg;
  }
}
if (!(port > 0) || !cert || !key) {
  throw new Error("port, cert, and key must be provided as script arguments");
}

const server = new HttpServer({ host: "127.0.0.1", port, https: { cert, key } });
server.route("GET", "/secure", () => ({ status: 200, body: "secure-hello" }));

await server.listen();
console.log(`wl2:http (tls) listening on 127.0.0.1:${port}`);
