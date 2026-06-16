// Standalone wl2:fs example.
//
// The generated executable enables RuntimeOptions.allowFilesystemReads and
// confines reads to this example's data directory. Script loading from the
// filesystem stays disabled; the script itself runs from an embedded resource.
// Paths below are relative to the read root (the test runs from data/).

import { readText, exists, stat, list, walk } from "wl2:fs";

const knownFile = "sample.txt";

console.log("exists(sample.txt):", exists(knownFile));

const info = stat(knownFile);
console.log("stat(sample.txt):", JSON.stringify({
  isFile: info.isFile,
  isDirectory: info.isDirectory,
  size: info.size
}));

console.log("readText(sample.txt) first line:", readText(knownFile).split("\n")[0]);

console.log("list('.'):");
for (const entry of list(".")) {
  const kind = entry.isDirectory ? "dir" : `${entry.size} bytes`;
  console.log(`  - ${entry.name} (${kind})`);
}

console.log("walk('.') entries:", walk(".").map((e) => e.path).join(", "));

// Reads outside the configured root are rejected by runtime policy.
let denied = false;
try {
  readText("/etc/hostname");
} catch (error) {
  denied = error.code === "fs_permission_denied";
}
console.log("read outside root denied:", denied);

console.log("fs-inspect example done");
