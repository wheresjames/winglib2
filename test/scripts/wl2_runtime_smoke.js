// Smoke test for the wl2.runtime JavaScript surface.
//
// Invoked as: wl2 run wl2_runtime_smoke.js -- runtime-arg-1 runtime-arg-2
// The two trailing arguments exercise host argv forwarding.

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

assert(typeof wl2 === "object" && wl2 !== null, "wl2 global is missing");
assert(typeof wl2.runtime === "object" && wl2.runtime !== null, "wl2.runtime is missing");

// version, build, and engine are always available.
assert(typeof wl2.runtime.version === "string" && wl2.runtime.version.length > 0,
  "wl2.runtime.version is not a non-empty string");
assert(typeof wl2.runtime.build === "string" && wl2.runtime.build.length > 0,
  "wl2.runtime.build is not a non-empty string");
assert(typeof wl2.runtime.engine === "string" && wl2.runtime.engine.length > 0,
  "wl2.runtime.engine is not a non-empty string");
assert(wl2.runtime.engine === "quickjs" || wl2.runtime.engine === "v8",
  `unexpected engine: ${wl2.runtime.engine}`);
console.log("runtime", wl2.runtime.version, wl2.runtime.build, wl2.runtime.engine);

// modules() always returns an array of metadata entries.
const modules = wl2.runtime.modules();
assert(Array.isArray(modules), "wl2.runtime.modules() did not return an array");
for (const entry of modules) {
  assert(typeof entry.name === "string" && entry.name.length > 0,
    "module entry is missing a name");
  assert(typeof entry.version === "string", "module entry is missing a version");
  assert(typeof entry.build === "string", "module entry is missing a build");
}
console.log("modules", JSON.stringify(modules.map((m) => m.name)));

// argv reflects host-forwarded arguments after the script path.
assert(Array.isArray(wl2.runtime.argv), "wl2.runtime.argv is not an array");
assert(wl2.runtime.argv.length === 2,
  `expected 2 forwarded arguments, got ${wl2.runtime.argv.length}`);
assert(wl2.runtime.argv[0] === "runtime-arg-1", "argv[0] mismatch");
assert(wl2.runtime.argv[1] === "runtime-arg-2", "argv[1] mismatch");
console.log("argv", JSON.stringify(wl2.runtime.argv));

// Environment access is disabled by default, so env() must reject any name.
let envBlocked = false;
try {
  wl2.runtime.env("PATH");
} catch (error) {
  envBlocked = true;
}
assert(envBlocked, "wl2.runtime.env() was permitted while environment access is disabled");

// env() with no argument is a usage error regardless of policy.
let envArgError = false;
try {
  wl2.runtime.env();
} catch (error) {
  envArgError = true;
}
assert(envArgError, "wl2.runtime.env() without a name did not throw");

assert(typeof wl2.runtime.now === "function", "wl2.runtime.now is missing");
const t0 = wl2.runtime.now();
const t1 = wl2.runtime.now();
assert(Number.isFinite(t0) && Number.isFinite(t1), "wl2.runtime.now should return finite milliseconds");
assert(t1 >= t0, "wl2.runtime.now should be monotonic");

console.log("wl2_runtime smoke test passed");
