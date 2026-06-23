#include "wl2/wl2.h"
#include "wl2_ffmpeg/wl2_ffmpeg.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_ffmpeg test failed: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    const auto fixturePath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-phase2-" + std::to_string(getpid()) + ".avi");
    const auto transcodePath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-transcode-" + std::to_string(getpid()) + ".avi");
    const auto remuxPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-remux-" + std::to_string(getpid()) + ".avi");
    const auto queueRemuxPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-queue-" + std::to_string(getpid()) + ".avi");
    const auto queueDropPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-queue-drop-" + std::to_string(getpid()) + ".avi");
    const auto irregularPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-irregular-" + std::to_string(getpid()) + ".ts");
    const auto mjpegPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-mjpeg-" + std::to_string(getpid()) + ".avi");
    const auto stillPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-still-" + std::to_string(getpid()) + ".png");
    const auto clipCopyPath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-clip-copy-" + std::to_string(getpid()) + ".avi");
    const auto clipTranscodePath = std::filesystem::temp_directory_path()
        / ("wl2-ffmpeg-clip-transcode-" + std::to_string(getpid()) + ".mp4");
    const std::string videoBufferName = "/wl2_ffmpeg_phase2_" + std::to_string(getpid());
    const std::string audioBufferName = "/wl2_ffmpeg_audio_" + std::to_string(getpid());
    const std::string packetQueueName = "/wl2_ffmpeg_packet_" + std::to_string(getpid());
    const std::string queueStreamName = "/wl2_ffmpeg_stream_" + std::to_string(getpid());
    const std::string queueDropName = "/wl2_ffmpeg_drop_" + std::to_string(getpid());
    const std::string profileQueueName = "/wl2_ffmpeg_profile_" + std::to_string(getpid());

    wl2::RuntimeOptions options;
    options.staticModules.push_back(wl2_ffmpeg_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return fail("runtime initialize failed: " + init.error().message());
    }

    auto engine = wl2::createConfiguredJsEngine();
    std::ostringstream script;
    script << R"JS(
import {
  AudioConverter,
  AudioDecoder,
  AudioEncoder,
  Demuxer,
  Evidence,
  FilterGraph,
  Muxer,
  PacketQueue,
  ReplaySession,
  VideoEncoder,
  analyzeTimestamps,
  capabilities,
  filters,
  generateSyntheticFixture,
  hardware,
  listCodecs,
  listFormats,
  profilePacketQueue,
  version
} from "wl2:ffmpeg";

const fixturePath = ")JS" << fixturePath.string() << R"JS(";
const transcodePath = ")JS" << transcodePath.string() << R"JS(";
const remuxPath = ")JS" << remuxPath.string() << R"JS(";
const queueRemuxPath = ")JS" << queueRemuxPath.string() << R"JS(";
const queueDropPath = ")JS" << queueDropPath.string() << R"JS(";
const irregularPath = ")JS" << irregularPath.string() << R"JS(";
const mjpegPath = ")JS" << mjpegPath.string() << R"JS(";
const stillPath = ")JS" << stillPath.string() << R"JS(";
const clipCopyPath = ")JS" << clipCopyPath.string() << R"JS(";
const clipTranscodePath = ")JS" << clipTranscodePath.string() << R"JS(";
const videoBufferName = ")JS" << videoBufferName << R"JS(";
const audioBufferName = ")JS" << audioBufferName << R"JS(";
const packetQueueName = ")JS" << packetQueueName << R"JS(";
const queueStreamName = ")JS" << queueStreamName << R"JS(";
const queueDropName = ")JS" << queueDropName << R"JS(";
const profileQueueName = ")JS" << profileQueueName << R"JS(";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const v = version();
assert(typeof v.avformat === "string" && v.avformat.length > 0, "missing avformat version");
assert(typeof v.avcodec === "string" && v.avcodec.length > 0, "missing avcodec version");
assert(v.externalTestMediaEnabledByDefault === false, "external media should be disabled by default");

