// value_bridge.h — marshaling between slint::interpreter::Value and QuickJS.
//
// Supported (as far as the basic-UI goal needs): number <-> f64, string <->
// SharedString, bool, JS object <-> one-level Slint Struct, and JS array <->
// Slint model (Value of Type::Model). Brushes/colors are readable and writable
// as CSS-style hex strings ("#rrggbb"/"#rrggbbaa"). Images remain a follow-up;
// unsupported conversions are reported as slint_type_error. All conversions run
// on the JS/UI thread.
#pragma once

#if WL2_HAVE_QUICKJS

#include <quickjs.h>
#include <slint-interpreter.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace wl2_slint_bridge {

using slint::interpreter::Value;
using slint::interpreter::ValueType;

// allowComposite controls whether nested composites (objects -> structs, arrays
// -> models) are accepted. Struct fields convert with allowComposite=false to
// keep structs one level deep; top-level values, array elements, and callback
// arguments use allowComposite=true.
bool js_to_value(JSContext* ctx, JSValueConst js, bool allowComposite, Value& out, std::string& error);

inline JSValue value_to_js(JSContext* ctx, const Value& value);

inline int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

inline bool parse_hex_byte(std::string_view text, std::size_t offset, uint8_t& out) {
    const int high = hex_digit(text[offset]);
    const int low = hex_digit(text[offset + 1]);
    if (high < 0 || low < 0) {
        return false;
    }
    out = static_cast<uint8_t>((high << 4) | low);
    return true;
}

inline std::optional<slint::Color> parse_css_hex_color(std::string_view text) {
    if (text.empty() || text.front() != '#') {
        return std::nullopt;
    }
    if (text.size() == 4) {
        const int r = hex_digit(text[1]);
        const int g = hex_digit(text[2]);
        const int b = hex_digit(text[3]);
        if (r < 0 || g < 0 || b < 0) {
            return std::nullopt;
        }
        return slint::Color::from_rgb_uint8(
            static_cast<uint8_t>((r << 4) | r),
            static_cast<uint8_t>((g << 4) | g),
            static_cast<uint8_t>((b << 4) | b));
    }
    if (text.size() != 7 && text.size() != 9) {
        return std::nullopt;
    }
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
    if (!parse_hex_byte(text, 1, r) || !parse_hex_byte(text, 3, g) || !parse_hex_byte(text, 5, b)) {
        return std::nullopt;
    }
    if (text.size() == 9 && !parse_hex_byte(text, 7, a)) {
        return std::nullopt;
    }
    return slint::Color::from_argb_uint8(a, r, g, b);
}

// Convert a JS object's own enumerable string-keyed properties to a Slint Struct.
// Field values are scalars only (number/string/bool); a nested object or array
// in a field fails with a slint_type_error-style message.
inline bool js_object_to_struct(JSContext* ctx, JSValueConst js, slint::interpreter::Struct& out,
    std::string& error) {
    JSPropertyEnum* props = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, js,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        error = "could not enumerate object properties";
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i) {
        const char* key = JS_AtomToCString(ctx, props[i].atom);
        if (!key) {
            ok = false;
            error = "could not read property name";
            break;
        }
        JSValue field = JS_GetProperty(ctx, js, props[i].atom);
        Value fieldValue;
        if (!js_to_value(ctx, field, /*allowComposite=*/false, fieldValue, error)) {
            JS_FreeValue(ctx, field);
            JS_FreeCString(ctx, key);
            ok = false;
            break;
        }
        out.set_field(std::string_view(key), std::move(fieldValue));
        JS_FreeValue(ctx, field);
        JS_FreeCString(ctx, key);
    }
    for (uint32_t i = 0; i < count; ++i) {
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);
    return ok;
}

// Convert a JS array to a Slint model Value. Elements may themselves be
// composites (structs/models), so nested arrays and arrays of structs work.
inline bool js_array_to_model(JSContext* ctx, JSValueConst js, Value& out, std::string& error) {
    JSValue lenVal = JS_GetPropertyStr(ctx, js, "length");
    int64_t length = 0;
    const bool lenOk = JS_ToInt64(ctx, &length, lenVal) == 0;
    JS_FreeValue(ctx, lenVal);
    if (!lenOk || length < 0) {
        error = "could not read array length";
        return false;
    }
    slint::SharedVector<Value> elements;
    for (int64_t i = 0; i < length; ++i) {
        JSValue el = JS_GetPropertyUint32(ctx, js, static_cast<uint32_t>(i));
        Value elementValue;
        if (!js_to_value(ctx, el, /*allowComposite=*/true, elementValue, error)) {
            JS_FreeValue(ctx, el);
            return false;
        }
        elements.push_back(std::move(elementValue));
        JS_FreeValue(ctx, el);
    }
    out = Value(elements);
    return true;
}

