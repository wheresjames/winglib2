// A dynamic module fixture that declares module dependencies. It
// requires wl2:dyn_good and optionally uses wl2:dyn_metrics. Used to exercise
// dependency metadata reading, validation, install, and graph resolution.
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

const wl2_module_dependency_info kDependencies[] = {
    {"wl2:dyn_good", ">=1.0.0 <2.0.0", nullptr, 1},
    {"wl2:dyn_metrics", "", nullptr, 0},
};
} // namespace

extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:dyn_deps";
    out->version = "1.0.0";
    out->build = WL2_BUILD;
    out->stable_id = "00000000-0000-0000-0000-0000000000bb";
    out->summary = "Dynamic fixture module that declares dependencies.";
    out->api = "No exports; test fixture only.";
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    out->dependencies = kDependencies;
    out->dependency_count = 2;
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    if (!host || !host->register_quickjs_module) {
        return 1;
    }
    host->register_quickjs_module(host->host, "wl2:dyn_deps", &dummy_factory);
    return 0;
}
