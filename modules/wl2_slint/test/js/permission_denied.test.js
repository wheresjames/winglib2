// Runs without any --allow-ui grant, proving the UI capability is denied by
// default: compiling and instantiating succeed (no window), but show() rejects
// with slint_permission_denied before any window is opened.
import { compile } from "wl2:slint";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const ui = await compile(`
  export component App inherits Window {
    in-out property <string> greeting: "hello";
  }
`);

const win = ui.create();

// Compile + instantiate must work without the capability.
assert(win.get("greeting") === "hello", "instantiate/get should work without the UI capability");

let error = null;
try {
  win.show();
} catch (caught) {
  error = caught;
}

assert(error !== null, "show() should have been denied");
assert(error.code === "slint_permission_denied", `unexpected error code: ${error.code}`);
assert(error.module === "wl2_slint", `unexpected error module: ${error.module}`);
assert(error.name === "SlintError", `unexpected error name: ${error.name}`);

console.log("wl2_slint permission denied test passed");
