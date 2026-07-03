import { parse, stringify, canonicalize, validate } from "wl2:json";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const parsed = parse('{"b":2,"a":[true,null,"x"]}');
assert(parsed.b === 2, "number did not parse");
assert(parsed.a[0] === true && parsed.a[1] === null && parsed.a[2] === "x", "array did not parse");

const compact = stringify({ z: 1, a: "x" });
assert(compact === '{"z":1,"a":"x"}', `unexpected compact JSON: ${compact}`);

const sorted = stringify({ z: 1, a: "x" }, { sortKeys: true });
assert(sorted === '{"a":"x","z":1}', `unexpected sorted JSON: ${sorted}`);

const pretty = stringify({ a: [1, 2] }, { pretty: true, indent: 2 });
assert(pretty.includes('\n  "a"'), "pretty JSON did not include indentation");

const stable = canonicalize({ z: 1, a: { c: 3, b: 2 } });
assert(stable === '{"a":{"b":2,"c":3},"z":1}', `unexpected canonical JSON: ${stable}`);

assert(validate('{"ok":true}').ok === true, "valid JSON was rejected");
const invalid = validate('{"bad":');
assert(invalid.ok === false && invalid.line >= 1 && invalid.column >= 1, "invalid JSON result is incomplete");

let rejected = false;
try {
  stringify({ nope: undefined });
} catch (error) {
  rejected = true;
}
assert(rejected, "undefined should be rejected");

const cycle = {};
cycle.self = cycle;
rejected = false;
try {
  stringify(cycle);
} catch (error) {
  rejected = true;
}
assert(rejected, "cycles should be rejected");

console.log("wl2:json ok");
