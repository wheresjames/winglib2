import { SharedQueue } from "wl2:membus";

const request = wl2.thread.recv({ timeoutMs: 1000 });
if (!request || request.type !== "process-queue") {
  throw new Error("expected process-queue request");
}

const { queueName } = JSON.parse(request.payload);
const queue = SharedQueue.attach(queueName, 4096, false);

try {
  const message = queue.read({ timeoutMs: 1000 });
  request.reply(`${message.payload.text()}-processed`);
} finally {
  queue.close();
}
