#include "wl2/js_engine.h"
#include "wl2/wl2.h"
#include "wl2_sdp/sdp.h"
#include "wl2_sdp/wl2_sdp.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace sdp = wl2::sdp;

int fail(const std::string& message) {
    std::cerr << "wl2_sdp test failed: " << message << '\n';
    return 1;
}

// ---------------------------------------------------------------------------
// C++ helper tests (usable without a JS engine).
// ---------------------------------------------------------------------------

int run_cpp_tests() {
    const std::string offer =
        "v=0\n"
        "o=- 1 2 IN IP4 127.0.0.1\n"
        "s=-\n"
        "t=0 0\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 103 104\n"
        "a=rtpmap:111 opus/48000/2\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\n"
        "a=rtpmap:103 ISAC/16000\n"
        "a=rtpmap:104 ISAC/32000\n"
        "a=fingerprint:sha-256 AA:BB:CC\n"
        "a=ice-ufrag:F7gr\n"
        "a=candidate:1 1 UDP 2130706431 192.0.2.1 54321 typ host\n"
        "a=candidate:2 1 UDP 1694498815 198.51.100.7 54323 typ srflx raddr 192.0.2.1 rport 54321\n";

    // Parse structure.
    auto parsed = sdp::parse(offer);
    if (!parsed) {
        return fail("parse failed: " + parsed.error().message());
    }
    sdp::Session session = parsed.value();
    if (session.media.size() != 1) {
        return fail("expected 1 media section");
    }
    const sdp::Media& audio = session.media[0];
    if (audio.kind != "audio" || audio.proto != "UDP/TLS/RTP/SAVPF" || audio.formats.size() != 3) {
        return fail("m= line parse wrong");
    }
    if (audio.formats[0] != "111" || audio.formats[2] != "104") {
        return fail("format list wrong");
    }

    // Round-trip (LF, trailing newline) must be byte-exact.
    if (sdp::build(session) != offer) {
        return fail("LF round-trip mismatch:\n[" + sdp::build(session) + "]");
    }

    // Round-trip CRLF.
    std::string crlf;
    {
        std::string tmp = offer;
        for (char c : tmp) {
            if (c == '\n') {
                crlf.push_back('\r');
            }
            crlf.push_back(c);
        }
    }
    auto parsedCrlf = sdp::parse(crlf);
    if (!parsedCrlf || !parsedCrlf.value().crlf) {
        return fail("CRLF not detected");
    }
    if (sdp::build(parsedCrlf.value()) != crlf) {
        return fail("CRLF round-trip mismatch");
    }

    // No trailing newline round-trip.
    std::string noTrailer = offer;
    noTrailer.pop_back(); // drop final '\n'
    auto parsedNoTrailer = sdp::parse(noTrailer);
    if (!parsedNoTrailer || parsedNoTrailer.value().trailingNewline) {
        return fail("trailing newline should be absent");
    }
    if (sdp::build(parsedNoTrailer.value()) != noTrailer) {
        return fail("no-trailer round-trip mismatch");
    }

    // Typed views: rtpmap.
    auto maps = sdp::rtpmaps(audio);
    if (maps.size() != 3 || maps[0].payload != 111 || maps[0].codec != "opus"
        || maps[0].clock != 48000 || maps[0].channels != 2) {
        return fail("rtpmap parse wrong");
    }
    if (maps[1].codec != "ISAC" || maps[1].clock != 16000 || maps[1].channels != 0) {
        return fail("rtpmap channels default wrong");
    }

    // Typed views: candidate with extension pairs.
    auto cands = sdp::candidates(audio);
    if (cands.size() != 2 || cands[0].type != "host" || cands[0].port != 54321
        || cands[0].priority != 2130706431LL) {
        return fail("candidate parse wrong");
    }
    if (cands[1].type != "srflx" || cands[1].ext.size() != 2 || cands[1].ext[0].first != "raddr"
        || cands[1].ext[1].first != "rport" || cands[1].ext[1].second != "54321") {
        return fail("candidate ext parse wrong");
    }

    // Typed views: fingerprint + attribute + flag vs value.
    auto fp = sdp::fingerprint(audio);
    if (!fp || fp->hashFunc != "sha-256" || fp->value != "AA:BB:CC") {
        return fail("fingerprint parse wrong");
    }
    auto ufrag = sdp::attribute(audio, "ice-ufrag");
    if (!ufrag || *ufrag != "F7gr") {
        return fail("attribute(ice-ufrag) wrong");
    }
    if (sdp::attribute(audio, "nonexistent")) {
        return fail("absent attribute should be nullopt");
    }

    // Munging: setAttribute replace-first, then append when absent.
    sdp::Media m = audio;
    sdp::setAttribute(m, "ice-ufrag", "ZZZZ");
    if (sdp::attribute(m, "ice-ufrag").value_or("") != "ZZZZ") {
        return fail("setAttribute replace failed");
    }
    sdp::setAttribute(m, "setup", "active");
    if (sdp::attribute(m, "setup").value_or("") != "active") {
        return fail("setAttribute append failed");
    }
    // Flag.
    sdp::setFlag(m, "rtcp-mux");
    if (sdp::attribute(m, "rtcp-mux").value_or("MISSING") != "") {
        return fail("setFlag failed");
    }

    // Munging: removeAttribute count.
    if (sdp::removeAttribute(m, "candidate") != 2) {
        return fail("removeAttribute count wrong");
    }
    if (!sdp::candidates(m).empty()) {
        return fail("candidates not removed");
    }

    // Munging: keepPayloads filters m= and drops orphan rtpmap/fmtp.
    sdp::Media keep = audio;
    sdp::keepPayloads(keep, {111});
    if (keep.formats.size() != 1 || keep.formats[0] != "111") {
        return fail("keepPayloads format filter wrong");
    }
    if (sdp::rtpmaps(keep).size() != 1) {
        return fail("keepPayloads did not drop orphan rtpmaps");
    }
    if (sdp::fmtps(keep).size() != 1) {
        return fail("keepPayloads dropped the kept fmtp");
    }

    // Munging: reorderPayloads.
    sdp::Media reorder = audio;
    sdp::reorderPayloads(reorder, {104, 111});
    if (reorder.formats.size() != 3 || reorder.formats[0] != "104" || reorder.formats[1] != "111"
        || reorder.formats[2] != "103") {
        return fail("reorderPayloads wrong");
    }

    // Canonical build injects v=0 and s=- and reorders.
    sdp::Session bare;
    bare.lines.push_back(sdp::Line{'t', "0 0"});
    bare.lines.push_back(sdp::Line{'o', "- 1 2 IN IP4 127.0.0.1"});
    sdp::BuildOptions canonical;
    canonical.canonical = true;
    canonical.ending = sdp::LineEnding::Crlf;
    std::string canon = sdp::build(bare, canonical);
    if (canon.rfind("v=0\r\no=", 0) != 0) {
        return fail("canonical did not inject v=0 first: [" + canon + "]");
    }
    if (canon.find("s=-\r\n") == std::string::npos) {
        return fail("canonical did not inject s=-");
    }

    // Lenient mode collects a malformed line; strict rejects it.
    const std::string dirty = offer + "garbage line without equals\n";
    if (auto strict = sdp::parse(dirty); strict || strict.error().code() != sdp::errors::ParseFailed) {
        return fail("strict parse should reject malformed line");
    }
    sdp::ParseOptions lenient;
    lenient.lenient = true;
    auto lenientParsed = sdp::parse(dirty, lenient);
    if (!lenientParsed || lenientParsed.value().warnings.size() != 1) {
        return fail("lenient parse should collect one warning");
    }

    // Strict missing v=/s= -> NoSession.
    if (auto ns = sdp::parse("o=- 1 2 IN IP4 127.0.0.1\nt=0 0\n");
        ns || ns.error().code() != sdp::errors::NoSession) {
        return fail("strict parse should require v=/s=");
    }

    // Hostile-input caps.
    sdp::ParseOptions tiny;
    tiny.maxLen = 4;
    if (auto big = sdp::parse(offer, tiny); big || big.error().code() != sdp::errors::TooLarge) {
        return fail("maxLen cap should trigger TooLarge");
    }
    sdp::ParseOptions fewMedia;
    fewMedia.maxMedia = 0;
    if (auto mm = sdp::parse(offer, fewMedia); mm || mm.error().code() != sdp::errors::TooLarge) {
        return fail("maxMedia cap should trigger TooLarge");
    }

    // equalNormalized: order-insensitive within a scope, sensitive to content.
    sdp::Session a = parsed.value();
    sdp::Session b = parsed.value();
    std::swap(b.media[0].lines.front(), b.media[0].lines.back());
    if (!sdp::equalNormalized(a, b)) {
        return fail("equalNormalized should ignore line order");
    }
    b.media[0].lines.push_back(sdp::Line{'a', "extra:1"});
    if (sdp::equalNormalized(a, b)) {
        return fail("equalNormalized should detect added line");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Corpus round-trip: every .sdp fixture must round-trip byte-exactly.
// ---------------------------------------------------------------------------

int run_corpus_tests() {
    const std::filesystem::path dir{WL2_SDP_TEST_DATA_DIR};
    if (!std::filesystem::is_directory(dir)) {
        return fail(std::string("data dir missing: ") + WL2_SDP_TEST_DATA_DIR);
    }
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".sdp") {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        const std::string content = buf.str();

        auto parsed = sdp::parse(content);
        if (!parsed) {
            return fail("corpus parse failed for " + entry.path().filename().string() + ": "
                + parsed.error().message());
        }
        const std::string rebuilt = sdp::build(parsed.value());
        if (rebuilt != content) {
            return fail("corpus round-trip mismatch for " + entry.path().filename().string());
        }
        ++count;
    }
    if (count == 0) {
        return fail("no corpus fixtures found");
    }
    std::cout << "wl2_sdp corpus: " << count << " fixtures round-tripped\n";
    return 0;
}

// ---------------------------------------------------------------------------
// JavaScript surface tests, driven through the engine.
// ---------------------------------------------------------------------------

int run_js_tests() {
    wl2::RuntimeOptions options;
    options.staticModules.push_back(wl2_sdp_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return fail("runtime initialize failed: " + init.error().message());
    }

    auto engine = wl2::createConfiguredJsEngine();

    const std::string source = R"JS(
import {
  parse, build, getAttr, getAttrs, setAttr, setFlag, addAttr, removeAttr,
  keepPayloads, reorderPayloads, rtpmaps, candidates, fingerprint, compare, errorCodes,
} from "wl2:sdp";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const offer =
  "v=0\n" +
  "o=- 1 2 IN IP4 127.0.0.1\n" +
  "s=-\n" +
  "t=0 0\n" +
  "m=audio 9 UDP/TLS/RTP/SAVPF 111 103\n" +
  "a=ice-ufrag:F7gr\n" +
  "a=rtpmap:111 opus/48000/2\n" +
  "a=rtpmap:103 ISAC/16000\n" +
  "a=candidate:1 1 UDP 2130706431 192.0.2.1 54321 typ host\n";

const s = parse(offer);
assert(s.media.length === 1, "expected one media section");
assert(s.media[0].kind === "audio", "kind wrong");

// Round-trip through JS is byte-exact.
assert(build(s) === offer, "JS round-trip mismatch");

// Attribute helpers work on a media section.
const audio = s.media[0];
assert(getAttr(audio, "ice-ufrag") === "F7gr", "getAttr wrong");
assert(getAttr(audio, "missing") === null, "absent getAttr should be null");

setAttr(audio, "ice-ufrag", "ZZZZ");
assert(getAttr(audio, "ice-ufrag") === "ZZZZ", "setAttr replace failed");
setAttr(audio, "setup", "active");
assert(getAttr(audio, "setup") === "active", "setAttr append failed");
setFlag(audio, "rtcp-mux");
assert(getAttr(audio, "rtcp-mux") === "", "setFlag should read as empty string");

// rtpmaps / candidates views.
const maps = rtpmaps(audio);
assert(maps.length === 2 && maps[0].codec === "opus" && maps[0].channels === 2, "rtpmaps wrong");
const cands = candidates(audio);
assert(cands.length === 1 && cands[0].type === "host" && cands[0].port === 54321, "candidates wrong");

// removeAttr returns the count.
assert(removeAttr(audio, "candidate") === 1, "removeAttr count wrong");
assert(candidates(audio).length === 0, "candidate not removed");

// keepPayloads drops orphaned rtpmap and filters the format list.
keepPayloads(audio, [111]);
assert(audio.formats.length === 1 && audio.formats[0] === "111", "keepPayloads format wrong");
assert(rtpmaps(audio).length === 1, "keepPayloads orphan drop wrong");

// reorderPayloads on a fresh parse.
const s2 = parse(offer);
reorderPayloads(s2.media[0], [103, 111]);
assert(s2.media[0].formats[0] === "103" && s2.media[0].formats[1] === "111", "reorderPayloads wrong");

// fingerprint falls back to null when absent here.
assert(fingerprint(audio) === null, "fingerprint should be null");

// compare: order-normalized equality.
const a = parse(offer);
const b = parse(offer);
const tmp = b.media[0].lines[0];
b.media[0].lines[0] = b.media[0].lines[1];
b.media[0].lines[1] = tmp;
assert(compare(a, b) === true, "compare should ignore line order");

// Errors: malformed strict parse throws SdpError with a stable code.
let err = null;
try { parse("not an sdp line\n"); } catch (e) { err = e; }
assert(err && err.name === "SdpError", "expected SdpError");
assert(err.code === errorCodes().PARSE_FAILED, "expected PARSE_FAILED, got " + (err && err.code));

// Lenient mode collects warnings instead of throwing.
const lenient = parse("v=0\ns=-\nbroken line\n", { lenient: true });
assert(lenient.warnings.length === 1, "lenient should collect one warning");

// Canonical build injects and orders.
const canon = build({ lines: [ { type: "t", value: "0 0" } ], media: [] }, { canonical: true, crlf: true });
assert(canon.indexOf("v=0\r\n") === 0, "canonical should start with v=0");
)JS";

    auto result = engine->runModule(runtime, "wl2-sdp-test.js", source);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    return 0;
}

} // namespace

int main() {
    if (int rc = run_cpp_tests(); rc != 0) {
        return rc;
    }
    if (int rc = run_corpus_tests(); rc != 0) {
        return rc;
    }
    if (int rc = run_js_tests(); rc != 0) {
        return rc;
    }
    std::cout << "wl2_sdp ok\n";
    return 0;
}
