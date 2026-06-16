import { SharedQueue } from "wl2:membus";

const queueName = "/wl2_thread_tree_example";
const queue = SharedQueue.create(queueName, 4096, true);

const worker = wl2.thread.spawn("inline:membus-worker", {
  name: "membus",
  source: `
import { SharedQueue } from "wl2:membus";
const queue = SharedQueue.attach("${queueName}", 4096, false);
const message = queue.read({ timeoutMs: 1000 });
wl2.thread.post("/main", { type: "queue-message", payload: message.payload.text() });
queue.close();
`
});

queue.write("hello over membus");

const result = wl2.thread.recv({ timeoutMs: 1000 });
if (!result || result.type !== "queue-message") {
  throw new Error("worker did not forward the membus message");
}

console.log(result.payload);
worker.close();
queue.close();
