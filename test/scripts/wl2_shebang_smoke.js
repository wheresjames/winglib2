#!/usr/bin/env -S wl2 run

if (!Array.isArray(wl2.runtime.argv)) {
  throw new Error("argv was not available");
}

console.log("shebang smoke passed");
