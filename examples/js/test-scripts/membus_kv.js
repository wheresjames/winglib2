import { KeyValueStore, hasV12Surface } from "wl2:membus";

if (!hasV12Surface) {
  console.log("key-value example skipped: libmembus v1.2 surface unavailable");
} else {
  const name = `/wl2_example_kv_${Date.now()}_${Math.floor(Math.random() * 1000000)}`;
  const kv = KeyValueStore.create(name, 2, 15, 31);

  try {
    kv.setName(0, "state");
    kv.setName(1, "count");
    kv.setValue("state", "ready");
    kv.setValue("count", "1");
    const all = kv.all();
    if (all.state !== "ready" || all.count !== "1") {
      throw new Error("key-value snapshot failed");
    }
    console.log(`${all.state}:${all.count}`);
  } finally {
    kv.close();
  }
}
