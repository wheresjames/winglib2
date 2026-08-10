import { connect } from "wl2:onvif";

const [url, username = "", password = ""] = wl2.runtime.argv;
if (!url) throw new Error("usage: events.js DEVICE_URL [USERNAME] [PASSWORD]");

const device = await connect(url, {
  credentials: username ? { username, password } : undefined,
  timeoutMs: 10_000,
});

const stream = await device.events.subscribe({ topics: [], reconnect: true, queueLimit: 256 });
try {
  for await (const item of stream) console.log(item);
} finally {
  await stream.close();
  await device.close();
}
