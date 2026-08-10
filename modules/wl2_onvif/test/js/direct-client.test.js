import { connect, discover, localNetworks, OnvifError, version } from "wl2:onvif";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(typeof connect === "function", "connect export is missing");
assert(typeof discover === "function", "discover export is missing");
assert(typeof localNetworks === "function", "localNetworks export is missing");
assert(typeof OnvifError === "function", "OnvifError export is missing");
assert(typeof version === "string" && version.length > 0, "native version is missing");

const constructed = new OnvifError("test error");
assert(constructed.name === "OnvifError", "OnvifError name is unstable");
assert(constructed.code === "onvif_internal", "OnvifError code is unstable");

await connect("http://127.0.0.1:9/onvif/device_service", {
  signal: { aborted: true },
}).then(
  () => { throw new Error("already-aborted connect resolved"); },
  error => assert(error.code === "onvif_cancelled", `unexpected abort code: ${error.code}`),
);

await connect("http://127.0.0.1:9/onvif/device_service", {
  authentication: "http-basic",
}).then(
  () => { throw new Error("unsupported authentication resolved"); },
  error => assert(error.code === "onvif_unsupported", `unexpected auth code: ${error.code}`),
);

await discover({ interfaces: ["127.0.0.1"] }).then(
  () => { throw new Error("discovery unexpectedly resolved"); },
  error => assert(error.code === "onvif_permission_unavailable", `unexpected discovery code: ${error.code}`),
);

const device = await connect("http://127.0.0.1:9/onvif/device_service", { timeoutMs: 1000 });
assert(device.deviceServiceUrl.includes("127.0.0.1"), "session URL is missing");
assert(device.authentication === "auto" && device.closed === false, "session state is incomplete");
assert(device.media && device.ptz && device.events, "session clients are missing");

await device.getDeviceInformation({ timeoutMs: 1000 }).then(
  () => { throw new Error("denied device query resolved"); },
  error => assert(error.code === "onvif_permission_denied", `unexpected permission code: ${error.code}`),
);

const closed = await device.close();
assert(closed.cleanupComplete === true, "session cleanup did not complete");
assert(device.closed === true, "session did not expose closed state");
console.log("wl2:onvif direct-client smoke passed");
