// UI-on-3D spike: render a Slint component off-screen into a FrameRing (UI is
// writer), then inject a synthetic pointer click that flips a property — the
// component re-renders and the ring pixels change. No window, no GPU.
import { compile, useOffscreenRendering } from "wl2:slint";
import { VideoBuffer, hasV12Surface } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function centerPixel(name, x, y) {
  const video = VideoBuffer.openExisting(name);
  try {
    const frame = video.frame(0);
    const bytes = frame.data.uint8Array();
    const idx = y * frame.scanWidth + x * 4;
    return { r: bytes[idx], g: bytes[idx + 1], b: bytes[idx + 2], a: bytes[idx + 3] };
  } finally {
    video.close();
  }
}

if (!hasV12Surface) {
  console.log("wl2:slint offscreen test skipped: libmembus v1.2 surface unavailable");
} else {
  // Must precede compile()/create(): Slint's platform is process-global.
  useOffscreenRendering();

  const source = `
export component HeadlessButton inherits Window {
    width: 64px;
    height: 32px;
    in-out property <bool> on: false;
    background: root.on ? #00ff00 : #ff0000;
    TouchArea {
        clicked => { root.on = !root.on; }
    }
}`;

  const component = await compile(source);
  const instance = component.create();

  const name = `/wl2_3d_test_slint_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
  const meta = instance.renderOffscreenTo(name, { size: [64, 32] });
  assert(meta.width === 64 && meta.height === 32, "offscreen size mismatch");
  assert(meta.format === "rgba8", "format mismatch");
  assert(meta.origin === "top-left", "origin mismatch");
  assert(meta.alpha === "premultiplied", "alpha contract mismatch");
  assert(instance.get("on") === false, "initial property should be false");

  const before = centerPixel(name, 32, 16);
  assert(before.a === 255, "frame should be opaque");
  // Initial background is red.
  assert(before.r > 120 && before.g < 90, `expected red initial pixel, got ${before.r},${before.g},${before.b}`);

  // Inject a click; TouchArea.clicked flips `on`, recoloring to green.
  const after = instance.injectPointer(32, 16, "click");
  assert(after.sequence > meta.sequence, "sequence should advance after input");
  assert(instance.get("on") === true, "click should toggle the bound property");

  const post = centerPixel(name, 32, 16);
  assert(post.g > before.g + 40, `green should rise after click (${before.g} -> ${post.g})`);
  assert(post.r < before.r - 40, `red should fall after click (${before.r} -> ${post.r})`);

  // A second click toggles back to red and re-renders.
  instance.injectPointer(32, 16, "click");
  assert(instance.get("on") === false, "second click should toggle back");
  const reset = centerPixel(name, 32, 16);
  assert(reset.r > 120 && reset.g < 90, `expected red after second click, got ${reset.r},${reset.g}`);

  console.log("wl2:slint offscreen round-trip ok");
}
