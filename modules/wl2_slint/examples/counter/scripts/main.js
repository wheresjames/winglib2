// wl2:slint counter example.
//
// Loads its UI from an embedded resource (compileFile), wires the increment
// callback, opens a window, and runs the event loop. Built as a single static
// executable (see main.cpp / CMakeLists.txt); run it directly to get a window,
// or in-tree with:
//
//   wl2 run --allow-ui --map-resource modules/wl2_slint/examples/counter:wl2:/counter \
//     wl2:/counter/scripts/main.js
//
// Passing --selftest makes it auto-increment once and quit (used by the headless
// CI smoke test under xvfb); otherwise it stays open until the window closes.
import { compileFile } from "wl2:slint";

const ui = await compileFile("wl2:/counter/ui/app.slint");
const win = ui.create();

win.set("name", "winglib2");
win.on("increment", () => win.set("count", win.get("count") + 1));

const selftest = (wl2.runtime.argv || []).includes("--selftest");
if (selftest) {
  win.on("selftest-tick", () => {
    win.invoke("increment");
    ui.quit();
  });
  win.set("selftest", true);
}

win.show();
await ui.run();

console.log(`wl2:slint counter exited with count=${win.get("count")}`);
