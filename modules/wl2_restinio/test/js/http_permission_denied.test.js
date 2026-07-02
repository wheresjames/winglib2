// Default-denied policy: with no listen grant, listen() must reject with
// http_permission_denied. Run with --no-permission-prompt so the denial is
// deterministic (no interactive stdin prompt).
import { HttpServer } from "wl2:http";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const server = new HttpServer({ host: "127.0.0.1", port: 18082 });
server.route("GET", "/", () => "should not bind");

let error = null;
try {
  await server.listen();
} catch (e) {
  error = e;
}

assert(error, "listen() should have been denied");
assert(error.code === "http_permission_denied", `expected http_permission_denied, got ${error.code}`);
assert(error.name === "HttpError", `expected HttpError, got ${error.name}`);

console.log("wl2:http permission denied ok");
