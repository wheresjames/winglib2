// Loopback echo demo: start a server on an ephemeral port, connect a client to
// it, and bounce a message through. Everything runs in one process and one event
// loop. The host runner grants only 127.0.0.1 access.
import { connect, listen } from "wl2:asio";

const server = await listen({ host: "127.0.0.1", port: 0 });
const { host, port } = server.address();
console.log(`wl2:asio echo server listening on ${host}:${port}`);

// Accept and connect concurrently; both promises settle on the same event loop.
const acceptedPromise = server.accept({ timeoutMs: 2000 });
const client = await connect({ host, port, timeoutMs: 2000 });
const accepted = await acceptedPromise;

const message = "hello from wl2:asio";
await client.write(message);

const received = await accepted.read({ maxBytes: 1024, timeoutMs: 2000 });
console.log(`server received: ${received.text()}`);

// Echo it back upper-cased.
await accepted.write(received.text().toUpperCase());
const reply = await client.read({ maxBytes: 1024, timeoutMs: 2000 });
console.log(`client received: ${reply.text()}`);

if (reply.text() !== message.toUpperCase()) {
  throw new Error(`unexpected echo: ${reply.text()}`);
}

client.close();
accepted.close();
server.close();
console.log("wl2:asio tcp-echo example done");
