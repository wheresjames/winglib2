// A self-contained single executable that statically links wl2_slint_static and
// runs an embedded counter UI. It demonstrates two things at once: static
// single-executable linking of a native UI module, and the wl2:slint security
// model — the runner grants the UI capability (allowUi), mirroring what
// `wl2 run --allow-ui ...` would grant on the CLI. The UI markup and script are
// embedded as wl2:/ resources, so no files are read at runtime.
#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2_slint/wl2_slint.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::RuntimeOptions options;
    options.allowFilesystem = false;  // everything is embedded
    options.allowUi = true;           // this example opens a window
    // Forward CLI arguments to the script (e.g. --selftest for the smoke test).
    for (int i = 1; i < argc; ++i) {
        options.scriptArgs.emplace_back(argv[i]);
    }
    options.staticModules.push_back(wl2_slint_register_module);

    wl2::Runtime runtime(std::move(options));
    wl2_register_embedded_resources(runtime.resources());

    auto result = runtime.runModule("wl2:/counter/scripts/main.js");
    if (!result) {
        std::cerr << result.error().code() << ": " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}
