// Standalone wl2:echo example.
//
// The generated executable links the static wl2_echo module and registers it, so
// this script can import wl2:echo directly. No host capabilities are required.

import { echo, shout } from "wl2:echo";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const message = "hello, winglib2";
console.log("echo:", echo(message));
console.log("shout:", shout(message));

assert(echo(message) === message, "echo should return its input unchanged");
assert(shout(message) === message.toUpperCase(), "shout should upper-case its input");

let threw = false;
try {
  echo();
} catch (error) {
  threw = error.code === "echo_invalid_argument";
}
assert(threw, "echo() without an argument should throw echo_invalid_argument");

console.log("wl2:echo example ok");
