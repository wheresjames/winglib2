#include "wl2_yml/wl2_yml.h"

#include "wl2/runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

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

// The module bridges JavaScript values and YAML through nlohmann::ordered_json,
// reusing the same JS<->value conversion and rejection rules as wl2:json and
// adding a YAML<->value layer on top of yaml-cpp. Key insertion order is
// preserved in both directions.
using Value = nlohmann::ordered_json;

constexpr const char* YmlApi = R"(Exports JavaScript module wl2:yml.

Functions:
  parse(text)                    -> JavaScript value (first YAML document)
  stringify(value, options?)     -> YAML text
  canonicalize(value)            -> deterministic block YAML with sorted keys
  validate(text)                 -> { ok: true } or { ok: false, error, line, column }

stringify options:
  indent: number = 2
  sortKeys: boolean = false
  flow: boolean = false          (emit compact flow style instead of block)

Scalars follow the YAML 1.2 core schema (null, bool, int, float, string).
Strings that would otherwise read back as another scalar type are quoted so
values round-trip. The module performs no filesystem I/O and needs no host
permissions.)";

// ---- YAML scalar typing (YAML 1.2 core schema) --------------------------

bool is_null_token(const std::string& text) {
    return text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL";
}

bool parse_bool_token(const std::string& text, bool& out) {
    if (text == "true" || text == "True" || text == "TRUE") {
        out = true;
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE") {
        out = false;
        return true;
    }
    return false;
}

bool parse_int_token(const std::string& text, int64_t& out) {
    if (text.empty()) {
        return false;
    }
    size_t start = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    if (start >= text.size()) {
        return false;
    }
    for (size_t i = start; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end != text.c_str() + text.size()) {
        return false;
    }
    out = static_cast<int64_t>(value);
    return true;
}

bool parse_float_token(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    size_t start = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    const bool negative = !text.empty() && text[0] == '-';
    std::string rest = text.substr(start);
    std::string lower = rest;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == ".inf") {
        out = negative ? -std::numeric_limits<double>::infinity()
                       : std::numeric_limits<double>::infinity();
        return true;
    }
    if (lower == ".nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }

    // Only accept a decimal float: digits with a fractional part and/or an
    // exponent. Requiring one of '.', 'e', 'E' keeps plain integers out and the
    // restricted character set keeps hex/other strtod extensions out.
    bool hasMarker = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.' || c == 'e' || c == 'E') {
            hasMarker = true;
        } else if (!std::isdigit(static_cast<unsigned char>(c)) && c != '+' && c != '-') {
            return false;
        }
    }
    if (!hasMarker) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

Value plain_scalar_to_value(const std::string& text) {
    if (is_null_token(text)) {
        return nullptr;
    }
    bool boolValue = false;
    if (parse_bool_token(text, boolValue)) {
        return boolValue;
    }
    int64_t intValue = 0;
    if (parse_int_token(text, intValue)) {
        return intValue;
    }
    double floatValue = 0.0;
    if (parse_float_token(text, floatValue)) {
        return floatValue;
    }
    return text;
}

Value scalar_to_value(const YAML::Node& node) {
    const std::string tag = node.Tag();
    const std::string text = node.Scalar();
    // Explicit core-schema tags win over inference.
    if (tag == "tag:yaml.org,2002:str" || tag == "!!str") {
        return text;
    }
    if (tag == "tag:yaml.org,2002:null" || tag == "!!null") {
        return nullptr;
    }
    if (tag == "tag:yaml.org,2002:bool" || tag == "!!bool") {
        bool boolValue = false;
        return parse_bool_token(text, boolValue) ? Value(boolValue) : Value(text);
    }
    if (tag == "tag:yaml.org,2002:int" || tag == "!!int") {
        int64_t intValue = 0;
        return parse_int_token(text, intValue) ? Value(intValue) : Value(text);
    }
    if (tag == "tag:yaml.org,2002:float" || tag == "!!float") {
        double floatValue = 0.0;
        return parse_float_token(text, floatValue) ? Value(floatValue) : Value(text);
    }
    // Quoted scalars carry the non-plain "!" tag and are always strings.
    if (tag == "!") {
        return text;
    }
    return plain_scalar_to_value(text);
}

