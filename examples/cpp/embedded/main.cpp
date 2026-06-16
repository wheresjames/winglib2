#include "wl2/wl2.h"

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main() {
    wl2::Runtime runtime;
    wl2_register_embedded_resources(runtime.resources());
    auto result = runtime.runModule("wl2:/app/main.js");
    return result ? result.value() : 1;
}
