#include "wl2/membus.h"

#include <chrono>
#include <iostream>
#include <string>

namespace {

std::string unique_name(std::string_view suffix) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "/wl2_membus_" + std::to_string(ticks) + "_" + std::string(suffix);
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

int test_shared_buffer() {
#if WL2_HAVE_LIBMEMBUS
    const auto name = unique_name("buffer");
    auto created = wl2::SharedBuffer::create(name, 64);
    if (!created) {
        return fail(created.error().message());
    }

    auto& writer = created.value();
    if (!writer.isOpen() || writer.existing() || writer.size() < 64) {
        return fail("created SharedBuffer has unexpected state");
    }

    auto written = writer.write("hello");
    if (!written || written.value() != 5) {
        return fail("SharedBuffer write failed");
    }

    auto attached = wl2::SharedBuffer::attach(name);
    if (!attached) {
        return fail(attached.error().message());
    }
    if (!attached.value().existing()) {
        return fail("attached SharedBuffer should report existing");
    }

    auto text = attached.value().read(5);
    if (!text || text.value() != "hello") {
        return fail("SharedBuffer read mismatch");
    }

    writer.close();
    attached.value().close();
    return 0;
#else
    std::cout << "libmembus unavailable; skipping shared buffer tests\n";
    return 0;
#endif
}

int test_shared_queue() {
#if WL2_HAVE_LIBMEMBUS
    const auto name = unique_name("queue");
    auto writer = wl2::SharedQueue::create(name, 256, true);
    if (!writer) {
        return fail(writer.error().message());
    }
    auto reader = wl2::SharedQueue::attach(name, 256, false);
    if (!reader) {
        return fail(reader.error().message());
    }

    auto write = writer.value().write("message");
    if (!write) {
        return fail(write.error().message());
    }

    auto read = reader.value().read(std::chrono::milliseconds{100});
    if (!read || read.value() != "message") {
        return fail("SharedQueue read mismatch");
    }

    if (wl2::libmembusHasV12Surface()) {
        if (!reader.value().sessionId()) {
            return fail("SharedQueue v1.2 session id unavailable");
        }
    }

    writer.value().close();
    reader.value().close();
    return 0;
#else
    std::cout << "libmembus unavailable; skipping shared queue tests\n";
    return 0;
#endif
}

int test_video_buffer() {
#if WL2_HAVE_LIBMEMBUS
    const auto name = unique_name("video");
    auto created = wl2::libmembusHasV12Surface()
        ? wl2::VideoBuffer::create(name, 16, 8, wl2::VideoPixelFormat::Rgb24, 30, 3)
        : wl2::VideoBuffer::create(name, 16, 8, 30, 3);
    if (!created) {
        return fail(created.error().message());
    }

    auto& video = created.value();
    if (!video.isOpen() || video.width() != 16 || video.height() != 8 || video.bitsPerPixel() != 24 || video.fps() != 30 || video.buffers() != 3) {
        return fail("VideoBuffer metadata mismatch");
    }
    if (wl2::libmembusHasV12Surface()) {
        if (video.bytesPerPixel() != 3 || video.format() != wl2::VideoPixelFormat::Rgb24 || video.formatName().empty() || !video.sessionId()) {
            return fail("VideoBuffer v1.2 metadata mismatch");
        }
    }

    auto fill = video.fill(0, 0x7f);
    if (!fill) {
        return fail(fill.error().message());
    }
    auto frame = video.frame(0);
    if (!frame || !frame.value().data || frame.value().width != 16 || frame.value().height != 8 || static_cast<unsigned char>(frame.value().data[0]) != 0x7f) {
        return fail("VideoBuffer frame view mismatch");
    }
    if (wl2::libmembusHasV12Surface()) {
        const auto before = video.sequence();
        video.next(1);
        if (!video.waitForFrame(std::chrono::milliseconds{100}, before) || video.sequence() <= before || video.frameSequence(0) <= 0) {
            return fail("VideoBuffer sequence metadata mismatch");
        }
    }

    auto attached = wl2::VideoBuffer::openExisting(name);
    if (!attached || attached.value().width() != 16 || attached.value().height() != 8) {
        return fail("VideoBuffer openExisting failed");
    }

    video.close();
    attached.value().close();
    return 0;
#else
    std::cout << "libmembus unavailable; skipping video buffer tests\n";
    return 0;
#endif
}

int test_audio_buffer() {
#if WL2_HAVE_LIBMEMBUS
    const auto name = unique_name("audio");
    auto created = wl2::libmembusHasV12Surface()
        ? wl2::AudioBuffer::create(name, 2, wl2::AudioSampleFormat::S16Le, 48000, 30, 3)
        : wl2::AudioBuffer::create(name, 2, 16, 48000, 30, 3);
    if (!created) {
        return fail(created.error().message());
    }

    auto& audio = created.value();
    if (!audio.isOpen() || audio.channels() != 2 || audio.bitsPerSample() != 16 || audio.sampleRate() != 48000 || audio.fps() != 30 || audio.buffers() != 3) {
        return fail("AudioBuffer metadata mismatch");
    }
    if (wl2::libmembusHasV12Surface()) {
        if (audio.bytesPerSample() != 2 || audio.format() != wl2::AudioSampleFormat::S16Le || audio.formatName().empty() || !audio.sessionId()) {
            return fail("AudioBuffer v1.2 metadata mismatch");
        }
    }

    auto fill = audio.fill(0, 0x22);
    if (!fill) {
        return fail(fill.error().message());
    }
    auto view = audio.buffer(0);
    if (!view || !view.value().data || view.value().channels != 2 || view.value().bitsPerSample != 16 || static_cast<unsigned char>(view.value().data[0]) != 0x22) {
        return fail("AudioBuffer view mismatch");
    }
    if (wl2::libmembusHasV12Surface()) {
        const auto before = audio.sequence();
        audio.next(1);
        if (!audio.waitForFrame(std::chrono::milliseconds{100}, before) || audio.sequence() <= before || audio.frameSequence(0) <= 0) {
            return fail("AudioBuffer sequence metadata mismatch");
        }
    }

    auto attached = wl2::AudioBuffer::openExisting(name);
    if (!attached || attached.value().channels() != 2 || attached.value().sampleRate() != 48000) {
        return fail("AudioBuffer openExisting failed");
    }

    audio.close();
    attached.value().close();
    return 0;
#else
    std::cout << "libmembus unavailable; skipping audio buffer tests\n";
    return 0;
#endif
}

int test_command_channel() {
#if WL2_HAVE_LIBMEMBUS
    if (!wl2::libmembusHasV12Surface()) {
        std::cout << "libmembus v1.2 surface unavailable; skipping command channel tests\n";
        return 0;
    }

    const auto name = unique_name("command");
    auto reader = wl2::CommandChannel::create(name, 512, true);
    if (!reader) {
        return fail(reader.error().message());
    }
    auto writer = wl2::CommandChannel::attach(name, 512, false);
    if (!writer) {
        return fail(writer.error().message());
    }

    if (!reader.value().isOpen() || reader.value().readerCount() < 1 || !reader.value().sessionId()) {
        return fail("CommandChannel metadata mismatch");
    }

    auto write = writer.value().write("pan_left");
    if (!write) {
        return fail(write.error().message());
    }
    if (!reader.value().poll()) {
        return fail("CommandChannel poll failed");
    }
    auto read = reader.value().read(std::chrono::milliseconds{100});
    if (!read || read.value().payload != "pan_left" || read.value().overrun) {
        return fail("CommandChannel read mismatch");
    }

    reader.value().close();
    writer.value().close();
    return 0;
#else
    std::cout << "libmembus unavailable; skipping command channel tests\n";
    return 0;
#endif
}

int test_key_value_store() {
#if WL2_HAVE_LIBMEMBUS
    if (!wl2::libmembusHasV12Surface()) {
        std::cout << "libmembus v1.2 surface unavailable; skipping key-value tests\n";
        return 0;
    }

    const auto name = unique_name("kv");
    auto invalid = wl2::KeyValueStore::create(unique_name("kv_bad"), 1, 16, 32);
    if (invalid) {
        return fail("KeyValueStore accepted an unaligned schema");
    }

    auto created = wl2::KeyValueStore::create(name, 3, 15, 31);
    if (!created) {
        return fail(created.error().message());
    }
    auto& kv = created.value();
    if (!kv.isOpen() || kv.count() != 3 || kv.maxNameLength() != 15 || kv.maxValueLength() != 31 || !kv.sessionId()) {
        return fail("KeyValueStore metadata mismatch");
    }

    if (!kv.setName(0, "pan") || !kv.setName(1, "tilt") || !kv.setName(2, "zoom")) {
        return fail("KeyValueStore setName failed");
    }
    if (!kv.setValue("pan", "10") || !kv.setValue(1, "-5") || !kv.setAll({{"zoom", "1.5"}})) {
        return fail("KeyValueStore setValue failed");
    }
    auto pan = kv.value("pan");
    auto tilt = kv.value(1);
    if (!pan || pan.value() != "10" || !tilt || tilt.value() != "-5" || kv.findName("zoom") != 2 || kv.slotName(0) != "pan") {
        return fail("KeyValueStore value mismatch");
    }

    auto all = kv.all();
    if (!all || all.value()["zoom"] != "1.5") {
        return fail("KeyValueStore snapshot mismatch");
    }

    int64_t epoch = 0;
    auto changed = kv.changed(epoch);
    if (!changed || changed.value().empty() || epoch <= 0) {
        return fail("KeyValueStore changed mismatch");
    }

    auto attached = wl2::KeyValueStore::open(name);
    if (!attached || !attached.value().existing() || attached.value().value("pan").value() != "10") {
        return fail("KeyValueStore open mismatch");
    }

    kv.close();
    attached.value().close();
    return 0;
#else
    std::cout << "libmembus unavailable; skipping key-value tests\n";
    return 0;
#endif
}

int test_selector() {
#if WL2_HAVE_LIBMEMBUS
    if (!wl2::libmembusHasV12Surface()) {
        std::cout << "libmembus v1.2 surface unavailable; skipping selector tests\n";
        return 0;
    }

    bool first = false;
    bool second = true;
    wl2::MembusSelector selector;
    selector.add([&] { return first; });
    selector.add([&] { return second; });
    if (selector.size() != 2) {
        return fail("MembusSelector size mismatch");
    }
    auto selected = selector.wait(std::chrono::milliseconds{10});
    if (!selected || selected.value() != 1) {
        return fail("MembusSelector selection mismatch");
    }
    selector.clear();
    auto timeout = selector.wait(std::chrono::milliseconds{0});
    if (!timeout || timeout.value() != -1) {
        return fail("MembusSelector timeout mismatch");
    }
    return 0;
#else
    std::cout << "libmembus unavailable; skipping selector tests\n";
    return 0;
#endif
}

} // namespace

int run_membus_tests() {
    if (int rc = test_shared_buffer()) {
        return rc;
    }
    if (int rc = test_shared_queue()) {
        return rc;
    }
    if (int rc = test_video_buffer()) {
        return rc;
    }
    if (int rc = test_audio_buffer()) {
        return rc;
    }
    if (int rc = test_command_channel()) {
        return rc;
    }
    if (int rc = test_key_value_store()) {
        return rc;
    }
    if (int rc = test_selector()) {
        return rc;
    }
    std::cout << "membus ok\n";
    return 0;
}

int run_membus_test_case(std::string_view name) {
    if (name == "shared_buffer") {
        return test_shared_buffer();
    }
    if (name == "shared_queue") {
        return test_shared_queue();
    }
    if (name == "video_buffer") {
        return test_video_buffer();
    }
    if (name == "audio_buffer") {
        return test_audio_buffer();
    }
    if (name == "command_channel") {
        return test_command_channel();
    }
    if (name == "key_value_store") {
        return test_key_value_store();
    }
    if (name == "selector") {
        return test_selector();
    }
    return fail("unknown membus test case");
}
