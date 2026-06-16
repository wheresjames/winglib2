import { SharedQueue } from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const config = JSON.parse(wl2.resources.readText("wl2:/foundations/config.json"));
const queueName = `${config.queuePrefix}_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
const queue = SharedQueue.create(queueName, 4096, true);

try {
  queue.write(config.input);

  const worker = wl2.thread.spawn("wl2:/foundations/worker.js", {
    name: "worker"
  });

  try {
    const reply = worker.request({
      type: "process-queue",
      payload: { queueName }
    }, { timeoutMs: 1000 });

    assert(reply.ok, reply.error || reply.status);
    assert(reply.payload === `${config.input}-processed`, `unexpected reply: ${reply.payload}`);
    assert(worker.wait({ timeoutMs: 1000 }), "worker did not finish");
  } finally {
    worker.close();
  }
} finally {
  queue.close();
  wl2.thread.shutdown({ timeoutMs: 100 });
}

console.log("foundations combined example ok");
