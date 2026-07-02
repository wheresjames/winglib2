#include "wl2/wl2.h"
#include "wl2_media/schema.h"
#include "wl2_media/wl2_media.h"

#include <iostream>
#include <string>

namespace {

namespace media = wl2::media;

int fail(const std::string& message) {
    std::cerr << "wl2_media test failed: " << message << '\n';
    return 1;
}

// ---------------------------------------------------------------------------
// Direct C++ helper tests (usable by native backends without a JS engine).
// ---------------------------------------------------------------------------

int run_cpp_tests() {
    // Time base parsing.
    int64_t num = 0;
    int64_t den = 0;
    if (!media::parseTimeBase("1/1000000000", num, den) || num != 1 || den != 1000000000) {
        return fail("parseTimeBase(ns) wrong");
    }
    if (media::parseTimeBase("1/0", num, den)) {
        return fail("parseTimeBase should reject zero denominator");
    }
    if (media::parseTimeBase("garbage", num, den)) {
        return fail("parseTimeBase should reject non-fraction");
    }

    // Timestamp conversion: milliseconds -> microseconds.
    if (media::convertTimestamp(1000, "1/1000", "1/1000000") != 1000000) {
        return fail("convertTimestamp ms->us wrong");
    }
    // One frame at 30fps expressed in nanoseconds, rounded to nearest.
    if (media::convertTimestamp(1, "1/30", "1/1000000000") != 33333333) {
        return fail("convertTimestamp frame->ns wrong");
    }
    // Malformed time base returns the value unchanged.
    if (media::convertTimestamp(42, "bad", "1/1000") != 42) {
        return fail("convertTimestamp should pass through on bad base");
    }

    // Packet metadata round-trip, including awkward sideData bytes.
    media::PacketMetadata packet;
    packet.mediaType = "video";
    packet.codec = "h264";
    packet.caps = "video/x-h264,stream-format=byte-stream";
    packet.streamFormat = "byte-stream";
    packet.alignment = "au";
    packet.track = 2;
    packet.pts = 123456789;
    packet.dts = 123456700;
    packet.duration = 33333333;
    packet.timeBase = "1/1000000000";
    packet.flags = 1;
    packet.discontinuity = true;
    packet.sideData = R"({"gstreamer":{"caps":"a\"b\n"}})";

    const std::string packetJson = media::serialize(packet);
    auto parsedPacket = media::parsePacketMetadata(packetJson);
    if (!parsedPacket) {
        return fail("parsePacketMetadata failed: " + parsedPacket.error().message());
    }
    const media::PacketMetadata& rp = parsedPacket.value();
    if (rp.mediaType != packet.mediaType || rp.codec != packet.codec || rp.caps != packet.caps
        || rp.streamFormat != packet.streamFormat || rp.alignment != packet.alignment
        || rp.track != packet.track || rp.pts != packet.pts || rp.dts != packet.dts
        || rp.duration != packet.duration || rp.timeBase != packet.timeBase
        || rp.flags != packet.flags || rp.discontinuity != packet.discontinuity
        || rp.sideData != packet.sideData) {
        return fail("packet round-trip mismatch; json=" + packetJson);
    }

    // Stream descriptor round-trip.
    media::StreamDescriptor stream;
    stream.mediaType = "audio";
    stream.codec = "aac";
    stream.caps = "audio/mpeg";
    stream.track = 1;
    auto parsedStream = media::parseStreamDescriptor(media::serialize(stream));
    if (!parsedStream) {
        return fail("parseStreamDescriptor failed: " + parsedStream.error().message());
    }
    if (parsedStream.value().mediaType != "audio" || parsedStream.value().codec != "aac"
        || parsedStream.value().track != 1) {
        return fail("stream round-trip mismatch");
    }

    // Validation rejects an unknown media type and a wrong schema major.
    media::StreamDescriptor bad = stream;
    bad.mediaType = "hologram";
    if (auto ok = media::validate(bad); ok || ok.error().code() != media::errors::UnsupportedMediaType) {
        return fail("validate should reject unknown media type");
    }
    media::PacketMetadata badSchema = packet;
    badSchema.schema = 99;
    if (auto ok = media::validate(badSchema); ok || ok.error().code() != media::errors::InvalidSchema) {
        return fail("validate should reject wrong schema");
    }

    // Parsing malformed JSON fails with a stable code.
    if (auto bogus = media::parsePacketMetadata("not json"); bogus || bogus.error().code() != media::errors::ParseFailed) {
        return fail("parsePacketMetadata should reject malformed JSON");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// JavaScript surface tests, driven through the engine.
// ---------------------------------------------------------------------------

int run_js_tests() {
    wl2::RuntimeOptions options;
    options.staticModules.push_back(wl2_media_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return fail("runtime initialize failed: " + init.error().message());
    }

    auto engine = wl2::createConfiguredJsEngine();

    const std::string source = R"JS(
import {
  schemaVersion,
  StreamDescriptor,
  validateStreamDescriptor,
  PacketMetadata,
  validatePacketMetadata,
  normalizePacketMetadata,
  VideoFormat,
  AudioFormat,
  backpressureProfiles,
  errorCodes,
  Timestamp,
} from "wl2:media";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const versions = schemaVersion();
assert(versions.packet === 1 && versions.stream === 1, "schemaVersion wrong");

// Stream descriptor validation normalizes and fills defaults.
const stream = validateStreamDescriptor({ mediaType: "video", codec: "h264", track: 0 });
assert(stream.schema === 1 && stream.mediaType === "video" && stream.codec === "h264", "stream normalize wrong");
assert(StreamDescriptor({ mediaType: "audio" }).mediaType === "audio", "StreamDescriptor alias wrong");

// Unknown media type throws a MediaError with a stable code.
let streamErr = null;
try { validateStreamDescriptor({ mediaType: "hologram" }); } catch (e) { streamErr = e; }
assert(streamErr && streamErr.code === errorCodes().UNSUPPORTED_MEDIA_TYPE, "bad media type should throw");
assert(streamErr.name === "MediaError" && streamErr.module === "wl2_media", "error shape wrong");

// Packet metadata fills defaults: schema, timeBase (ns), zeroed timing.
const packet = normalizePacketMetadata({ mediaType: "video", codec: "h264" });
assert(packet.schema === 1, "packet schema default wrong");
assert(packet.timeBase === "1/1000000000", "packet timeBase default wrong: " + packet.timeBase);
assert(packet.pts === 0 && packet.dts === 0 && packet.discontinuity === false, "packet timing defaults wrong");

// validatePacketMetadata rejects a bad time base.
let tbErr = null;
try { validatePacketMetadata({ mediaType: "video", timeBase: "oops" }); } catch (e) { tbErr = e; }
assert(tbErr && tbErr.code === errorCodes().INVALID_TIME_BASE, "bad timeBase should throw");

// Timestamp conversion.
assert(Timestamp.convert(1000, "1/1000", "1/1000000") === 1000000, "Timestamp.convert wrong");
let convErr = null;
try { Timestamp.convert(1, "bad", "1/1000"); } catch (e) { convErr = e; }
assert(convErr && convErr.code === errorCodes().INVALID_TIME_BASE, "bad convert base should throw");

// Format helpers.
const vf = VideoFormat({ format: "RGBA", width: 640, height: 480, framerate: "30/1" });
assert(vf.format === "RGBA" && vf.width === 640 && vf.height === 480, "VideoFormat wrong");
const af = AudioFormat({ format: "S16LE", rate: 48000, channels: 2, layout: "interleaved" });
assert(af.rate === 48000 && af.channels === 2, "AudioFormat wrong");
let vfErr = null;
try { VideoFormat({ width: 640 }); } catch (e) { vfErr = e; }
assert(vfErr && vfErr.code === errorCodes().INVALID_ARGUMENT, "VideoFormat should require format");

// Backpressure profile names.
const profiles = backpressureProfiles();
for (const name of ["record", "transcode", "preview", "relay"]) {
  assert(profiles.includes(name), "missing backpressure profile " + name);
}
)JS";

    auto result = engine->runModule(runtime, "wl2-media-test.js", source);
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
    if (int rc = run_js_tests(); rc != 0) {
        return rc;
    }
    std::cout << "wl2_media ok\n";
    return 0;
}
