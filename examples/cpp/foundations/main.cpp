#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2_membus/wl2_membus.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::RuntimeOptions options;
    options.allowFilesystem = false;
    options.allowSharedMemory = true;
    options.sharedMemoryAllowList = {"/wl2_foundations"};
    options.staticModules.push_back(wl2_membus_register_module);

    wl2::Runtime runtime(std::move(options));
    wl2_register_embedded_resources(runtime.resources());

    auto result = runtime.runModule("wl2:/foundations/main.js");
    if (!result) {
        std::cerr << result.error().code() << ": " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}
