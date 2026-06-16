function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const a = wl2.buffer.fromText("hello");
assert(wl2.buffer.isBuffer(a), "fromText did not return a wl2.Buffer");
assert(a.byteLength === 5, `unexpected byteLength: ${a.byteLength}`);
assert(a.size === 5, `unexpected size: ${a.size}`);
assert(a.text() === "hello", `unexpected text: ${a.text()}`);
assert(String(a) === "hello", "toString should return text");

const b = a.slice(1, 3);
assert(wl2.buffer.isBuffer(b), "slice did not return a wl2.Buffer");
assert(b.byteLength === 3, `unexpected slice byteLength: ${b.byteLength}`);
assert(b.text() === "ell", `unexpected slice text: ${b.text()}`);

const bytes = a.uint8Array();
assert(bytes instanceof Uint8Array, "uint8Array did not return Uint8Array");
assert(bytes.length === 5, `unexpected Uint8Array length: ${bytes.length}`);
assert(bytes[0] === 104 && bytes[4] === 111, "unexpected Uint8Array bytes");

const arrayBuffer = a.arrayBuffer();
assert(arrayBuffer instanceof ArrayBuffer, "arrayBuffer did not return ArrayBuffer");
assert(arrayBuffer.byteLength === 5, `unexpected ArrayBuffer length: ${arrayBuffer.byteLength}`);

const c = wl2.buffer.fromArrayBuffer(new Uint8Array([65, 66, 67]).buffer);
assert(c.text() === "ABC", `unexpected fromArrayBuffer text: ${c.text()}`);

const d = new wl2.Buffer("ctor");
assert(wl2.buffer.isBuffer(d), "constructor did not return a wl2.Buffer");
assert(d.text() === "ctor", `unexpected constructor text: ${d.text()}`);

console.log("wl2.Buffer smoke test passed");
