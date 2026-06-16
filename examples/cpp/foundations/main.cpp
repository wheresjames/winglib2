#include "wl2/wl2.h"
#include "wl2_membus/wl2_membus.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main() {
    wl2::RuntimeOptions options;
    options.allowFilesystem = false;
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
