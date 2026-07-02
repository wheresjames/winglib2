import { compile } from "wl2:slint";
import { testPattern } from "wl2:gstreamer";

const RING = `/wl2_gst_slint_preview_${Date.now()}`;
const SOURCE = `
  export component Preview inherits Window {
    title: "wl2 gstreamer preview";
    width: 320px;
    height: 240px;
    in property <image> frame;
    callback tick();
    Timer {
      interval: 33ms;
      running: true;
      triggered => { root.tick(); }
    }
    Image {
      width: 100%;
      height: 100%;
      source: root.frame;
      image-fit: contain;
    }
  }
`;

const ui = await compile(SOURCE);
const win = ui.create();

// Continuous, animated source: pattern "ball" moves each frame and
// numBuffers -1 keeps videotestsrc producing until the pipeline is closed.
const pipeline = testPattern({
  videoBufferName: RING,
  width: 320,
  height: 240,
  fps: 30,
  buffers: 4,
  numBuffers: -1,
  pattern: "ball"
});

// Copy the newest frame from the shared-memory ring into the image property.
// Early ticks may fire before the first frame lands; ignore those.
let frames = 0;
function refresh() {
  try {
    win.setImageFromFrameRing("frame", RING);
    frames += 1;
  } catch (error) {
    if (error.code !== "gstreamer_invalid_argument" && error.code !== "slint_invalid_argument") {
      throw error;
    }
  }
}

try {
  pipeline.play();
  win.on("tick", refresh);
  refresh();
  win.show();
  await ui.run(); // blocks until the window is closed
  console.log(`slint preview displayed ${frames} frames`);
} finally {
  pipeline.close();
}
