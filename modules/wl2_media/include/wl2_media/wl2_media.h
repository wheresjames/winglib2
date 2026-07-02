#pragma once

/**
 * @file wl2_media.h
 * @brief Registration and dynamic ABI entry points for the wl2:media module.
 *
 * wl2:media is the backend-agnostic media schema module. It owns the stream
 * descriptor, packet metadata, timestamp, backpressure, and media error
 * schemas shared by wl2:gstreamer, wl2:ffmpeg, and future media backends. The
 * reusable C++ helpers live in @ref wl2_media/schema.h; this header declares
 * only the module entry points required by the Winglib2 module conventions.
 */

#include "wl2/module.h"

/// Static module entry point. Registers the QuickJS factory and returns metadata.
wl2::ModuleInfo wl2_media_register_module(wl2::Runtime& runtime);

/// Dynamic module ABI metadata entry point (dynamic build only).
extern "C" int wl2_module_get_info(wl2_module_info* out);

/// QuickJS module factory for the `wl2:media` specifier.
extern "C" void* wl2_media_quickjs_module_factory(void* context, const char* moduleName);
