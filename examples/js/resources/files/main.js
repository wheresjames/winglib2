// Standalone JavaScript resources example.
//
// This mirrors examples/cpp/resources, but drives the embedded resource tree
// from JavaScript through `wl2.resources` instead of the C++ ResourceStore. The
// resources (and this script) are embedded at build time under the wl2:/resources
// prefix by wl2_add_javascript_executable(); no filesystem access is used.

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

// Stored resources can be read as text or opened as a handle over their bytes.
const config = JSON.parse(wl2.resources.readText("wl2:/resources/config.json"));
assert(config.name === "winglib2", "config.json did not round-trip");
console.log("config:", JSON.stringify(config));

const handle = wl2.resources.open("wl2:/resources/config.json");
try {
  console.log("config bytes:", handle.size, "compressed:", handle.compressed);
  assert(handle.size > 0, "config handle should report a non-zero size");
} finally {
  handle.close();
}

// Compressed resources expose the same API as stored ones; the content is
// transparently inflated on read.
const repeated = wl2.resources.open("wl2:/resources/repeated.txt");
try {
  console.log("repeated.txt compressed:", repeated.compressed);
  assert(repeated.compressed === true, "repeated.txt should be stored compressed");
} finally {
  repeated.close();
}
console.log("repeated text:", wl2.resources.readText("wl2:/resources/repeated.txt").trim());

// readBytes returns a wl2.Buffer.
const bytes = wl2.resources.readBytes("wl2:/resources/config.json");
assert(wl2.buffer.isBuffer(bytes), "readBytes should return a wl2.Buffer");
console.log("config byteLength:", bytes.byteLength);

// Directories embed recursively; paths stay relative to the resource root.
const nested = wl2.resources.readText("wl2:/resources/assets/nested/info.txt");
console.log("nested text:", nested.trim());

// Excluded patterns (*.tmp) are not embedded.
assert(wl2.resources.exists("wl2:/resources/assets/skip.tmp") === false,
  "excluded resource should not be embedded");

// Empty files embed as a valid zero-length resource.
const empty = wl2.resources.open("wl2:/resources/empty.txt");
try {
  assert(empty.size === 0, "empty.txt should be zero length");
  console.log("empty bytes:", empty.size);
} finally {
  empty.close();
}

// stat() reports type without reading the content.
const rootStat = wl2.resources.stat("wl2:/resources/assets");
console.log("assets is directory:", rootStat.directory);

// list() returns direct children only.
console.log("wl2:/resources direct children:");
for (const entry of wl2.resources.list("wl2:/resources")) {
  console.log(`  ${entry.directory ? "dir " : "file"} ${entry.path}`);
}

console.log("assets direct children:");
for (const entry of wl2.resources.list("wl2:/resources/assets")) {
  console.log(`  ${entry.directory ? "dir " : "file"} ${entry.path}`);
}

// walk() enumerates every file below a directory.
console.log("assets recursive files:");
for (const entry of wl2.resources.walk("wl2:/resources/assets")) {
  console.log(`  file ${entry.path} (${entry.size} bytes)`);
}

console.log("resources example done");
