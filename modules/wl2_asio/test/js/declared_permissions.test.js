/* wl2
permissions:
  listen: ["127.0.0.1:*"]
  network:
    - "127.0.0.1:*"
*/

import { connect, listen } from "wl2:asio";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const permissions = {
  listen: ["127.0.0.1:*"],
  network: ["127.0.0.1:*"]
};
assert(!wl2.runtime.hasPermissions(permissions), "declared envelope should not grant by itself");

const granted = await wl2.runtime.requestPermissions(permissions);
assert(granted.granted === true, `permission request failed: ${granted.error}`);
assert(wl2.runtime.hasPermissions(permissions), "requested permissions should be effective");

const server = await listen({ host: "127.0.0.1", port: 0 });
const address = server.address();
assert(address.port > 0, "ephemeral listen port was not assigned");

const acceptedPromise = server.accept({ timeoutMs: 2000 });
const client = await connect({ host: "127.0.0.1", port: address.port, timeoutMs: 2000 });
const accepted = await acceptedPromise;

await client.write("declared");
const got = await accepted.read({ maxBytes: 64, timeoutMs: 2000 });
assert(got.text() === "declared", `unexpected payload: ${got.text()}`);

client.close();
accepted.close();
server.close();

console.log("wl2 declared permissions test passed");
