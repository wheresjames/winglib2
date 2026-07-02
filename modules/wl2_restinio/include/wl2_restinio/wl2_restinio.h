#pragma once

/**
 * @file wl2_restinio.h
 * @brief Registration and dynamic ABI entry points for the wl2:http module.
 *
 * wl2:http is an embeddable HTTP/1.1 (and, later, HTTPS/WebSocket) server built
 * on RESTinio and standalone Asio. RESTinio runs on a module-owned io_context
 * and worker thread; inbound requests are marshalled onto the JavaScript thread
 * through Runtime::async(), where a registered JS handler produces the response.
 * This header declares only the module entry points required by the Winglib2
 * module conventions.
 */

#include "wl2/module.h"

/// Static module entry point. Registers the QuickJS factory and returns metadata.
wl2::ModuleInfo wl2_restinio_register_module(wl2::Runtime& runtime);

/// Dynamic module ABI metadata entry point (dynamic build only).
extern "C" int wl2_module_get_info(wl2_module_info* out);

/// QuickJS module factory for the `wl2:http` specifier.
extern "C" void* wl2_restinio_quickjs_module_factory(void* context, const char* moduleName);
