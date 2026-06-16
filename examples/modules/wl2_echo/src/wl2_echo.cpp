#include "wl2_echo/wl2_echo.h"

#if WL2_ECHO_STATIC_MODULE
#include "wl2/runtime.h"
#endif

#include <cctype>
#include <string>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif

namespace {

constexpr const char* EchoApi = R"(Exports JavaScript module wl2:echo.

Functions:
  echo(text)   -> string   returns the given string unchanged
  shout(text)  -> string   returns the given string in upper case

This module is a minimal, dependency-free reference for the out-of-tree module
shape. It demonstrates static and dynamic targets, registration, metadata, and a
project-local JavaScript runner without needing any host capabilities.)";

#if WL2_HAVE_QUICKJS

JSValue throw_echo_error(
    JSContext* ctx,
    const char* code,
    const char* operation,
    const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "EchoError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_echo"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}

// Read argv[0] as a string argument. Leaves a pending exception and returns
// false when the argument is missing or not a string.
bool read_text_argument(JSContext* ctx, const char* operation, int argc, JSValueConst* argv, std::string& out) {
    if (argc < 1) {
        throw_echo_error(ctx, "echo_invalid_argument", operation,
            std::string(operation) + "(text) requires a string argument");
        return false;
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return false;
    }
    out = text;
    JS_FreeCString(ctx, text);
    return true;
}

JSValue echo_echo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string text;
    if (!read_text_argument(ctx, "echo", argc, argv, text)) {
        return JS_EXCEPTION;
    }
    return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue echo_shout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string text;
    if (!read_text_argument(ctx, "shout", argc, argv, text)) {
        return JS_EXCEPTION;
    }
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return JS_NewStringLen(ctx, text.data(), text.size());
}

int init_echo_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "echo", JS_NewCFunction(ctx, echo_echo, "echo", 1));
    JS_SetModuleExport(ctx, module, "shout", JS_NewCFunction(ctx, echo_shout, "shout", 1));
    return 0;
}
#endif

} // namespace

#if WL2_ECHO_STATIC_MODULE
wl2::ModuleInfo wl2_echo_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:echo", wl2_echo_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:echo",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "7d3e9c2a-1b54-4f86-9a0e-2c6f8b1d4e07",
        .summary = "Minimal example module that echoes and upper-cases strings.",
        .api = EchoApi,
        .unloadSafe = true,
    };
}
#endif

extern "C" void* wl2_echo_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_echo_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "echo");
    JS_AddModuleExport(ctx, module, "shout");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_ECHO_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:echo";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "7d3e9c2a-1b54-4f86-9a0e-2c6f8b1d4e07";
    out->summary = "Minimal example module that echoes and upper-cases strings.";
    out->api = EchoApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}

extern "C" int wl2_module_register(const wl2_module_host* host) {
    if (!host || !host->register_quickjs_module) {
        return 1;
    }
#if WL2_HAVE_QUICKJS
    host->register_quickjs_module(host->host, "wl2:echo", wl2_echo_quickjs_module_factory);
#endif
    return 0;
}
#endif
