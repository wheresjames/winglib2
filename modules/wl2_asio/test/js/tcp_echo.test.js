// Connects to the loopback echo fixture, writes bytes, reads them back, and
// verifies clean close semantics. The fixture exports WL2_ASIO_TEST_HOST/PORT
// and grants network access only for that endpoint.
import { connect } from "wl2:asio";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const host = globalThis.WL2_ASIO_TEST_HOST || "127.0.0.1";
const port = Number(globalThis.WL2_ASIO_TEST_PORT || "0");
assert(port > 0, "WL2_ASIO_TEST_PORT not provided");

const socket = await connect({ host, port, timeoutMs: 2000 });

const written = await socket.write("ping");
assert(written.bytesWritten === 4, `unexpected bytesWritten: ${written.bytesWritten}`);

const echoed = await socket.read({ maxBytes: 64, timeoutMs: 2000 });
assert(wl2.buffer.isBuffer(echoed), "read did not return a wl2.Buffer");
assert(echoed.text() === "ping", `unexpected echo: ${echoed.text()}`);

// A second round trip proves the socket stays usable across operations.
await socket.write("pong");
const second = await socket.read({ maxBytes: 64, timeoutMs: 2000 });
assert(second.text() === "pong", `unexpected second echo: ${second.text()}`);

// close() is idempotent.
socket.close();
socket.close();

// Operations after close reject with a stable error code.
let closedError = null;
try {
  await socket.read({ maxBytes: 16, timeoutMs: 500 });
} catch (error) {
  closedError = error;
}
assert(closedError && closedError.code === "asio_closed", "read after close should reject with asio_closed");

console.log("wl2_asio tcp echo test passed");
