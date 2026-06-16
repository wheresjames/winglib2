const worker = wl2.thread.spawn("inline:worker", {
  name: "worker",
  source: `
wl2.thread.post("/main", { type: "ready", payload: wl2.thread.path });
`
});

const ready = wl2.thread.recv({ timeoutMs: 1000 });
if (!ready || ready.type !== "ready") {
  throw new Error("worker did not report ready");
}

console.log(`spawned ${ready.payload}`);
worker.close();
