import { parse, stringify, canonicalize, validate } from "wl2:yml";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// Scalar typing follows the YAML 1.2 core schema.
const parsed = parse(
  "name: demo\ncount: 3\nratio: 3.14\nflag: true\nempty: ~\nitems:\n  - a\n  - 2\n  - false"
);
assert(parsed.name === "demo", "string scalar did not parse");
assert(parsed.count === 3, "int scalar did not parse");
assert(parsed.ratio === 3.14, "float scalar did not parse");
assert(parsed.flag === true, "bool scalar did not parse");
assert(parsed.empty === null, "null scalar did not parse");
assert(
  Array.isArray(parsed.items) &&
    parsed.items[0] === "a" &&
    parsed.items[1] === 2 &&
    parsed.items[2] === false,
  "sequence did not parse"
);

// Round-trip structure and types through stringify -> parse.
const value = {
  name: "demo",
  count: 3,
  nested: { z: 1, a: 2 },
  list: [1, "two", false, null],
};
const round = parse(stringify(value));
assert(round.name === "demo" && round.count === 3, "round-trip lost scalars");
assert(round.nested.z === 1 && round.nested.a === 2, "round-trip lost nested map");
assert(
  round.list[0] === 1 &&
    round.list[1] === "two" &&
    round.list[2] === false &&
    round.list[3] === null,
  "round-trip lost sequence"
);

// Strings that look like other scalars must stay strings after a round-trip.
const typed = parse(stringify({ s: "123", b: "true", n: "null" }));
assert(typeof typed.s === "string" && typed.s === "123", "numeric string was retyped");
assert(typeof typed.b === "string" && typed.b === "true", "boolean string was retyped");
assert(typeof typed.n === "string" && typed.n === "null", "null string was retyped");

// Key insertion order is preserved by default.
const ordered = stringify({ z: 1, a: 2 });
assert(ordered.indexOf("z:") < ordered.indexOf("a:"), "default order not preserved");

// sortKeys sorts map keys.
const sorted = stringify({ z: 1, a: 2 }, { sortKeys: true });
assert(sorted.indexOf("a:") < sorted.indexOf("z:"), "sortKeys did not sort");

// flow emits compact inline collections.
const flow = stringify({ a: [1, 2] }, { flow: true });
assert(flow.includes("{") && flow.includes("["), "flow style was not emitted");

// canonicalize is deterministic regardless of input key order.
const c1 = canonicalize({ z: 1, a: { c: 3, b: 2 } });
const c2 = canonicalize({ a: { b: 2, c: 3 }, z: 1 });
assert(c1 === c2, `canonicalize not deterministic:\n${c1}\n---\n${c2}`);

// validate reports success and structured failures.
assert(validate("a: 1\nb: 2").ok === true, "valid YAML was rejected");
const invalid = validate("a: [1, 2");
assert(
  invalid.ok === false && invalid.line >= 1 && invalid.column >= 1,
  "invalid YAML result is incomplete"
);

// Unsupported JavaScript values are rejected during serialization.
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

console.log("wl2:yml ok");