const caps = capabilities();
assert(caps.syntheticFixtureMetadata === true, "fixture metadata capability missing");
assert(caps.syntheticFixtureWriter === true, "fixture writer capability missing");
assert(caps.demuxer === true, "demuxer capability missing");
assert(caps.videoDecoder === true, "video decoder capability missing");
assert(caps.audioDecoder === true, "audio decoder capability missing");
assert(caps.audioConverter === true, "audio converter capability missing");
assert(caps.replaySession === true, "replay session capability missing");
assert(caps.audioBufferPublish === true, "audio buffer publish capability missing");
assert(caps.packetQueue === true, "packet queue capability missing");
assert(caps.externalTestMediaDefault === false, "external test media default should be false");
assert(caps.rawVideoFormats.includes("bgra"), "required video format bgra missing");
assert(caps.rawAudioFormats.includes("s16"), "required audio format s16 missing");

const codecs = listCodecs({ type: "video" });
assert(Array.isArray(codecs), "listCodecs should return an array");
assert(codecs.length > 0, "no video codecs reported");
assert(codecs.some((codec) => codec.decoder || codec.encoder), "codec direction flags missing");

const formats = listFormats();
assert(Array.isArray(formats), "listFormats should return an array");
assert(formats.some((format) => format.demuxer), "no demuxers reported");
assert(formats.some((format) => format.muxer), "no muxers reported");

const fixture = generateSyntheticFixture({
  name: "phase2",
  path: fixturePath,
  fps: 30,
  durationSeconds: 2,
  width: 160,
  height: 90,
  gop: 15,
  tone: true
});
assert(fixture.kind === "synthetic-archive-plan", "fixture kind mismatch");
assert(fixture.frameCount === 60, "fixture frame count mismatch: " + fixture.frameCount);
assert(fixture.burnedInClock === true, "fixture should include burned-in clock");
assert(fixture.toneAudio === true, "fixture should include tone flag");
assert(fixture.writesMedia === true, "phase 2 fixture should write media");
assert(fixture.status === "written", "fixture write status mismatch");

const demuxer = Demuxer.open(fixturePath);
const streams = demuxer.streams();
assert(streams.some((stream) => stream.type === "video" && stream.width === 160 && stream.height === 90),
  "generated video stream metadata missing");
assert(streams.some((stream) => stream.type === "audio" && stream.sampleRate === 48000 && stream.channels === 2),
  "generated audio stream metadata missing");
const packet = demuxer.readPacket();
assert(packet !== null, "expected first packet");
assert(packet.size > 0, "packet should contain compressed or muxed frame bytes");
assert(packet.data.size === packet.size, "packet Buffer size mismatch");
demuxer.close();

const replay = ReplaySession.open(fixturePath);
const seek = replay.seek({ timestampUs: 500000 });
assert(seek.seekId === 1, "seek id should advance");
const frame = replay.extractFrame({ timestampUs: 500000, format: "rgb24" });
assert(frame.width === 160 && frame.height === 90, "decoded frame dimensions mismatch");
assert(frame.format === "rgb24", "decoded frame format mismatch: " + frame.format);
assert(frame.size === 160 * 90 * 3, "decoded rgb24 frame size mismatch: " + frame.size);
assert(frame.data.size === frame.size, "decoded Buffer size mismatch");
assert(frame.ptsUs >= 0, "decoded frame pts should be known");
const state = replay.state();
assert(state.lastSeekId === 2, "extractFrame should advance seek id");
const published = replay.publishFrame({ videoBufferName, timestampUs: 750000, create: true, fps: 30, buffers: 3 });
assert(published.slot >= 0, "publishFrame should report a slot");
assert(published.bytes === 160 * 90 * 3, "publishFrame byte count mismatch: " + published.bytes);
assert(published.format === "rgb24", "publishFrame format mismatch: " + published.format);
assert(published.seekId === 3, "publishFrame should advance seek id");
const audio = replay.extractAudio({ timestampUs: 500000, durationUs: 100000, sampleRate: 16000, channels: 1, format: "s16" });
assert(audio.sampleRate === 16000, "audio sample rate mismatch: " + audio.sampleRate);
assert(audio.channels === 1, "audio channels mismatch: " + audio.channels);
assert(audio.format === "s16", "audio format mismatch: " + audio.format);
assert(audio.samples === 1600, "audio sample count mismatch: " + audio.samples);
assert(audio.size === 3200, "audio byte count mismatch: " + audio.size);
assert(audio.data.size === audio.size, "audio Buffer size mismatch");
const publishedAudio = replay.publishAudio({
  audioBufferName,
  timestampUs: 500000,
  durationUs: 100000,
  sampleRate: 16000,
  channels: 1,
  create: true,
  fps: 10,
  buffers: 3
});
assert(publishedAudio.bytes === 3200, "publishAudio byte count mismatch: " + publishedAudio.bytes);
assert(publishedAudio.sampleRate === 16000, "publishAudio sample rate mismatch");
assert(publishedAudio.channels === 1, "publishAudio channel count mismatch");
assert(publishedAudio.seekId === 5, "publishAudio should advance seek id");
replay.close();

