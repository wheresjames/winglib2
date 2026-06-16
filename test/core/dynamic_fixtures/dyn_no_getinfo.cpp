// Fixture that exports wl2_module_register but not wl2_module_get_info.
#include "wl2/module.h"

extern "C" int wl2_module_register(const wl2_module_host* host) {
    (void)host;
    return 0;
}