// Convert a JS value to a Slint Value. Returns false and sets `error` for any
// unsupported type.
inline bool js_to_value(JSContext* ctx, JSValueConst js, bool allowComposite, Value& out,
    std::string& error) {
    if (JS_IsBool(js)) {
        out = Value(static_cast<bool>(JS_ToBool(ctx, js)));
        return true;
    }
    if (JS_IsNumber(js)) {
        double number = 0;
        if (JS_ToFloat64(ctx, &number, js) != 0) {
            error = "could not read number";
            return false;
        }
        out = Value(number);
        return true;
    }
    if (JS_IsString(js)) {
        size_t len = 0;
        const char* text = JS_ToCStringLen(ctx, &len, js);
        if (!text) {
            error = "could not read string";
            return false;
        }
        const std::string_view view(text, len);
        if (auto color = parse_css_hex_color(view)) {
            out = Value(slint::Brush(*color));
            JS_FreeCString(ctx, text);
            return true;
        }
        out = Value(slint::SharedString(view));
        JS_FreeCString(ctx, text);
        return true;
    }
    if (JS_IsArray(ctx, js)) {
        if (!allowComposite) {
            error = "nested arrays/models are not supported here";
            return false;
        }
        return js_array_to_model(ctx, js, out, error);
    }
    if (JS_IsObject(js) && !JS_IsFunction(ctx, js)) {
        if (!allowComposite) {
            error = "nested structs are not supported here";
            return false;
        }
        slint::interpreter::Struct strct;
        if (!js_object_to_struct(ctx, js, strct, error)) {
            return false;
        }
        out = Value(std::move(strct));
        return true;
    }
    error = "value type is not supported (expected number, string, bool, array, or object)";
    return false;
}

// Format a Slint color as a CSS-style hex string: "#rrggbb" when opaque,
// otherwise "#rrggbbaa".
inline JSValue color_to_js(JSContext* ctx, const slint::Color& color) {
    char buffer[10];
    if (color.alpha() == 255) {
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
            color.red(), color.green(), color.blue());
    } else {
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x",
            color.red(), color.green(), color.blue(), color.alpha());
    }
    return JS_NewString(ctx, buffer);
}

// Convert a Slint Value to a JS value. Unsupported value types (images) produce
// a JS undefined value.
inline JSValue value_to_js(JSContext* ctx, const Value& value) {
    switch (value.type()) {
        case ValueType::Number: {
            if (auto number = value.to_number()) {
                return JS_NewFloat64(ctx, *number);
            }
            return JS_UNDEFINED;
        }
        case ValueType::String: {
            if (auto text = value.to_string()) {
                std::string_view view(*text);
                return JS_NewStringLen(ctx, view.data(), view.size());
            }
            return JS_UNDEFINED;
        }
        case ValueType::Bool: {
            if (auto flag = value.to_bool()) {
                return JS_NewBool(ctx, *flag);
            }
            return JS_UNDEFINED;
        }
        case ValueType::Model: {
            if (auto array = value.to_array()) {
                JSValue jsArray = JS_NewArray(ctx);
                uint32_t index = 0;
                for (const auto& element : *array) {
                    JS_SetPropertyUint32(ctx, jsArray, index++, value_to_js(ctx, element));
                }
                return jsArray;
            }
            return JS_UNDEFINED;
        }
        case ValueType::Struct: {
            if (auto strct = value.to_struct()) {
                JSValue obj = JS_NewObject(ctx);
                for (auto field : *strct) {
                    std::string name(field.first);
                    JS_SetPropertyStr(ctx, obj, name.c_str(), value_to_js(ctx, field.second));
                }
                return obj;
            }
            return JS_UNDEFINED;
        }
        case ValueType::Brush: {
            if (auto brush = value.to_brush()) {
                return color_to_js(ctx, brush->color());
            }
            return JS_UNDEFINED;
        }
        case ValueType::Void:
        default:
            return JS_UNDEFINED;
    }
}

}  // namespace wl2_slint_bridge

#endif  // WL2_HAVE_QUICKJS
