// Opt-in windowed smoke test (label "display"): requires a real backend, so it
// is excluded from the default suite and only built when WL2_SLINT_DISPLAY_TESTS
// is ON. Run with `wl2 run --allow-ui`.
//
// It opens a window, lets a Slint Timer fire once (driving the event loop and
// the pump that runs JS jobs on the UI thread), quits the loop from the
// resulting JavaScript callback, and asserts run() returned cleanly.
import { compile } from "wl2:slint";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const ui = await compile(`
  export component MainWindow inherits Window {
    width: 200px;
    height: 120px;
    in-out property <int> ticks: 0;
    callback tick();
    Timer {
      interval: 50ms;
      running: true;
      triggered => { root.tick(); }
    }
    Text { text: "ticks: " + root.ticks; }
  }
`);

const win = ui.create();

let fired = false;
win.on("tick", () => {
  fired = true;
  win.set("ticks", win.get("ticks") + 1);
  ui.quit();
});

win.show();
await ui.run();  // blocks until ui.quit(); the pump keeps JS jobs flowing

assert(fired, "the tick callback should have fired from the running event loop");
assert(win.get("ticks") === 1, "the JS-driven property change should be reflected: " + win.get("ticks"));

console.log("wl2_slint windowed smoke test passed");
