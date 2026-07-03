#include "wl2_json/wl2_json.h"

#include "wl2/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

using Json = nlohmann::json;
using JsJson = nlohmann::ordered_json;

constexpr const char* JsonApi = R"(Exports JavaScript module wl2:json.

Functions:
  parse(text)                    -> JavaScript value
  stringify(value, options?)     -> JSON text
  canonicalize(value)            -> deterministic compact JSON with sorted keys
  validate(text)                 -> { ok: true } or { ok: false, error, line, column }

stringify options:
  pretty: boolean = false
  indent: number = 2
  sortKeys: boolean = false

The module performs no filesystem I/O and requires no host permissions.)";

#if WL2_HAVE_QUICKJS

std::string js_string(JSContext* ctx, JSValueConst value) {
    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, value);
    if (!text) {
        return {};
    }
    std::string out(text, len);
    JS_FreeCString(ctx, text);
    return out;
}

JSValue json_to_js(JSContext* ctx, const Json& value) {
    if (value.is_null()) {
        return JS_NULL;
    }
    if (value.is_boolean()) {
        return JS_NewBool(ctx, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return JS_NewInt64(ctx, value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        const auto n = value.get<uint64_t>();
        if (n <= static_cast<uint64_t>(INT64_MAX)) {
            return JS_NewInt64(ctx, static_cast<int64_t>(n));
        }
        return JS_NewFloat64(ctx, static_cast<double>(n));
    }
    if (value.is_number_float()) {
        return JS_NewFloat64(ctx, value.get<double>());
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        return JS_NewStringLen(ctx, text.data(), text.size());
    }
    if (value.is_array()) {
        JSValue array = JS_NewArray(ctx);
        if (JS_IsException(array)) {
            return array;
        }
        uint32_t index = 0;
        for (const auto& item : value) {
            JSValue converted = json_to_js(ctx, item);
            if (JS_IsException(converted)) {
                JS_FreeValue(ctx, array);
                return converted;
            }
            JS_SetPropertyUint32(ctx, array, index++, converted);
        }
        return array;
    }
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) {
        return object;
    }
    for (const auto& [key, item] : value.items()) {
        JSValue converted = json_to_js(ctx, item);
        if (JS_IsException(converted)) {
            JS_FreeValue(ctx, object);
            return converted;
        }
        JS_SetPropertyStr(ctx, object, key.c_str(), converted);
    }
    return object;
}

bool seen_object(JSValueConst value, const std::vector<void*>& stack) {
    void* ptr = JS_VALUE_GET_PTR(value);
    for (void* seen : stack) {
        if (seen == ptr) {
            return true;
        }
    }
    return false;
}

void free_properties(JSContext* ctx, JSPropertyEnum* properties, uint32_t propertyCount) {
    if (!properties) {
        return;
    }
    for (uint32_t i = 0; i < propertyCount; ++i) {
        JS_FreeAtom(ctx, properties[i].atom);
    }
    js_free(ctx, properties);
}

bool js_to_json(JSContext* ctx, JSValueConst value, std::vector<void*>& stack, JsJson& out) {
    if (JS_IsUndefined(value)) {
        JS_ThrowTypeError(ctx, "JSON serialization rejects undefined");
        return false;
    }
    if (JS_IsNull(value)) {
        out = nullptr;
        return true;
    }
    if (JS_IsBool(value)) {
        out = JS_ToBool(ctx, value) != 0;
        return true;
    }
    if (JS_IsNumber(value)) {
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) != 0) {
            return false;
        }
        if (!std::isfinite(number)) {
            JS_ThrowTypeError(ctx, "JSON serialization rejects non-finite numbers");
            return false;
        }
        if (std::trunc(number) == number
            && number >= static_cast<double>(std::numeric_limits<int64_t>::min())
            && number <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            out = static_cast<int64_t>(number);
            return true;
        }
        out = number;
        return true;
    }
    if (JS_IsString(value)) {
        out = js_string(ctx, value);
        return true;
    }
    if (JS_IsFunction(ctx, value)) {
        JS_ThrowTypeError(ctx, "JSON serialization rejects functions");
        return false;
    }
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "JSON serialization rejects this JavaScript value");
        return false;
    }

    if (seen_object(value, stack)) {
        JS_ThrowTypeError(ctx, "JSON serialization rejects cyclic objects");
        return false;
    }
    stack.push_back(JS_VALUE_GET_PTR(value));

    int isArray = JS_IsArray(ctx, value);
    if (isArray < 0) {
        stack.pop_back();
        return false;
    }
    if (isArray) {
        JSValue lengthValue = JS_GetPropertyStr(ctx, value, "length");
        uint32_t length = 0;
        if (JS_ToUint32(ctx, &length, lengthValue) != 0) {
            JS_FreeValue(ctx, lengthValue);
            stack.pop_back();
            return false;
        }
        JS_FreeValue(ctx, lengthValue);

        out = JsJson::array();
        for (uint32_t i = 0; i < length; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, value, i);
            if (JS_IsException(item)) {
                stack.pop_back();
                return false;
            }
            JsJson converted;
            const bool convertedOk = js_to_json(ctx, item, stack, converted);
            JS_FreeValue(ctx, item);
            if (!convertedOk) {
                stack.pop_back();
                return false;
            }
            out.push_back(std::move(converted));
        }
        stack.pop_back();
        return true;
    }

    JSPropertyEnum* properties = nullptr;
    uint32_t propertyCount = 0;
    if (JS_GetOwnPropertyNames(
            ctx,
            &properties,
            &propertyCount,
            value,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        stack.pop_back();
        return {};
    }

    out = JsJson::object();
    for (uint32_t i = 0; i < propertyCount; ++i) {
        const char* keyText = JS_AtomToCString(ctx, properties[i].atom);
        if (!keyText) {
            free_properties(ctx, properties, propertyCount);
            stack.pop_back();
            return false;
        }
        std::string key(keyText);
        JS_FreeCString(ctx, keyText);

        JSValue item = JS_GetProperty(ctx, value, properties[i].atom);
        if (JS_IsException(item)) {
            free_properties(ctx, properties, propertyCount);
            stack.pop_back();
            return false;
        }
        JsJson converted;
        const bool convertedOk = js_to_json(ctx, item, stack, converted);
        JS_FreeValue(ctx, item);
        if (!convertedOk) {
            free_properties(ctx, properties, propertyCount);
            stack.pop_back();
            return false;
        }
        out[key] = std::move(converted);
    }
    free_properties(ctx, properties, propertyCount);
    stack.pop_back();
    return true;
}