const queued = PacketQueue.roundTripFirstPacket({
  path: fixturePath,
  queueName: packetQueueName,
  queueSize: 262144
});
assert(queued.magic === "WL2FQP1", "packet queue magic mismatch");
assert(queued.version === 1, "packet queue version mismatch");
assert(queued.packetSize === queued.sourcePacketSize, "packet queue packet size mismatch");
assert(queued.streamIndex === queued.sourceStreamIndex, "packet queue stream index mismatch");
assert(queued.recordSize > queued.packetSize, "packet record should include a header");

// Phase 3: standalone audio decode reports source planar/interleaved metadata.
assert(caps.videoEncoder === true, "video encoder capability missing");
assert(caps.audioEncoder === true, "audio encoder capability missing");
assert(caps.muxer === true, "muxer capability missing");
assert(caps.transcode === true, "transcode capability missing");
assert(caps.remux === true, "remux capability missing");
assert(caps.packetQueueStream === true, "packet queue stream capability missing");
assert(caps.avSync === true, "A/V sync capability missing");

assert(VideoEncoder.presets().presets.includes("low-latency"), "video encoder low-latency preset missing");
assert(AudioEncoder.presets().presets.includes("archival"), "audio encoder archival preset missing");

const decodedAudio = AudioDecoder.decode({
  path: fixturePath,
  timestampUs: 500000,
  durationUs: 100000,
  sampleRate: 16000,
  channels: 1,
  format: "s16"
});
assert(decodedAudio.samples === 1600, "standalone audio decode sample count mismatch: " + decodedAudio.samples);
assert(decodedAudio.sourceSampleRate === 48000, "audio decode source rate mismatch: " + decodedAudio.sourceSampleRate);
assert(decodedAudio.sourceChannels === 2, "audio decode source channels mismatch: " + decodedAudio.sourceChannels);
assert(typeof decodedAudio.sourceFormat === "string", "audio decode source format missing");
assert(typeof decodedAudio.sourcePlanar === "boolean", "audio decode planar flag missing");

// AudioConverter: convert s16 stereo to flt mono and verify format/layout/rate.
const converted = AudioConverter.convert({
  data: decodedAudio.data,
  inFormat: "s16",
  inSampleRate: 16000,
  inChannels: 1,
  outFormat: "flt",
  outSampleRate: 8000,
  outChannels: 1
});
assert(converted.format === "flt", "audio convert format mismatch: " + converted.format);
assert(converted.sampleRate === 8000, "audio convert rate mismatch: " + converted.sampleRate);
assert(converted.channels === 1, "audio convert channels mismatch: " + converted.channels);
assert(converted.samples > 0, "audio convert produced no samples");
assert(converted.size === converted.samples * 1 * 4, "audio convert byte count mismatch: " + converted.size);
assert(converted.data.size === converted.size, "audio convert buffer size mismatch");

