// Imports a module that is provided by a dynamically loaded native library
// (passed with `wl2 run --load-module ...`), proving dynamic registration works
// end to end through the runner.
import { echo, shout } from "wl2:echo";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

assert(echo("dynamic") === "dynamic", "echo identity failed");
assert(shout("dynamic") === "DYNAMIC", "shout upper-case failed");

console.log("dynamic module smoke ok");
