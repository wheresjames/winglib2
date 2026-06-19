import { Selector } from "wl2:membus";

let ready = false;
if (Selector.wait([() => ready], { timeoutMs: 0 }) !== -1) {
  throw new Error("selector should time out before predicate is ready");
}

ready = true;
const index = Selector.wait([() => false, () => ready], { timeoutMs: 1000 });
if (index !== 1) {
  throw new Error(`unexpected ready index: ${index}`);
}

console.log(`ready:${index}`);
