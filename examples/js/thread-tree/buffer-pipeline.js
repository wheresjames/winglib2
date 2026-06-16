const worker = wl2.thread.spawn("inline:buffer-worker", {
  name: "buffer",
  source: `
const message = wl2.thread.recv({ timeoutMs: 1000 });
if (!message) {
  throw new Error("no buffer received");
}
wl2.thread.post("/main", { type: "processed", payload: message.payload + "-processed" });
`
});

worker.post({ type: "frame", payload: wl2.buffer.fromText("frame-001") });

const output = wl2.thread.recv({ timeoutMs: 1000 });
if (!output || output.type !== "processed") {
  throw new Error("pipeline did not produce output");
}

console.log(output.payload);
worker.close();
