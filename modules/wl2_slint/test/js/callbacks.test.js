// Headless end-to-end check of on()/invoke() through the wl2 runner: register
// JS handlers for component callbacks and invoke them, asserting argument and
// return-value marshaling and a JS-driven property change — no window opened.
import { compile } from "wl2:slint";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const ui = await compile(`
  export component App {
    in-out property <int> count: 0;
    callback add(int, int) -> int;
    callback bump();
  }
`);

const win = ui.create();

// argument + return-value marshaling
win.on("add", (a, b) => a + b);
assert(win.invoke("add", 2, 3) === 5, "callback arg/return marshaling failed");

// a JS handler drives a property change reflected in the component
win.on("bump", () => win.set("count", win.get("count") + 1));
win.invoke("bump");
win.invoke("bump");
assert(win.get("count") === 2, "JS-driven property change not reflected: " + win.get("count"));

// re-registering replaces the handler
win.on("add", (a, b) => a * b);
assert(win.invoke("add", 4, 5) === 20, "callback re-registration failed");

// invoking an unknown callback is a stable error
let cbErr = null;
try { win.invoke("nope"); } catch (e) { cbErr = e; }
assert(cbErr && cbErr.code === "slint_unknown_callback", "unknown callback code wrong");

console.log("wl2_slint callbacks test passed");
