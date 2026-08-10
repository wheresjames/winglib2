#pragma once

/**
 * @file wl2_uv.h
 * @brief Registration and dynamic ABI entry points for the `wl2:uv` module.
 *
 * `wl2:uv` provides portable system and network inspection backed by libuv.
 * The initial JavaScript API enumerates local interface addresses and derives
 * bounded IPv4 CIDRs. These operations are synchronous and do not create a
 * libuv event loop.
 *
 * @defgroup wl2_uv_module wl2:uv module
 * Cross-platform system and network utilities for JavaScript.
 * @{
 */

#include "wl2/module.h"

/**
 * @brief Register the statically linked `wl2:uv` module.
 *
 * Installs the QuickJS factory for the `wl2:uv` module specifier and returns
 * metadata for Winglib2's static module registry.
 *
 * @param runtime Runtime that will own the JavaScript module registration.
 * @return Module ABI, identity, version, summary, and API metadata.
 */
wl2::ModuleInfo wl2_uv_register_module(wl2::Runtime& runtime);

/**
 * @brief Populate metadata for a dynamically loaded `wl2:uv` module.
 *
 * @param out Destination supplied by the Winglib2 dynamic module loader.
 * @return Zero on success, or non-zero when @p out is null.
 */
extern "C" int wl2_module_get_info(wl2_module_info* out);

/**
 * @brief Create the QuickJS definition for the `wl2:uv` module specifier.
 *
 * @param context QuickJS `JSContext*` supplied as an opaque pointer.
 * @param moduleName Module specifier requested by the JavaScript loader.
 * @return QuickJS `JSModuleDef*` as an opaque pointer, or null on failure or
 *         when QuickJS support is disabled.
 */
extern "C" void* wl2_uv_quickjs_module_factory(void* context, const char* moduleName);

/** @} */
