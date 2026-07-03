#pragma once

#include "wl2/module.h"

extern "C" void* wl2_json_quickjs_module_factory(void* context, const char* moduleName);
wl2::ModuleInfo wl2_json_register_module(wl2::Runtime& runtime);
