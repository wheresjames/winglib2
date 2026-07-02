import { PacketBuffer } from "wl2:membus";
import { parseLaunch } from "wl2:gstreamer";

function pollToEos(pipeline, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 16 });
    for (const message of messages) {
      if (message.type === "error") throw new Error(message.message || "pipeline error");
      if (message.type === "eos") return true;
    }
  }
  return false;
}

const caps = "application/octet-stream";
const sourceName = `/wl2_gst_packet_src_${Date.now()}`;
const sinkName = `/wl2_gst_packet_sink_${Date.now()}`;
const source = PacketBuffer.create(sourceName, 8, 65536, 4096, { metadata: caps });

try {
  source.write(new Uint8Array([0, 0, 1, 9, 16, 0, 0, 1, 65, 136]), {
    pts: 123,
    duration: 33333333,
    metadata: JSON.stringify({ caps })
  });

  const relay = parseLaunch("appsrc name=wl2_packet_src caps=application/octet-stream ! appsink name=wl2_packet_sink");
  relay.attachPacketSource({ packetBufferName: sourceName, caps });
  relay.attachPacketSink({
    packetBufferName: sinkName,
    create: true,
    buffers: 8,
    arenaSize: 65536,
    maxRecord: 4096,
    caps,
    track: 1
  });
  relay.play();
  const pushed = relay.pushPacket({ waitTimeoutMs: 1 });
  if (!pushed.ok) throw new Error("could not push packet");
  relay.endOfStream();
  if (!pollToEos(relay)) throw new Error("packet relay timed out");
  const stats = relay.stats();
  console.log(`pushed=${stats.wl2_packet_src.pushed} relayed=${stats.wl2_packet_sink.packets}`);
  relay.close();
} finally {
  source.close();
}
