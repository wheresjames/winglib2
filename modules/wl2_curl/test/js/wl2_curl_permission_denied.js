import { get } from "wl2:curl";

const baseUrl = globalThis.WL2_CURL_TEST_URL || "";
if (!baseUrl) {
  throw new Error("WL2_CURL_TEST_URL is required");
}

let denied = false;
try {
  await get(`${baseUrl}/text`, { timeoutMs: 5000 });
} catch (error) {
  denied = String(error && error.message || error).includes("network_connect_denied");
}

if (!denied) {
  throw new Error("wl2:curl request was not denied by network policy");
}

console.log("wl2_curl permission denial test passed");
