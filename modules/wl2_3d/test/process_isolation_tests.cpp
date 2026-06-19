#include "../src/wl2_3d_engine.h"

#include "wl2/membus.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_3d process isolation test failed: " << message << '\n';
    return 1;
}

void write_frame(wl2::VideoFrameView frame, int64_t seq) {
    auto* bytes = reinterpret_cast<std::uint8_t*>(frame.data);
    for (int64_t y = 0; y < frame.height; ++y) {
        auto* row = bytes + static_cast<size_t>(y * frame.scanWidth);
        for (int64_t x = 0; x < frame.width; ++x) {
            row[x * 4 + 0] = static_cast<std::uint8_t>((x * 31 + seq) & 0xff);
            row[x * 4 + 1] = static_cast<std::uint8_t>((y * 47 + seq * 3) & 0xff);
            row[x * 4 + 2] = static_cast<std::uint8_t>((x + y + seq * 7) & 0xff);
            row[x * 4 + 3] = 255;
        }
    }
}

int renderer_process(const std::string& name) {
    auto created = wl2::VideoBuffer::create(name, 16, 8, wl2::VideoPixelFormat::Rgba32, 30, 3);
    if (!created) {
        std::cerr << "renderer failed to create ring: " << created.error().message() << '\n';
        return 42;
    }

    wl2::VideoBuffer video = std::move(created.value());
    int64_t seq = 0;
    for (;;) {
        auto frame = video.frame(0);
        if (!frame) {
            std::cerr << "renderer failed to map frame: " << frame.error().message() << '\n';
            return 43;
        }
        if (!frame.value().data || frame.value().scanWidth < frame.value().width * 4 ||
            frame.value().size < static_cast<size_t>(frame.value().scanWidth * frame.value().height)) {
            std::cerr << "renderer saw invalid frame metadata\n";
            return 44;
        }
        write_frame(frame.value(), ++seq);
        video.next(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::uint64_t checksum(wl2::VideoFrameView frame) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.data);
    std::uint64_t sum = 1469598103934665603ull;
    for (int64_t y = 0; y < frame.height; ++y) {
        const auto* row = bytes + static_cast<size_t>(y * frame.scanWidth);
        for (int64_t x = 0; x < frame.width * 4; ++x) {
            sum ^= row[x];
            sum *= 1099511628211ull;
        }
    }
    return sum;
}

bool ui_side_still_responsive() {
    wl2::three_d::Engine engine;
    wl2::three_d::Node marker;
    marker.id = "ui-marker";
    marker.position = {0.0, 1.0, 0.0};
    const int64_t handle = engine.addNode(marker);
    auto* node = engine.node(handle);
    if (!node || node->id != "ui-marker") {
        return false;
    }

    wl2::three_d::Tween tween;
    tween.node = handle;
    tween.toPosition = wl2::three_d::Vec3{2.0, 1.0, 0.0};
    tween.durationMs = 50.0;
    engine.enqueueTween(tween);
    engine.tick(50.0);
    node = engine.node(handle);
    return node && std::abs(node->position.x - 2.0) < 1e-9;
}

}  // namespace

int main() {
    if (!wl2::libmembusHasV12Surface()) {
        std::cout << "wl2_3d process isolation skipped: libmembus v1.2 surface unavailable\n";
        return 0;
    }

    const std::string name = "/wl2_3d_process_" + std::to_string(getpid());
    const pid_t child = fork();
    if (child < 0) {
        return fail("fork failed");
    }
    if (child == 0) {
        return renderer_process(name);
    }

    wl2::VideoBuffer reader;
    bool attached = false;
    for (int attempt = 0; attempt < 100 && !attached; ++attempt) {
        auto opened = wl2::VideoBuffer::openExisting(name);
        if (opened) {
            reader = std::move(opened.value());
            attached = true;
            break;
        }
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) {
            return fail("renderer process exited before the UI could attach");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!attached) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return fail("timed out attaching to renderer ring");
    }

    int64_t last = reader.sequence();
    bool advanced = false;
    for (int attempt = 0; attempt < 20 && !advanced; ++attempt) {
        advanced = reader.waitForFrame(std::chrono::milliseconds(100), last);
        last = reader.sequence();
    }
    if (!advanced || last <= 0) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return fail("renderer did not publish frames");
    }

    auto before = reader.frame(0);
    if (!before) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return fail("UI reader could not map frame before crash");
    }
    const std::uint64_t beforeSum = checksum(before.value());
    if (beforeSum == 0 || before.value().width != 16 || before.value().height != 8 ||
        before.value().scanWidth < 16 * 4) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return fail("UI reader saw invalid frame metadata");
    }

    kill(child, SIGKILL);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status)) {
        return fail("renderer process was not killed as expected");
    }

    auto after = reader.frame(0);
    if (!after) {
        return fail("UI reader lost its mapped frame after renderer crash");
    }
    if (checksum(after.value()) != beforeSum) {
        return fail("last published frame changed after renderer crash");
    }
    if (!ui_side_still_responsive()) {
        return fail("UI-side engine work did not continue after renderer crash");
    }

    reader.close();
    std::cout << "wl2_3d process isolation ok\n";
    return 0;
}
