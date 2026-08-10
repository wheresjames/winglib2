import { connect } from "wl2:onvif";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function rejectsCode(promise, code) {
  await promise.then(
    () => { throw new Error(`expected ${code}`); },
    error => assert(error.code === code, `expected ${code}, got ${error.code}`),
  );
}

const device = await connect("http://127.0.0.1:9/onvif/device_service", { timeoutMs: 1000 });
assert(device.events, "EventClient is missing");
assert(typeof device.events.createPullPoint === "function", "createPullPoint is missing");
assert(typeof device.events.subscribe === "function", "subscribe is missing");

await rejectsCode(device.events.getEventProperties(), "onvif_unsupported");
await rejectsCode(device.events.createPullPoint({ topics: ["tns1:Device"] }), "onvif_unsupported");
await rejectsCode(device.events.subscribe({ topics: ["tns1:Device"] }), "onvif_unsupported");
await rejectsCode(device.events.subscribe({ reconnect: false }), "onvif_unsupported");
await rejectsCode(device.events.createPullPoint({ queueLimit: 8, timeoutMs: 1000 }), "onvif_permission_denied");

await device.close();
console.log("wl2:onvif events smoke passed");
