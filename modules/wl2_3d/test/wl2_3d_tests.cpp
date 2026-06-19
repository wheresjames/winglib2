#include "wl2/wl2.h"
#include "wl2_3d/wl2_3d.h"

#include <iostream>

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_3d test failed: " << message << '\n';
    return 1;
}

int run_js_foundation_test() {
    wl2::RuntimeOptions options;
    options.staticModules.push_back(wl2_3d_register_module);
    wl2::Runtime runtime{std::move(options)};
    if (auto initialized = runtime.initialize(); !initialized) {
        return fail(initialized.error().code() + ": " + initialized.error().message());
    }
    auto engine = wl2::createConfiguredJsEngine();

    static const char* source = R"JS(
import { Scene, hasRenderer } from "wl2:3d";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const scene = await Scene.create({ size: [16, 8] });
const meta = scene.metadata();
assert(meta.width === 16 && meta.height === 8, "scene size mismatch");
assert(meta.format === "rgba8", "pixel format mismatch");
assert(meta.colorSpace === "srgb", "color space mismatch");
assert(meta.alpha === "premultiplied", "alpha contract mismatch");
assert(meta.origin === "top-left", "origin contract mismatch");
assert(typeof hasRenderer === "boolean", "renderer flag should be boolean");
assert(meta.renderer === (hasRenderer ? "magnum" : "synthetic"), "renderer metadata mismatch");

let denied = false;
try {
  scene.publishTo("/wl2_3d_denied/frame");
} catch (error) {
  denied = error.code === "shared_memory_denied";
}
assert(denied, "publishTo should require shared-memory capability");
scene.close();
)JS";

    auto result = engine->runModule(runtime, "wl2-3d-foundation.test.js", source);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }
    return 0;
}

} // namespace

int main() {
    if (int rc = run_js_foundation_test()) {
        return rc;
    }
    std::cout << "wl2_3d ok\n";
    return 0;
}
