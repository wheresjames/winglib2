import { setInterval, setTimeout } from "wl2:asio";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

let fired = 0;
await new Promise((resolve, reject) => {
  setTimeout(() => {
    fired += 1;
    resolve();
  }, 5);
  setTimeout(() => reject(new Error("setTimeout did not fire")), 1000);
});
assert(fired === 1, "setTimeout fired the wrong number of times");

let ticks = 0;
await new Promise((resolve, reject) => {
  const interval = setInterval(() => {
    ticks += 1;
    if (ticks === 3) {
      interval.cancel();
      resolve();
    }
  }, 5);
  setTimeout(() => reject(new Error("setInterval did not tick")), 1000);
});
assert(ticks === 3, "setInterval cancel did not stop at the expected tick");

let canceled = false;
const timer = setTimeout(() => {
  canceled = true;
}, 50);
timer.close();
await new Promise((resolve) => setTimeout(resolve, 80));
assert(canceled === false, "closed timer should not fire");

console.log("wl2_asio timers test passed");
