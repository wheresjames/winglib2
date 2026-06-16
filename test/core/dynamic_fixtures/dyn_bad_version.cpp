// Fixture that requires a wl2 version the host cannot satisfy.
#include "wl2/module.h"

extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:dyn_bad_version";
    out->version = "1.0.0";
    out->stable_id = "00000000-0000-0000-0000-0000000000dd";
    out->summary = "Fixture requiring an incompatible wl2 version.";
    out->api = "No exports; test fixture only.";
    out->unload_safe = 1;
    out->required_wl2_version = "99.0.0";
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    (void)host;
    return 0;
}
