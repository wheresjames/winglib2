#pragma once

#include "wl2/module.h"

wl2::ModuleInfo wl2_3d_register_module(wl2::Runtime& runtime);

#if WL2_HAVE_QUICKJS
extern "C" void* wl2_3d_quickjs_module_factory(void* context, const char* moduleName);
#endif
