#include "wl2/wl2.h"

#include <cstring>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "module requirements test failed: " << message << '\n';
    return 1;
}

// A required module that is never registered must stop initialization before
// the entry script is loaded or executed.
int missing_required_blocks_before_execution() {
    const char* script = "throw new Error('entry script must not run');";

    wl2::RuntimeOptions options;
    options.requiredModules = {"wl2:absent"};
    wl2::Runtime runtime{std::move(options)};
    runtime.resources().add(
        "wl2:/main.js",
        reinterpret_cast<const unsigned char*>(script),
        std::strlen(script));

    // initialize() alone reports the failure without touching the script.
    auto init = runtime.initialize();
    if (init) {
        return fail("initialize should fail when a required module is missing");
    }
    if (init.error().code() != "module_required_missing") {
        return fail("unexpected initialize error code: " + init.error().code());
    }

    // runModule surfaces the same error rather than the script's thrown error,
    // proving the entry script never executed.
    auto result = runtime.runModule("wl2:/main.js");
    if (result) {
        return fail("runModule should fail when a required module is missing");
    }
    if (result.error().code() != "module_required_missing") {
        return fail("required module check did not run before the script: " + result.error().code());
    }
    return 0;
}

// A missing optional module must not block startup.
int missing_optional_does_not_block() {
    wl2::RuntimeOptions options;
    options.optionalModules = {"wl2:absent"};
    wl2::Runtime runtime{std::move(options)};
    auto init = runtime.initialize();
    if (!init) {
        return fail("optional missing module should not block initialization: " + init.error().code());
    }
    return 0;
}

} // namespace

int wl2_module_requirements_tests_entry() {
    if (int rc = missing_required_blocks_before_execution(); rc != 0) {
        return rc;
    }
    if (int rc = missing_optional_does_not_block(); rc != 0) {
        return rc;
    }
    std::cout << "module_requirements ok\n";
    return 0;
}
