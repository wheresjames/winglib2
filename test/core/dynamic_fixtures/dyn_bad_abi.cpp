// Fixture that reports an incompatible ABI version.
#include "wl2/module.h"

extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = 999;
    out->name = "wl2:dyn_bad_abi";
    out->version = "1.0.0";
    out->stable_id = "00000000-0000-0000-0000-0000000000cc";
    out->summary = "Fixture with a mismatched ABI version.";
    out->api = "No exports; test fixture only.";
    out->unload_safe = 1;
    out->required_wl2_version = nullptr;
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    (void)host;
    return 0;
}