std::string yaml_key_to_string(const YAML::Node& key) {
    if (key.IsScalar()) {
        return key.Scalar();
    }
    YAML::Emitter emitter;
    emitter << YAML::Flow << key;
    return emitter.good() ? std::string(emitter.c_str()) : std::string();
}

Value yaml_to_value(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Null:
        case YAML::NodeType::Undefined:
            return nullptr;
        case YAML::NodeType::Scalar:
            return scalar_to_value(node);
        case YAML::NodeType::Sequence: {
            Value array = Value::array();
            for (const auto& item : node) {
                array.push_back(yaml_to_value(item));
            }
            return array;
        }
        case YAML::NodeType::Map: {
            Value object = Value::object();
            for (auto it = node.begin(); it != node.end(); ++it) {
                object[yaml_key_to_string(it->first)] = yaml_to_value(it->second);
            }
            return object;
        }
    }
    return nullptr;
}

// ---- value -> YAML emission ---------------------------------------------

// True when a plain-emitted string would be re-read as a non-string scalar and
// therefore must be quoted to preserve its type on the next parse.
bool string_needs_quoting(const std::string& text) {
    if (is_null_token(text)) {
        return true;
    }
    bool boolValue = false;
    if (parse_bool_token(text, boolValue)) {
        return true;
    }
    int64_t intValue = 0;
    if (parse_int_token(text, intValue)) {
        return true;
    }
    double floatValue = 0.0;
    if (parse_float_token(text, floatValue)) {
        return true;
    }
    return false;
}

void emit_value(YAML::Emitter& out, const Value& value, bool sortKeys, bool flow) {
    if (value.is_null()) {
        out << YAML::Null;
        return;
    }
    if (value.is_boolean()) {
        out << value.get<bool>();
        return;
    }
    if (value.is_number_integer()) {
        out << value.get<int64_t>();
        return;
    }
    if (value.is_number_unsigned()) {
        out << value.get<uint64_t>();
        return;
    }
    if (value.is_number_float()) {
        out << value.get<double>();
        return;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (string_needs_quoting(text)) {
            out << YAML::DoubleQuoted;
        }
        out << text;
        return;
    }
    if (value.is_array()) {
        if (flow) {
            out << YAML::Flow;
        }
        out << YAML::BeginSeq;
        for (const auto& item : value) {
            emit_value(out, item, sortKeys, flow);
        }
        out << YAML::EndSeq;
        return;
    }
    // object
    if (flow) {
        out << YAML::Flow;
    }
    out << YAML::BeginMap;
    if (sortKeys) {
        std::vector<std::string> keys;
        keys.reserve(value.size());
        for (auto it = value.begin(); it != value.end(); ++it) {
            keys.push_back(it.key());
        }
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            out << YAML::Key << key << YAML::Value;
            emit_value(out, value.at(key), sortKeys, flow);
        }
    } else {
        for (auto it = value.begin(); it != value.end(); ++it) {
            out << YAML::Key << it.key() << YAML::Value;
            emit_value(out, it.value(), sortKeys, flow);
        }
    }
    out << YAML::EndMap;
}

std::string emit_document(const Value& value, int indent, bool sortKeys, bool flow) {
    YAML::Emitter emitter;
    if (indent > 0) {
        emitter.SetIndent(indent);
    }
    emit_value(emitter, value, sortKeys, flow);
    if (!emitter.good()) {
        throw YAML::Exception(YAML::Mark::null_mark(), emitter.GetLastError());
    }
    return std::string(emitter.c_str());
}

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

