#include "wl2/wl2.h"
#include "wl2_echo/wl2_echo.h"

#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_echo test failed: " << message << '\n';
    return 1;
}

} // namespace

int main() {
#if WL2_HAVE_QUICKJS
    wl2::RuntimeOptions options;
    options.staticModules.push_back(wl2_echo_register_module);
    wl2::Runtime runtime{std::move(options)};

    // The module reports its metadata after initialization.
    if (auto init = runtime.initialize(); !init) {
        return fail(init.error().code() + ": " + init.error().message());
    }
    const wl2::ModuleInfo* info = runtime.modules().find("wl2:echo");
    if (!info) {
        return fail("wl2:echo module was not registered");
    }
    if (info->name != "wl2:echo") {
        return fail("unexpected module name: " + info->name);
    }

    static const char* source = R"JS(
import { echo, shout } from "wl2:echo";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

assert(echo("abc") === "abc", "echo identity failed");
assert(shout("abc") === "ABC", "shout upper-case failed");

let threw = false;
try {
  shout();
} catch (error) {
  threw = error.code === "echo_invalid_argument" && error.module === "wl2_echo";
}
assert(threw, "missing argument should throw echo_invalid_argument");
)JS";

    runtime.resources().add(
        "wl2:/tests/echo.js",
        reinterpret_cast<const unsigned char*>(source),
        std::char_traits<char>::length(source));
    auto result = runtime.runModule("wl2:/tests/echo.js");
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    std::cout << "wl2_echo ok\n";
    return 0;
#else
    std::cout << "wl2_echo skipped (QuickJS unavailable)\n";
    return 0;
#endif
}
