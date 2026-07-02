#pragma once

/**
 * @file wl2_webrtc.h
 * @brief Registration and dynamic ABI entry points for the wl2:webrtc module.
 *
 * wl2:webrtc provides WebRTC client/server functionality backed by
 * libdatachannel. It exposes PeerConnection creation, SDP offer/answer and ICE
 * exchange, data channels, media tracks carrying RTP over membus PacketBuffer,
 * and a built-in WebSocket signaling client/server. Native callbacks are
 * delivered through off-thread event queues drained by JavaScript poll calls.
 *
 * libdatachannel runs its own threads; JavaScript callbacks are never invoked on
 * them. Events are marshaled into bounded queues drained by `poll()`.
 *
 * @defgroup wl2_webrtc_module wl2:webrtc module
 * WebRTC transport, data channel, RTP PacketBuffer bridge, and WebSocket
 * signaling bindings for JavaScript.
 * @{
 */

#include "wl2/module.h"

/**
 * @brief Static module entry point.
 *
 * Registers the QuickJS factory for the `wl2:webrtc` specifier and returns the
 * module metadata consumed by the static module registry.
 */
wl2::ModuleInfo wl2_webrtc_register_module(wl2::Runtime& runtime);

/**
 * @brief Dynamic module ABI metadata entry point.
 *
 * Available when the module is built as a dynamic Winglib2 module. Fills @p out
 * with ABI version, module name, summary, and JavaScript API text.
 */
extern "C" int wl2_module_get_info(wl2_module_info* out);

/**
 * @brief QuickJS module factory for the `wl2:webrtc` specifier.
 *
 * @param context QuickJS `JSContext*` supplied by the runtime.
 * @param moduleName Module specifier being loaded.
 * @return QuickJS module definition pointer, or null when QuickJS is disabled.
 */
extern "C" void* wl2_webrtc_quickjs_module_factory(void* context, const char* moduleName);

/** @} */
