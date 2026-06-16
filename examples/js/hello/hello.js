function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const text = "hello from javascript";
const buffer = wl2.buffer.fromText(text);

assert(buffer.text() === text, "buffer text did not round trip");
assert(buffer.uint8Array()[0] === 104, "unexpected first byte");

console.log(`${buffer.text()} (${buffer.byteLength} bytes)`);
