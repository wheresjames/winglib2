#include "wl2/wl2.h"
#include "wl2/crash_report.h"

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::Runtime runtime;
    wl2_register_embedded_resources(runtime.resources());
    auto result = runtime.runModule("wl2:/app/main.js");
    return result ? result.value() : 1;
}
