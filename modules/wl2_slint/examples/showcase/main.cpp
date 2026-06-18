// A richer self-contained wl2:slint executable. The script and UI are embedded
// as wl2:/showcase resources, and the runtime grants only the UI capability
// needed to open a window.
#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2_slint/wl2_slint.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::RuntimeOptions options;
    options.allowFilesystem = false;
    options.allowUi = true;
    for (int i = 1; i < argc; ++i) {
        options.scriptArgs.emplace_back(argv[i]);
    }
    options.staticModules.push_back(wl2_slint_register_module);

    wl2::Runtime runtime(std::move(options));
    wl2_register_embedded_resources(runtime.resources());

    auto result = runtime.runModule("wl2:/showcase/scripts/main.js");
    if (!result) {
        std::cerr << result.error().code() << ": " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}
