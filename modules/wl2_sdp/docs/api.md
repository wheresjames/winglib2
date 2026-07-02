# wl2:sdp

`wl2:sdp` is a dependency-free SDP (RFC 4566 / 8866) parser and builder. Its
representation is a **fidelity-preserving line model**: session-level and
media-level lines are stored raw and in order, so `build(parse(x))` reproduces
`x` byte-for-byte (including duplicate `a=` lines, unknown attributes, and the
original line endings). That exact round-trip is what makes SDP *munging* —
codec reordering, ICE stripping, bandwidth caps — safe to feed back into a real
WebRTC/SIP stack.

Typed accessors (`rtpmaps`, `candidates`, `fingerprint`, …) are read-only
*views* computed on demand over the stored lines; they never own or reorder the
data. The module has no third-party dependencies (no JSON library), matching the
`wl2:media` policy.

## Concepts

A **section** is either the session object or one media object. Both carry a
`lines` array of `{ type, value }`, and every attribute helper accepts either.

```js
parse(text) -> {
  crlf, trailingNewline,            // line-ending fidelity flags
  lines: [ { type, value }, ... ],  // session-level lines, in order
  media: [ { kind, port, proto, formats: [...], lines: [...] } ],
  warnings: [ ... ],                // populated only in lenient mode
}
```

## JavaScript API

```js
import {
  parse, build, getAttr, getAttrs, setAttr, setFlag, addAttr, removeAttr,
  keepPayloads, reorderPayloads, rtpmaps, candidates, fingerprint, compare, errorCodes,
} from "wl2:sdp";
```

| Export | Description |
|--------|-------------|
| `parse(text, options?)` | Parse SDP into a session. Throws `SdpError` in strict mode. |
| `build(session, options?)` | Serialize a session back to SDP text. |
| `getAttr(section, key)` | First `a=key:value` value, `""` for a bare flag, `null` when absent. |
| `getAttrs(section, key)` | All values for a repeated attribute key. |
| `setAttr(section, key, value)` | Replace the first `a=key:…`, or append. |
| `setFlag(section, key)` | Set a valueless `a=key` (replace-first-or-append). |
| `addAttr(section, rawValue)` | Always append `a=<rawValue>`. |
| `removeAttr(section, key)` | Remove every `a=key`/`a=key:…`; returns the count. |
| `keepPayloads(media, pts)` | Keep only these payload types; filters `m=` and drops orphan `rtpmap`/`fmtp`/`rtcp-fb`. |
| `reorderPayloads(media, pts)` | Move these payload types to the front of the `m=` format list. |
| `rtpmaps(section)` | `[ { payload, codec, clock, channels } ]`. |
| `candidates(section)` | `[ { foundation, component, proto, priority, ip, port, type, ext } ]`. |
| `fingerprint(section)` | `{ hashFunc, value }` or `null`. |
| `compare(a, b)` | Order-normalized textual equality (test helper). |
| `errorCodes()` | Stable error code constants keyed by name. |

### Parse options

| Option | Default | Meaning |
|--------|---------|---------|
| `lenient` | `false` | Collect malformed lines into `warnings` instead of throwing. |
| `crlfOnly` | `false` | Reject bare-LF lines (default accepts LF and CRLF). |
| `maxLen` | `1048576` | Hard cap on input bytes (hostile-input guard). |
| `maxMedia` | `512` | Cap on `m=` sections. |
| `maxLines` | `100000` | Cap on total non-empty lines. |

### Build options

| Option | Default | Meaning |
|--------|---------|---------|
| `canonical` | `false` | `false` re-emits stored order (exact round-trip / munging). `true` sorts each section into RFC 4566 order and injects a required `v=0` / `s=-`. |
| `crlf` | follows the session | Force CRLF (`true`) or LF (`false`) line endings. |

### Munging example

```js
const offer = parse(remoteSdp);
for (const m of offer.media) {
  removeAttr(m, "candidate");          // strip host candidates for trickle ICE
  setAttr(m, "setup", "active");       // force DTLS role
  reorderPayloads(m, [111, 96]);       // prefer Opus / VP8
}
sendToPeer(build(offer));              // byte-faithful re-emit
```

## Errors

Errors use the shared Winglib2 module error shape with `name: "SdpError"`,
`module: "wl2_sdp"`, and a stable `code`:

| Code | Meaning |
|------|---------|
| `sdp_invalid_argument` | Missing or wrong-typed argument. |
| `sdp_parse_failed` | Malformed line in strict mode. |
| `sdp_too_large` | Input or section count exceeded a cap. |
| `sdp_no_session` | Required `v=`/`s=` missing in strict mode. |
| `sdp_build_failed` | Session cannot be serialized. |

## C++ helpers

`include/wl2_sdp/sdp.h` exposes the same surface as native C++
(`wl2::sdp::parse`, `build`, the typed views, the munging helpers, and
`equalNormalized`) returning `Result<T>` rather than throwing. Link the
`wl2_sdp_static` target to reuse them from another module.

## Design notes

The full design rationale, resolved open questions, and the fuzzing strategy are
in `WL2-SDP.md` at the repository root. Highlights:

- **Recompute, don't cache** typed views — the line model is cheap and caching
  would go stale after edits.
- **Data-channel / `application` media** is preserved raw in v1 (no typed
  helpers beyond the generic attribute accessors).
- **`compare`** is order-normalized *textual* equality, deliberately not
  protocol-level semantic equivalence.
- The parser is fuzzed via `test/fuzz/sdp_fuzz.cpp` (opt-in, `-DWL2_SDP_FUZZ=ON`).
