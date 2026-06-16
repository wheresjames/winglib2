#pragma once

#include "wl2/module.h"

// Static registration entry point, called by the host for every
// RuntimeOptions::staticModules entry during Runtime::initialize().
wl2::ModuleInfo wl2_echo_register_module(wl2::Runtime& runtime);

// C ABI entry points used by the dynamic module build. wl2_module_get_info and
// wl2_module_register are defined only in the dynamic target (guarded by
// WL2_ECHO_STATIC_MODULE) so that many static modules can be linked into one
// executable without colliding.
extern "C" int wl2_module_get_info(wl2_module_info* out);
extern "C" int wl2_module_register(const wl2_module_host* host);
extern "C" void* wl2_echo_quickjs_module_factory(void* context, const char* moduleName);
