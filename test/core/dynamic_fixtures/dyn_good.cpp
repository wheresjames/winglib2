// A well-formed dynamic module fixture: valid metadata and a register entry
// that registers a (no-op) QuickJS factory.
#include "wl2/module.h"

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
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
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:dyn_good";
    out->version = "1.0.0";
    out->build = WL2_BUILD;
    out->stable_id = "00000000-0000-0000-0000-0000000000aa";
    out->summary = "Well-formed dynamic fixture module.";
    out->api = "No exports; test fixture only.";
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    if (!host || !host->register_quickjs_module) {
        return 1;
    }
    host->register_quickjs_module(host->host, "wl2:dyn_good", &dummy_factory);
    return 0;
}
