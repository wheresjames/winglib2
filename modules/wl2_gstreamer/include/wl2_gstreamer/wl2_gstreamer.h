#pragma once

/**
 * @file wl2_gstreamer.h
 * @brief Registration and dynamic ABI entry points for the wl2:gstreamer module.
 *
 * wl2:gstreamer wraps the GStreamer library to build, run, and inspect
 * pipelines from JavaScript. The core runtime provides version and capability
 * reporting, plugin/element listing, `parseLaunch()`, pipeline lifecycle and
 * state control, position/duration/seek queries, and synchronous bus polling.
 * Media membus bridges (appsink/appsrc into VideoBuffer / AudioBuffer /
 * PacketBuffer) build on this runtime.
 *
 * Launch strings are a trusted-input API and are documented as such in
 * `docs/security.md`.
 */

#include "wl2/module.h"

/// Static module entry point. Registers the QuickJS factory and returns metadata.
wl2::ModuleInfo wl2_gstreamer_register_module(wl2::Runtime& runtime);

/// Dynamic module ABI metadata entry point (dynamic build only).
extern "C" int wl2_module_get_info(wl2_module_info* out);

/// QuickJS module factory for the `wl2:gstreamer` specifier.
extern "C" void* wl2_gstreamer_quickjs_module_factory(void* context, const char* moduleName);
