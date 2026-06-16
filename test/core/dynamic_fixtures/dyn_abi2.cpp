// A dynamic module fixture that reports the previous ABI version (2). The host
// must still accept it and treat it as declaring no dependencies, proving
// backward compatibility across the ABI v2 -> v3 dependency-metadata bump.
#include "wl2/module.h"

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif

namespace {
void* dummy_factory(void*, const char*) {
    return nullptr;
}
} // namespace

extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    // Deliberately report ABI v2 and leave the v3 dependency fields unset.
    out->abi_version = 2;
    out->name = "wl2:dyn_abi2";
    out->version = "1.0.0";
    out->stable_id = "00000000-0000-0000-0000-0000000000cc";
    out->summary = "Legacy ABI v2 dynamic fixture module.";
    out->api = "No exports; test fixture only.";
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    if (!host || !host->register_quickjs_module) {
        return 1;
    }
    host->register_quickjs_module(host->host, "wl2:dyn_abi2", &dummy_factory);
    return 0;
}
