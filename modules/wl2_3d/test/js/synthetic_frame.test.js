import { Scene } from "wl2:3d";
import { VideoBuffer, hasV12Surface } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

if (!hasV12Surface) {
  console.log("wl2:3d synthetic frame test skipped: libmembus v1.2 surface unavailable");
} else {
  const name = `/wl2_3d_test_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
  const scene = await Scene.create({ size: [8, 4] });
  const published = scene.publishTo(name);
  assert(published.width === 8 && published.height === 4, "published metadata size mismatch");
  assert(published.format === "rgba8", "published format mismatch");
  assert(published.alpha === "premultiplied", "published alpha mismatch");
  assert(published.origin === "top-left", "published origin mismatch");
  assert(published.sequence >= 0, "published sequence missing");

  const video = VideoBuffer.openExisting(name);
  try {
    const meta = video.metadata();
    assert(meta.width === 8 && meta.height === 4, "video metadata size mismatch");
    assert(meta.bytesPerPixel === 4, "video should be RGBA");
    assert(meta.formatName === "RGBA32", `unexpected format: ${meta.formatName}`);
    assert(meta.sequence >= published.sequence, "video sequence mismatch");
    const frame = video.frame(0);
    assert(frame.scanWidth >= 8 * 4, "frame stride too small");
    const bytes = frame.data.uint8Array();
    assert(bytes[3] === 255, "first pixel should be opaque");
    // The renderer draws the scene (here an empty scene, so just the sky
    // background), producing a non-blank, opaque frame in every build.
    assert(bytes[0] !== 0 || bytes[1] !== 0 || bytes[2] !== 0, "rendered frame should not be blank");
  } finally {
    video.close();
    scene.close();
  }
}

console.log("wl2:3d synthetic frame ok");