bool js_value_to_json(JSContext* ctx, JSValueConst value, JsJson& out) {
    std::vector<void*> stack;
    return js_to_json(ctx, value, stack, out);
}

bool option_bool(JSContext* ctx, JSValueConst options, const char* name, bool fallback) {
    if (!JS_IsObject(options)) {
        return fallback;
    }
    JSValue value = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    bool out = JS_ToBool(ctx, value) != 0;
    JS_FreeValue(ctx, value);
    return out;
}

int option_int(JSContext* ctx, JSValueConst options, const char* name, int fallback) {
    if (!JS_IsObject(options)) {
        return fallback;
    }
    JSValue value = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    int32_t out = fallback;
    JS_ToInt32(ctx, &out, value);
    JS_FreeValue(ctx, value);
    return out;
}

JSValue validation_result(JSContext* ctx, bool ok, std::string error = {}, int line = 0, int column = 0) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, ok));
    if (!ok) {
        JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, error.c_str()));
        JS_SetPropertyStr(ctx, obj, "line", JS_NewInt32(ctx, line));
        JS_SetPropertyStr(ctx, obj, "column", JS_NewInt32(ctx, column));
    }
    return obj;
}

std::pair<int, int> line_column_for_byte(std::string_view text, size_t byte) {
    int line = 1;
    int column = 1;
    const size_t target = byte == 0 ? 0 : byte - 1;
    for (size_t i = 0; i < text.size() && i < target; ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {line, column};
}

JSValue json_parse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "parse(text) requires a JSON string");
    }
    const std::string text = js_string(ctx, argv[0]);
    try {
        return json_to_js(ctx, Json::parse(text));
    } catch (const Json::parse_error& e) {
        return JS_ThrowSyntaxError(ctx, "%s", e.what());
    } catch (const Json::exception& e) {
        return JS_ThrowInternalError(ctx, "%s", e.what());
    }
}

JSValue json_stringify(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "stringify(value, options) requires a value");
    }
    JsJson value;
    if (!js_value_to_json(ctx, argv[0], value)) {
        return JS_EXCEPTION;
    }
    const bool pretty = argc > 1 && option_bool(ctx, argv[1], "pretty", false);
    const bool sortKeys = argc > 1 && option_bool(ctx, argv[1], "sortKeys", false);
    const int indent = std::max(0, option_int(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "indent", 2));
    const std::string text = [&] {
        if (!sortKeys) {
            return pretty ? value.dump(indent) : value.dump();
        }
        const Json sorted = Json::parse(value.dump());
        return pretty ? sorted.dump(indent) : sorted.dump();
    }();
    return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue json_canonicalize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "canonicalize(value) requires a value");
    }
    JsJson value;
    if (!js_value_to_json(ctx, argv[0], value)) {
        return JS_EXCEPTION;
    }
    const Json sorted = Json::parse(value.dump());
    const std::string text = sorted.dump();
    return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue json_validate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "validate(text) requires a JSON string");
    }
    const std::string text = js_string(ctx, argv[0]);
    try {
        auto parsed = Json::parse(text);
        (void)parsed;
        return validation_result(ctx, true);
    } catch (const Json::parse_error& e) {
        const auto [line, column] = line_column_for_byte(text, e.byte);
        return validation_result(ctx, false, e.what(), line, column);
    } catch (const Json::exception& e) {
        return validation_result(ctx, false, e.what(), 0, 0);
    }
}

int init_json_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "parse", JS_NewCFunction(ctx, json_parse, "parse", 1));
    JS_SetModuleExport(ctx, module, "stringify", JS_NewCFunction(ctx, json_stringify, "stringify", 2));
    JS_SetModuleExport(ctx, module, "canonicalize", JS_NewCFunction(ctx, json_canonicalize, "canonicalize", 1));
    JS_SetModuleExport(ctx, module, "validate", JS_NewCFunction(ctx, json_validate, "validate", 1));
    return 0;
}

#endif

} // namespace

wl2::ModuleInfo wl2_json_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:json", wl2_json_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:json",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "8c73c0d6-01c0-4c22-8e4f-407356cf76be",
        .summary = "JSON parsing, stringifying, canonicalization, and validation.",
        .api = JsonApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_json_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_json_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "parse");
    JS_AddModuleExport(ctx, module, "stringify");
    JS_AddModuleExport(ctx, module, "canonicalize");
    JS_AddModuleExport(ctx, module, "validate");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_JSON_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:json";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "8c73c0d6-01c0-4c22-8e4f-407356cf76be";
    out->summary = "JSON parsing, stringifying, canonicalization, and validation.";
    out->api = JsonApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