// Muxer.transcode: decode raw fixture, encode video+audio, mux, and reopen.
const transcoded = Muxer.transcode({
  path: fixturePath,
  outputPath: transcodePath,
  preset: "low-latency",
  audio: true
});
assert(transcoded.videoCodec.length > 0, "transcode should report a video codec");
assert(transcoded.videoPackets > 0, "transcode produced no video packets");
assert(transcoded.width === 160 && transcoded.height === 90, "transcode dimensions mismatch");
assert(transcoded.reopened === true, "transcoded output should reopen");
assert(transcoded.reopenedWidth === 160 && transcoded.reopenedHeight === 90, "transcode reopen dimensions mismatch");
assert(transcoded.preset === "low-latency", "transcode preset mismatch: " + transcoded.preset);
if (transcoded.audioCodec.length > 0) {
  assert(transcoded.audioPackets > 0, "transcode reported audio codec but no audio packets");
  assert(transcoded.reopenedHasAudio === true, "transcoded output should reopen with audio");
}

// Muxer.remux: compressed copy preserving extradata and source timestamps.
const remuxed = Muxer.remux({ path: fixturePath, outputPath: remuxPath });
assert(remuxed.streams >= 1, "remux should map at least one stream");
assert(remuxed.packets > 0, "remux copied no packets");
assert(remuxed.extradataPreserved === true, "remux should preserve extradata size");
assert(remuxed.firstVideoPts === 0, "remux should preserve first video pts: " + remuxed.firstVideoPts);

// PacketQueue.streamThroughQueue lossless: payload, timestamps, extradata preserved.
const lossless = PacketQueue.streamThroughQueue({
  path: fixturePath,
  outputPath: queueRemuxPath,
  queueName: queueStreamName,
  policy: "lossless",
  queueSize: 1048576,
  maxDepth: 16
});
assert(lossless.policy === "lossless", "lossless policy mismatch");
assert(lossless.packetsRead > 0, "queue stream read no packets");
assert(lossless.recordsWritten === lossless.packetsRead, "lossless should not drop records");
assert(lossless.dropped === 0, "lossless policy must not drop");
assert(lossless.payloadPreserved === true, "queue stream payload not preserved");
assert(lossless.timestampsPreserved === true, "queue stream timestamps not preserved");
assert(lossless.extradataPreserved === true, "queue stream extradata not preserved");

// PacketQueue.streamThroughQueue drop policy: recovers at keyframe with discontinuity.
const dropping = PacketQueue.streamThroughQueue({
  path: fixturePath,
  outputPath: queueDropPath,
  queueName: queueDropName,
  policy: "dropOldestUntilKeyframe",
  queueSize: 1048576,
  maxDepth: 2
});
assert(dropping.policy === "dropOldestUntilKeyframe", "drop policy mismatch");
assert(dropping.packetsRead > 0, "drop stream read no packets");
assert(dropping.recordsWritten <= dropping.packetsRead, "drop policy should not write more than read");

// ReplaySession playback plan: video-master sync with muted reverse/high-speed.
const planReplay = ReplaySession.open(fixturePath);
const normalPlan = planReplay.playbackPlan({ speed: 1.0, reverse: false });
assert(normalPlan.clock === "video-master", "playback clock should be video-master");
assert(normalPlan.audioEnabled === true, "normal-speed forward playback should enable audio");
assert(normalPlan.driftBudgetUs > 0, "playback plan should report a drift budget");
const fastPlan = planReplay.playbackPlan({ speed: 4.0, reverse: false });
assert(fastPlan.audioMuted === true, "high-speed playback should mute audio");
const reversePlan = planReplay.playbackPlan({ speed: 1.0, reverse: true });
assert(reversePlan.audioMuted === true, "reverse playback should mute audio");
assert(reversePlan.audioMode === "muted-reverse", "reverse playback audio mode mismatch");
planReplay.close();

// Phase 2 trick-play: seek modes, reverse window, paced schedule, counters.
assert(caps.seekModes === true, "seek modes capability missing");
assert(caps.reversePlayback === true, "reverse playback capability missing");
assert(caps.playbackScheduler === true, "playback scheduler capability missing");
assert(caps.timestampDiagnostics === true, "timestamp diagnostics capability missing");
assert(caps.evidenceWorkflows === true, "evidence workflows capability missing");
assert(caps.mjpegInput === true, "mjpeg input capability missing");
assert(caps.hardwareDiscovery === true, "hardware discovery capability missing");

