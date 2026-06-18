// Headless check of compileFile() loading a component from a wl2:/ resource
// (mounted with --map-resource). The component imports a sibling file, so a
// successful compile also proves the include path was set to the resource's
// directory. No window is opened.
import { compileFile } from "wl2:slint";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const ui = await compileFile("wl2:/fixtures/headless.slint");
const win = ui.create();

assert(win.get("label") === "hi", "compileFile default property wrong: " + win.get("label"));
win.set("label", "loaded");
assert(win.get("label") === "loaded", "compileFile property round-trip failed");
win.set("n", 7);
assert(win.get("n") === 7, "compileFile numeric round-trip failed");

// A missing resource is reported, not crashed.
let missingErr = null;
try { await compileFile("wl2:/fixtures/does-not-exist.slint"); } catch (e) { missingErr = e; }
assert(missingErr && missingErr.code === "slint_invalid_argument", "missing resource code wrong: " + (missingErr && missingErr.code));

console.log("wl2_slint compileFile test passed");
