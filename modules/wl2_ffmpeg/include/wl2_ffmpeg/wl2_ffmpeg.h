#pragma once

#include "wl2/module.h"

wl2::ModuleInfo wl2_ffmpeg_register_module(wl2::Runtime& runtime);

extern "C" int wl2_module_get_info(wl2_module_info* out);
extern "C" int wl2_module_register(const wl2_module_host* host);
extern "C" void* wl2_ffmpeg_quickjs_module_factory(void* context, const char* moduleName);
