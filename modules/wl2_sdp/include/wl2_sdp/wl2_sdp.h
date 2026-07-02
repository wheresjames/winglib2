#pragma once

/**
 * @file wl2_sdp.h
 * @brief Registration and dynamic ABI entry points for the wl2:sdp module.
 *
 * wl2:sdp is a dependency-free SDP (RFC 4566 / 8866) parser and builder. It
 * owns a fidelity-preserving line model (@ref wl2_sdp/sdp.h) so that
 * parse → edit → re-emit round-trips byte-for-byte, which is what makes SDP
 * "munging" (codec reordering, ICE stripping, bandwidth caps) safe. This header
 * declares only the module entry points required by the Winglib2 module
 * conventions.
 */

#include "wl2/module.h"

/// Static module entry point. Registers the QuickJS factory and returns metadata.
wl2::ModuleInfo wl2_sdp_register_module(wl2::Runtime& runtime);

/// Dynamic module ABI metadata entry point (dynamic build only).
extern "C" int wl2_module_get_info(wl2_module_info* out);

/// QuickJS module factory for the `wl2:sdp` specifier.
extern "C" void* wl2_sdp_quickjs_module_factory(void* context, const char* moduleName);