const trick = ReplaySession.open(fixturePath);
const fastFrame = trick.extractFrame({ timestampUs: 500000, format: "rgb24", mode: "fast" });
assert(fastFrame.resultMode === "keyframe", "fast seek should report keyframe mode: " + fastFrame.resultMode);
const accurateFrame = trick.extractFrame({ timestampUs: 500000, format: "rgb24", mode: "accurate" });
assert(accurateFrame.resultMode === "exact" || accurateFrame.resultMode === "nearest-after",
  "accurate seek result mode unexpected: " + accurateFrame.resultMode);
const nearestFrame = trick.extractFrame({ timestampUs: 500000, format: "rgb24", mode: "nearest" });
assert(nearestFrame.resultMode === "nearest", "nearest seek result mode unexpected: " + nearestFrame.resultMode);
const frameByIndex = trick.extractFrame({ frameIndex: 5, format: "rgb24", mode: "frame" });
assert(frameByIndex.resultMode === "frame", "frame seek result mode unexpected: " + frameByIndex.resultMode);

const reverseWindow = trick.extractReverseWindow({ timestampUs: 1500000, count: 6, format: "rgb24" });
assert(reverseWindow.count >= 2, "reverse window should return multiple frames: " + reverseWindow.count);
assert(reverseWindow.decreasingMediaTime === true, "reverse window media time should decrease");
assert(reverseWindow.firstPtsUs >= reverseWindow.lastPtsUs, "reverse window first pts should be >= last");

const slow = trick.playSchedule({ startUs: 0, ticks: 32, targetFps: 30, speed: 0.5 });
assert(slow.repeats > 0, "0.5x playback should repeat source frames");
const fast4 = trick.playSchedule({ startUs: 0, ticks: 32, targetFps: 30, speed: 4.0 });
assert(fast4.drops > 0, "4x playback should drop source frames");
const trickState = trick.state();
assert(trickState.repeatCount > 0, "state should report accumulated repeat count");
assert(trickState.dropCount > 0, "state should report accumulated drop count");
trick.close();

// Phase 4 irregular fixture + timestamp diagnostics.
const irregular = generateSyntheticFixture({
  name: "irregular",
  path: irregularPath,
  fps: 30,
  durationSeconds: 2,
  width: 160,
  height: 90,
  gaps: true,
  duplicateTimestamps: true,
  backwardPts: true,
  missingDurations: true
});
assert(irregular.irregular === true, "irregular fixture should be flagged irregular");
assert(irregular.status === "written", "irregular fixture should be written");
const diag = analyzeTimestamps({ path: irregularPath });
assert(diag.packets > 0, "timestamp analysis read no packets");
assert(diag.duplicatePts > 0, "expected duplicate pts diagnostics: " + diag.duplicatePts);
assert(diag.backwardPts > 0, "expected backward pts diagnostics: " + diag.backwardPts);
assert(diag.nonMonotonicPts > 0, "expected non-monotonic pts diagnostics: " + diag.nonMonotonicPts);
assert(diag.nonMonotonicDts > 0, "expected non-monotonic dts diagnostics: " + diag.nonMonotonicDts);
assert(Array.isArray(diag.anomalies) && diag.anomalies.length > 0, "expected anomaly list entries");

// A clean rawvideo fixture should report no timestamp anomalies.
const cleanDiag = analyzeTimestamps({ path: fixturePath });
assert(cleanDiag.duplicatePts === 0 && cleanDiag.backwardPts === 0,
  "clean fixture should have no pts anomalies");

