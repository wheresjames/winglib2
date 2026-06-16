const worker = wl2.thread.spawn("inline:echo-worker", {
  name: "echo",
  source: `
const req = wl2.thread.recv({ timeoutMs: 1000 });
if (!req) {
  throw new Error("no request received");
}
req.reply(req.payload.toUpperCase());
`
});

const reply = worker.request({ type: "uppercase", payload: "hello" }, { timeoutMs: 1000 });
if (!reply.ok) {
  throw new Error(reply.error || reply.status);
}

console.log(reply.payload);
worker.close();
