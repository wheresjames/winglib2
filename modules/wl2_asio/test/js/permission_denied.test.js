// Runs without any --allow-network / --network-allow grant, proving network
// access is denied by default: connect() must reject with asio_permission_denied
// before any socket is opened.
import { connect } from "wl2:asio";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

let error = null;
try {
  await connect({ host: "127.0.0.1", port: 9, timeoutMs: 500 });
} catch (caught) {
  error = caught;
}

assert(error !== null, "connect() should have been denied");
assert(error.code === "asio_permission_denied", `unexpected error code: ${error.code}`);
assert(error.module === "wl2_asio", `unexpected error module: ${error.module}`);
assert(error.name === "AsioError", `unexpected error name: ${error.name}`);

console.log("wl2_asio permission denied test passed");