// Phase 4 MJPEG input: encode an mjpeg fixture and decode it back.
const mjpegFixture = generateSyntheticFixture({
  name: "mjpeg",
  path: mjpegPath,
  fps: 15,
  durationSeconds: 1,
  width: 160,
  height: 90,
  videoCodec: "mjpeg"
});
assert(mjpegFixture.videoCodec === "mjpeg", "mjpeg fixture codec mismatch");
const mjpegDemuxer = Demuxer.open(mjpegPath);
const mjpegStreams = mjpegDemuxer.streams();
assert(mjpegStreams.some((s) => s.type === "video" && s.codec === "mjpeg"), "mjpeg stream not reported");
mjpegDemuxer.close();
const mjpegReplay = ReplaySession.open(mjpegPath);
const mjpegFrame = mjpegReplay.extractFrame({ timestampUs: 300000, format: "rgb24" });
assert(mjpegFrame.width === 160 && mjpegFrame.height === 90, "mjpeg decoded frame dimensions mismatch");
assert(mjpegFrame.size === 160 * 90 * 3, "mjpeg decoded frame size mismatch");
mjpegReplay.close();

// Phase 4 evidence workflows.
const still = Evidence.exportStill({ path: fixturePath, outputPath: stillPath, timestampUs: 500000, format: "png" });
assert(still.bytes > 0, "still export wrote no bytes");
assert(still.format === "png", "still export format mismatch");
const clipCopy = Evidence.extractClip({ path: fixturePath, outputPath: clipCopyPath, startUs: 250000, endUs: 750000, mode: "auto" });
assert(clipCopy.mode === "packet-copy", "auto clip should packet-copy: " + clipCopy.mode);
assert(clipCopy.videoFrames > 0, "clip copy produced no frames");
const clipTranscode = Evidence.extractClip({ path: fixturePath, outputPath: clipTranscodePath, startUs: 250000, endUs: 750000, mode: "transcode" });
assert(clipTranscode.mode === "transcode", "transcode clip mode mismatch: " + clipTranscode.mode);
assert(clipTranscode.videoFrames > 0, "transcode clip produced no frames");
const keyframes = Evidence.keyframeWindow({ path: fixturePath, timestampUs: 1000000, windowUs: 500000 });
assert(keyframes.count > 0, "keyframe window found no keyframes");

// Phase 4 hardware discovery, filter info, and profiling.
const hw = hardware();
assert(Array.isArray(hw.deviceTypes), "hardware discovery should list device types");
assert(hw.requiresHardware === false, "hardware must never be required");
assert(hw.softwareFallback === true, "software fallback should be advertised");
const filterInfo = filters();
assert(typeof filterInfo.enabled === "boolean", "filters() should report enabled flag");
if (!filterInfo.enabled) {
  let threw = false;
  try {
    FilterGraph.apply({ path: fixturePath, filter: "scale=80:45" });
  } catch (_) {
    threw = true;
  }
  assert(threw, "FilterGraph.apply should throw when filters are disabled");
} else {
  const filtered = FilterGraph.apply({ path: fixturePath, timestampUs: 500000, filter: "scale=80:45", format: "rgb24" });
  assert(filtered.width === 80 && filtered.height === 45, "filter graph scale mismatch");
}
const profile = profilePacketQueue({ path: fixturePath, queueName: profileQueueName, iterations: 64 });
assert(profile.completed > 0, "packet queue profiling completed no iterations");
assert(profile.perRecordUs >= 0, "packet queue profiling should report per-record cost");
)JS";

    auto result = engine->runModule(runtime, "wl2-ffmpeg-test.js", script.str());
    std::error_code cleanupError;
    std::filesystem::remove(fixturePath, cleanupError);
    std::filesystem::remove(transcodePath, cleanupError);
    std::filesystem::remove(remuxPath, cleanupError);
    std::filesystem::remove(queueRemuxPath, cleanupError);
    std::filesystem::remove(queueDropPath, cleanupError);
    std::filesystem::remove(irregularPath, cleanupError);
    std::filesystem::remove(mjpegPath, cleanupError);
    std::filesystem::remove(stillPath, cleanupError);
    std::filesystem::remove(clipCopyPath, cleanupError);
    std::filesystem::remove(clipTranscodePath, cleanupError);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    std::cout << "wl2_ffmpeg ok\n";
    return 0;
}
