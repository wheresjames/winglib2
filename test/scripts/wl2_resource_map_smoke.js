function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const config = JSON.parse(wl2.resources.readText("wl2:/mapped/config.json"));
assert(config.name === "winglib2", "mapped config did not load");

const nested = wl2.resources.readText("wl2:/mapped/assets/nested/info.txt");
assert(nested.trim() === "Nested resource loaded from a directory.", "mapped nested file did not load");

const entries = wl2.resources.list("wl2:/mapped");
assert(entries.some((entry) => entry.path === "wl2:/mapped/config.json"), "mapped list missing config");
assert(entries.some((entry) => entry.path === "wl2:/mapped/assets" && entry.directory), "mapped list missing assets dir");

let escaped = false;
try {
  wl2.resources.readText("wl2:/mapped/../config.json");
} catch (error) {
  escaped = error.code === "resource_not_found";
}
assert(escaped, "mapped resource traversal should not resolve");

console.log("resource map smoke passed");
