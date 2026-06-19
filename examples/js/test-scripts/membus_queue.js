import { SharedQueue } from "wl2:membus";

const name = `/wl2_example_queue_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
const writer = SharedQueue.create(name, 512, true);
const reader = SharedQueue.attach(name, 512, false);

try {
  writer.write("hello queue");
  const message = reader.read({ timeoutMs: 1000 });
  if (message.payload.text() !== "hello queue") {
    throw new Error("queue round trip failed");
  }
  console.log(message.payload.text());
} finally {
  reader.close();
  writer.close();
}
