import { PacketBuffer } from "wl2:membus";
import { receiveRtpPackets, sendRtpPackets } from "wl2:gstreamer";

const argv = wl2.runtime.argv || [];
const portIndex = argv.findIndex((arg) => arg === "--port" || arg === "-p");
const PORT = portIndex >= 0 ? Number(argv[portIndex + 1]) : 45596;
const PLAY = argv.includes("--play");
const HOST = "127.0.0.1";
const CAPS = "application/x-rtp,media=video,encoding-name=H264,payload=96";
const OUT = `/wl2_gst_rtp_out_${Date.now()}`;
const IN = `/wl2_gst_rtp_in_${Date.now()}`;

function poll(pipeline, iterations = 20) {
  for (let i = 0; i < iterations; ++i) {
    const messages = pipeline.busPoll({ timeoutMs: 50, max: 16 });
    for (const message of messages) {
      if (message.type === "error") throw new Error(message.message || "pipeline error");
    }
  }
}

const out = PacketBuffer.create(OUT, 8, 65536, 4096, { metadata: CAPS });
out.write(new Uint8Array([
  0x80, 0x60, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00,
  0x12, 0x34, 0x56, 0x78,
  0x00, 0x00, 0x01, 0x09
]), { pts: 0, metadata: JSON.stringify({ caps: CAPS }) });

const receiver = receiveRtpPackets({
  packetBufferName: IN,
  host: HOST,
  port: PORT,
  caps: CAPS,
  buffers: 8,
  arenaSize: 65536,
  maxRecord: 4096
});
const sender = sendRtpPackets({
  packetBufferName: OUT,
  host: HOST,
  port: PORT,
  caps: CAPS
});

try {
  if (PLAY) {
    receiver.play();
    sender.play();
    const pushed = sender.pushPacket({ waitTimeoutMs: 1 });
    if (!pushed.ok) throw new Error("could not push RTP packet");
    poll(sender, 5);
    poll(receiver, 20);
    console.log(`sent bytes: ${pushed.bytes}`);
    console.log(`received packets: ${receiver.stats().wl2_packet_sink.packets}`);
  } else {
    console.log(`RTP loopback pipelines constructed on ${HOST}:${PORT}`);
  }
} finally {
  sender.close();
  receiver.close();
  out.close();
}
