import { get } from "wl2:curl";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const config = JSON.parse(wl2.resources.readText("wl2:/app/config.json"));
const payload = wl2.resources.readText("wl2:/app/payload.txt").trim();

assert(payload.length > 0, "embedded payload is empty");

console.log(`resource: ${payload}`);

try {
  const response = await get(config.url, {
    timeoutMs: config.timeoutMs,
    headers: {
      "User-Agent": config.userAgent
    }
  });

  assert(response.status >= 200 && response.status < 400, `GET failed: ${response.status}`);
  assert(wl2.buffer.isBuffer(response.body), "curl response body is not a wl2.Buffer");
  console.log(`curl: GET ${response.status} ${response.url || config.url}`);
} catch (error) {
  console.log(`curl: unavailable (${error.message})`);
}
