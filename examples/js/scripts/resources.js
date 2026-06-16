const configPath = "wl2:/resources/config.json";
const assetsPath = "wl2:/resources/assets";

if (wl2.resources.exists(configPath)) {
  const config = wl2.resources.readText(configPath);
  console.log(config);
}

for (const entry of wl2.resources.list("wl2:/resources")) {
  console.log(entry.directory ? "dir " : "file", entry.path);
}

for (const entry of wl2.resources.walk(assetsPath)) {
  const handle = wl2.resources.open(entry.path);
  try {
    console.log(handle.path, handle.size, handle.compressed);
    const bytes = handle.bytes();
    console.log(bytes.byteLength);
  } finally {
    handle.close();
  }
}
