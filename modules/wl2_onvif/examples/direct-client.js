import { connect } from "wl2:onvif";

const [url, username = "", password = ""] = wl2.runtime.argv;
if (!url) throw new Error("usage: direct-client.js DEVICE_URL [USERNAME] [PASSWORD]");

const device = await connect(url, {
  credentials: username ? { username, password } : undefined,
  authentication: "auto",
  timeoutMs: 10_000,
});

try {
  console.log(await device.getDeviceInformation());
  const profiles = await device.media.getProfiles();
  console.log(profiles);
  if (profiles.length) console.log(await device.media.getStreamUri(profiles[0].token));
} finally {
  await device.close();
}
