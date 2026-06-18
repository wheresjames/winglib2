// Headless end-to-end check through the wl2 runner: compile a component,
// instantiate it, and round-trip number/string/bool/struct properties. No
// window is opened and no UI capability is required.
import { compile } from "wl2:slint";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const ui = await compile(`
  export component App {
    in-out property <string> name: "world";
    in-out property <int> count: 0;
    in-out property <bool> active: false;
    in-out property <{ x: int, y: int }> point: { x: 1, y: 2 };
  }
`);

const win = ui.create();

win.set("name", "winglib2");
assert(win.get("name") === "winglib2", "string round-trip failed");

win.set("count", 41);
assert(win.get("count") === 41, "number round-trip failed");

win.set("active", true);
assert(win.get("active") === true, "bool round-trip failed");

win.set("point", { x: 5, y: 6 });
const p = win.get("point");
assert(p.x === 5 && p.y === 6, "struct round-trip failed: " + JSON.stringify(p));

// Compile errors surface diagnostics.
let compileErr = null;
try {
  await compile(`export component Broken { Text { text: ; } }`);
} catch (e) {
  compileErr = e;
}
assert(compileErr && compileErr.code === "slint_compile_failed", "expected slint_compile_failed");
assert(Array.isArray(compileErr.diagnostics) && compileErr.diagnostics.length > 0,
  "compile error should carry diagnostics");

console.log("wl2_slint compile/properties test passed");