JSValue value_to_js(JSContext* ctx, const Value& value) {
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
            JSValue converted = value_to_js(ctx, item);
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
        JSValue converted = value_to_js(ctx, item);
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

bool js_to_value(JSContext* ctx, JSValueConst value, std::vector<void*>& stack, Value& out) {
    if (JS_IsUndefined(value)) {
        JS_ThrowTypeError(ctx, "YAML serialization rejects undefined");
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
            JS_ThrowTypeError(ctx, "YAML serialization rejects non-finite numbers");
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
        JS_ThrowTypeError(ctx, "YAML serialization rejects functions");
        return false;
    }
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "YAML serialization rejects this JavaScript value");
        return false;
    }

    if (seen_object(value, stack)) {
        JS_ThrowTypeError(ctx, "YAML serialization rejects cyclic objects");
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

        out = Value::array();
        for (uint32_t i = 0; i < length; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, value, i);
            if (JS_IsException(item)) {
                stack.pop_back();
                return false;
            }
            Value converted;
            const bool convertedOk = js_to_value(ctx, item, stack, converted);
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
        return false;
    }

    out = Value::object();
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
        Value converted;
        const bool convertedOk = js_to_value(ctx, item, stack, converted);
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

bool js_value_to_value(JSContext* ctx, JSValueConst value, Value& out) {
    std::vector<void*> stack;
    return js_to_value(ctx, value, stack, out);
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

JSValue yml_parse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "parse(text) requires a YAML string");
    }
    const std::string text = js_string(ctx, argv[0]);
    try {
        YAML::Node node = YAML::Load(text);
        return value_to_js(ctx, yaml_to_value(node));
    } catch (const YAML::Exception& e) {
        return JS_ThrowSyntaxError(ctx, "%s", e.what());
    }
}

JSValue yml_stringify(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "stringify(value, options) requires a value");
    }
    Value value;
    if (!js_value_to_value(ctx, argv[0], value)) {
        return JS_EXCEPTION;
    }
    const bool sortKeys = argc > 1 && option_bool(ctx, argv[1], "sortKeys", false);
    const bool flow = argc > 1 && option_bool(ctx, argv[1], "flow", false);
    const int indent = std::max(0, option_int(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "indent", 2));
    try {
        const std::string text = emit_document(value, indent, sortKeys, flow);
        return JS_NewStringLen(ctx, text.data(), text.size());
    } catch (const YAML::Exception& e) {
        return JS_ThrowInternalError(ctx, "%s", e.what());
    }
}

JSValue yml_canonicalize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "canonicalize(value) requires a value");
    }
    Value value;
    if (!js_value_to_value(ctx, argv[0], value)) {
        return JS_EXCEPTION;
    }
    try {
        const std::string text = emit_document(value, 2, /*sortKeys=*/true, /*flow=*/false);
        return JS_NewStringLen(ctx, text.data(), text.size());
    } catch (const YAML::Exception& e) {
        return JS_ThrowInternalError(ctx, "%s", e.what());
    }
}

JSValue yml_validate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "validate(text) requires a YAML string");
    }
    const std::string text = js_string(ctx, argv[0]);
    try {
        YAML::Node node = YAML::Load(text);
        (void)node;
        return validation_result(ctx, true);
    } catch (const YAML::Exception& e) {
        // yaml-cpp marks are 0-based; report 1-based positions.
        const int line = e.mark.line >= 0 ? e.mark.line + 1 : 0;
        const int column = e.mark.column >= 0 ? e.mark.column + 1 : 0;
        return validation_result(ctx, false, e.what(), line, column);
    }
}

int init_yml_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "parse", JS_NewCFunction(ctx, yml_parse, "parse", 1));
    JS_SetModuleExport(ctx, module, "stringify", JS_NewCFunction(ctx, yml_stringify, "stringify", 2));
    JS_SetModuleExport(ctx, module, "canonicalize", JS_NewCFunction(ctx, yml_canonicalize, "canonicalize", 1));
    JS_SetModuleExport(ctx, module, "validate", JS_NewCFunction(ctx, yml_validate, "validate", 1));
    return 0;
}

#endif

} // namespace

wl2::ModuleInfo wl2_yml_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:yml", wl2_yml_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:yml",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "be935e3c-3348-4f62-a20e-17958558e8d9",
        .summary = "YAML parsing, stringifying, canonicalization, and validation.",
        .api = YmlApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_yml_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_yml_module);
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

#if !WL2_YML_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:yml";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "be935e3c-3348-4f62-a20e-17958558e8d9";
    out->summary = "YAML parsing, stringifying, canonicalization, and validation.";
    out->api = YmlApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
