// Fixture with valid metadata but no wl2_module_register entry point.
#include "wl2/module.h"

extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:dyn_no_register";
    out->version = "1.0.0";
    out->stable_id = "00000000-0000-0000-0000-0000000000bb";
    out->summary = "Fixture missing wl2_module_register.";
    out->api = "No exports; test fixture only.";
    out->unload_safe = 1;
    out->required_wl2_version = nullptr;
    return 0;
}
