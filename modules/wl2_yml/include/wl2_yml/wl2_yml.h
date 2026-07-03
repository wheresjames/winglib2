#pragma once

#include "wl2/module.h"

extern "C" void* wl2_yml_quickjs_module_factory(void* context, const char* moduleName);
wl2::ModuleInfo wl2_yml_register_module(wl2::Runtime& runtime);
