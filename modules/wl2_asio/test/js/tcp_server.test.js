// Single-process server/client round trip: listen on an ephemeral loopback port,
// accept a connection from an in-process client, and exchange data both ways.
// Run with --allow-listen --listen-allow 127.0.0.1 --network-allow 127.0.0.1.
import { connect, listen } from "wl2:asio";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const server = await listen({ host: "127.0.0.1", port: 0 });
const address = server.address();
assert(address.host === "127.0.0.1", `unexpected bind host: ${address.host}`);
assert(address.port > 0, "ephemeral port was not assigned");

// Start accepting before connecting; both promises settle on the same event loop.
const acceptedPromise = server.accept({ timeoutMs: 2000 });
const client = await connect({ host: "127.0.0.1", port: address.port, timeoutMs: 2000 });
const accepted = await acceptedPromise;

// Address getters reflect the established connection.
const clientRemote = client.remoteAddress();
assert(clientRemote.host === "127.0.0.1" && clientRemote.port === address.port,
  `unexpected remoteAddress: ${clientRemote.host}:${clientRemote.port}`);
const acceptedLocal = accepted.localAddress();
assert(acceptedLocal.port === address.port, `unexpected localAddress port: ${acceptedLocal.port}`);

await client.write("hello");
const serverGot = await accepted.read({ maxBytes: 64, timeoutMs: 2000 });
assert(serverGot.text() === "hello", `server read mismatch: ${serverGot.text()}`);

await accepted.write("world");
const clientGot = await client.read({ maxBytes: 64, timeoutMs: 2000 });
assert(clientGot.text() === "world", `client read mismatch: ${clientGot.text()}`);

// Closing the client surfaces EOF (zero-length read) on the server side.
client.close();
const eof = await accepted.read({ maxBytes: 64, timeoutMs: 2000 });
assert(eof.byteLength === 0, `expected EOF, got ${eof.byteLength} bytes`);

accepted.close();

// close() is idempotent, and accept() after close() rejects with asio_closed.
server.close();
server.close();
let closedError = null;
try {
  await server.accept({ timeoutMs: 500 });
} catch (error) {
  closedError = error;
}
assert(closedError && closedError.code === "asio_closed",
  `accept after close should reject with asio_closed, got ${closedError && closedError.code}`);

console.log("wl2_asio tcp server test passed");
