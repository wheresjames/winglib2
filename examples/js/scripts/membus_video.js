import { VideoBuffer } from "wl2:membus";

const name = `/wl2_example_video_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
const video = VideoBuffer.create(name, 4, 3, 30, 2);

try {
  video.fill(0, 0x44);
  const frame = video.frame(0);
  if (frame.width !== 4 || frame.height !== 3 || frame.data.uint8Array()[0] !== 0x44) {
    throw new Error("video frame check failed");
  }
  console.log(`${frame.width}x${frame.height}`);
} finally {
  video.close();
}
