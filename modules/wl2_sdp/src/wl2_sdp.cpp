#include "wl2_sdp/wl2_sdp.h"

#include "wl2_sdp/sdp.h"

#include "wl2/runtime.h"

#include <string>
#include <vector>

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

namespace sdp = wl2::sdp;

constexpr const char* SdpApi = R"(Exports JavaScript module wl2:sdp.

Dependency-free SDP (RFC 4566 / 8866) parser and builder with a
fidelity-preserving line model, so parse -> edit -> build round-trips exactly.
A "section" is either the session object or a media object; both carry a
`lines` array of { type, value }, and the attribute helpers operate on either.

Functions:
  parse(text, options?)        -> session { lines, media, crlf, trailingNewline, warnings }
                                  options: { lenient, crlfOnly, maxLen, maxMedia, maxLines }
  build(session, options?)     -> string   options: { canonical, crlf }
  getAttr(section, key)        -> string | null   ("" for a valueless flag)
  getAttrs(section, key)       -> [ string ]
  setAttr(section, key, value) -> undefined        (replace-first-or-append)
  setFlag(section, key)        -> undefined        (valueless a=<key>)
  addAttr(section, rawValue)   -> undefined        (always append a=<rawValue>)
  removeAttr(section, key)     -> number removed
  keepPayloads(media, pts)     -> undefined        (filter m= + orphan rtpmap/fmtp/rtcp-fb)
  reorderPayloads(media, pts)  -> undefined        (codec preference)
  rtpmaps(section)             -> [ { payload, codec, clock, channels } ]
  candidates(section)          -> [ { foundation, component, proto, priority, ip, port, type, ext } ]
  fingerprint(section)         -> { hashFunc, value } | null
  compare(a, b)                -> bool  (order-normalized textual; test helper)
  errorCodes()                 -> { INVALID_ARGUMENT, PARSE_FAILED, TOO_LARGE, NO_SESSION, BUILD_FAILED }

Errors use the shared SdpError shape with stable sdp_* codes.)";

#if WL2_HAVE_QUICKJS

JSValue throw_sdp_error(JSContext* ctx, const char* code, const char* operation, const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "SdpError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_sdp"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}

// --- Small property readers (mirror the wl2:media conventions) ------------

bool read_string(JSContext* ctx, JSValueConst obj, const char* key, std::string& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    const char* text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (!text) {
        return false;
    }
    out = text;
    JS_FreeCString(ctx, text);
    return true;
}

bool read_bool(JSContext* ctx, JSValueConst obj, const char* key, bool& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    out = JS_ToBool(ctx, value) == 1;
    JS_FreeValue(ctx, value);
    return true;
}

bool read_int(JSContext* ctx, JSValueConst obj, const char* key, int64_t& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    int64_t parsed = 0;
    int rc = JS_ToInt64(ctx, &parsed, value);
    JS_FreeValue(ctx, value);
    if (rc < 0) {
        return false;
    }
    out = parsed;
    return true;
}

int64_t array_length(JSContext* ctx, JSValueConst array) {
    JSValue len = JS_GetPropertyStr(ctx, array, "length");
    int64_t n = 0;
    JS_ToInt64(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n < 0 ? 0 : n;
}

// --- Line <-> JS ----------------------------------------------------------

JSValue line_to_js(JSContext* ctx, const sdp::Line& line) {
    JSValue obj = JS_NewObject(ctx);
    char t[2] = {line.type, '\0'};
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, t));
    JS_SetPropertyStr(ctx, obj, "value", JS_NewString(ctx, line.value.c_str()));
    return obj;
}

JSValue lines_to_js(JSContext* ctx, const std::vector<sdp::Line>& lines) {
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const sdp::Line& line : lines) {
        JS_SetPropertyUint32(ctx, array, i++, line_to_js(ctx, line));
    }
    return array;
}

// Read a section's `lines` array into a C++ vector. Absent/non-array -> empty.
std::vector<sdp::Line> read_lines(JSContext* ctx, JSValueConst section) {
    std::vector<sdp::Line> out;
    JSValue arr = JS_GetPropertyStr(ctx, section, "lines");
    if (JS_IsArray(ctx, arr)) {
        int64_t n = array_length(ctx, arr);
        for (int64_t i = 0; i < n; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
            std::string type;
            std::string value;
            read_string(ctx, el, "type", type);
            read_string(ctx, el, "value", value);
            JS_FreeValue(ctx, el);
            out.push_back(sdp::Line{type.empty() ? 'a' : type[0], std::move(value)});
        }
    }
    JS_FreeValue(ctx, arr);
    return out;
}

void write_lines(JSContext* ctx, JSValueConst section, const std::vector<sdp::Line>& lines) {
    JS_SetPropertyStr(ctx, section, "lines", lines_to_js(ctx, lines));
}

// Wrap a section's lines in a temporary Media so the scope-agnostic helpers and
// typed views can be reused for both session and media objects.
sdp::Media section_as_media(JSContext* ctx, JSValueConst section) {
    sdp::Media media;
    media.lines = read_lines(ctx, section);
    return media;
}

// --- Media / session <-> JS ----------------------------------------------

std::vector<std::string> read_string_array(JSContext* ctx, JSValueConst obj, const char* key) {
    std::vector<std::string> out;
    JSValue arr = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsArray(ctx, arr)) {
        int64_t n = array_length(ctx, arr);
        for (int64_t i = 0; i < n; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
            const char* text = JS_ToCString(ctx, el);
            if (text) {
                out.emplace_back(text);
                JS_FreeCString(ctx, text);
            }
            JS_FreeValue(ctx, el);
        }
    }
    JS_FreeValue(ctx, arr);
    return out;
}

JSValue media_to_js(JSContext* ctx, const sdp::Media& media) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, media.kind.c_str()));
    JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, media.port.c_str()));
    JS_SetPropertyStr(ctx, obj, "proto", JS_NewString(ctx, media.proto.c_str()));
    JSValue formats = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const std::string& fmt : media.formats) {
        JS_SetPropertyUint32(ctx, formats, i++, JS_NewString(ctx, fmt.c_str()));
    }
    JS_SetPropertyStr(ctx, obj, "formats", formats);
    JS_SetPropertyStr(ctx, obj, "lines", lines_to_js(ctx, media.lines));
    return obj;
}

sdp::Media js_to_media(JSContext* ctx, JSValueConst obj) {
    sdp::Media media;
    read_string(ctx, obj, "kind", media.kind);
    read_string(ctx, obj, "port", media.port);
    read_string(ctx, obj, "proto", media.proto);
    media.formats = read_string_array(ctx, obj, "formats");
    media.lines = read_lines(ctx, obj);
    return media;
}

JSValue session_to_js(JSContext* ctx, const sdp::Session& session) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "crlf", JS_NewBool(ctx, session.crlf));
    JS_SetPropertyStr(ctx, obj, "trailingNewline", JS_NewBool(ctx, session.trailingNewline));
    JS_SetPropertyStr(ctx, obj, "lines", lines_to_js(ctx, session.lines));
    JSValue media = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const sdp::Media& m : session.media) {
        JS_SetPropertyUint32(ctx, media, i++, media_to_js(ctx, m));
    }
    JS_SetPropertyStr(ctx, obj, "media", media);
    JSValue warnings = JS_NewArray(ctx);
    i = 0;
    for (const std::string& w : session.warnings) {
        JS_SetPropertyUint32(ctx, warnings, i++, JS_NewString(ctx, w.c_str()));
    }
    JS_SetPropertyStr(ctx, obj, "warnings", warnings);
    return obj;
}

sdp::Session js_to_session(JSContext* ctx, JSValueConst obj) {
    sdp::Session session;
    session.crlf = true;
    session.trailingNewline = true;
    read_bool(ctx, obj, "crlf", session.crlf);
    read_bool(ctx, obj, "trailingNewline", session.trailingNewline);
    session.lines = read_lines(ctx, obj);
    JSValue media = JS_GetPropertyStr(ctx, obj, "media");
    if (JS_IsArray(ctx, media)) {
        int64_t n = array_length(ctx, media);
        for (int64_t i = 0; i < n; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, media, static_cast<uint32_t>(i));
            session.media.push_back(js_to_media(ctx, el));
            JS_FreeValue(ctx, el);
        }
    }
    JS_FreeValue(ctx, media);
    return session;
}

// --- Exported functions ---------------------------------------------------

JSValue js_parse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "parse", "parse(text, options?) requires a string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_EXCEPTION;
    }
    std::string input = text;
    JS_FreeCString(ctx, text);

    sdp::ParseOptions options;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        read_bool(ctx, argv[1], "lenient", options.lenient);
        read_bool(ctx, argv[1], "crlfOnly", options.crlfOnly);
        int64_t v = 0;
        if (read_int(ctx, argv[1], "maxLen", v) && v > 0) {
            options.maxLen = static_cast<std::size_t>(v);
        }
        if (read_int(ctx, argv[1], "maxMedia", v) && v > 0) {
            options.maxMedia = static_cast<std::size_t>(v);
        }
        if (read_int(ctx, argv[1], "maxLines", v) && v > 0) {
            options.maxLines = static_cast<std::size_t>(v);
        }
    }

    auto result = sdp::parse(input, options);
    if (!result) {
        return throw_sdp_error(ctx, result.error().code().c_str(), "parse", result.error().message());
    }
    return session_to_js(ctx, result.value());
}

JSValue js_build(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "build", "build(session, options?) requires a session object");
    }
    sdp::Session session = js_to_session(ctx, argv[0]);
    sdp::BuildOptions options;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        read_bool(ctx, argv[1], "canonical", options.canonical);
        bool crlf = false;
        if (read_bool(ctx, argv[1], "crlf", crlf)) {
            options.ending = crlf ? sdp::LineEnding::Crlf : sdp::LineEnding::Lf;
        }
    }
    std::string out = sdp::build(session, options);
    return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue js_get_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "getAttr", "getAttr(section, key) requires a section and key");
    }
    const char* keyText = JS_ToCString(ctx, argv[1]);
    if (!keyText) {
        return JS_EXCEPTION;
    }
    std::string key = keyText;
    JS_FreeCString(ctx, keyText);

    sdp::Media media = section_as_media(ctx, argv[0]);
    auto value = sdp::attribute(media, key);
    if (!value) {
        return JS_NULL;
    }
    return JS_NewString(ctx, value->c_str());
}

JSValue js_get_attrs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "getAttrs", "getAttrs(section, key) requires a section and key");
    }
    const char* keyText = JS_ToCString(ctx, argv[1]);
    if (!keyText) {
        return JS_EXCEPTION;
    }
    std::string key = keyText;
    JS_FreeCString(ctx, keyText);

    sdp::Media media = section_as_media(ctx, argv[0]);
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const std::string& value : sdp::attributes(media, key)) {
        JS_SetPropertyUint32(ctx, array, i++, JS_NewString(ctx, value.c_str()));
    }
    return array;
}

// Shared body for setAttr / setFlag / addAttr / removeAttr.
enum class MutateOp { SetAttr, SetFlag, AddAttr, RemoveAttr };

JSValue js_mutate(JSContext* ctx, MutateOp op, const char* name, int argc, JSValueConst* argv) {
    int minArgs = op == MutateOp::SetAttr ? 3 : 2;
    if (argc < minArgs || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, name, std::string(name) + " requires a section and key");
    }
    const char* keyText = JS_ToCString(ctx, argv[1]);
    if (!keyText) {
        return JS_EXCEPTION;
    }
    std::string key = keyText;
    JS_FreeCString(ctx, keyText);

    std::string value;
    if (op == MutateOp::SetAttr) {
        const char* valText = JS_ToCString(ctx, argv[2]);
        if (!valText) {
            return JS_EXCEPTION;
        }
        value = valText;
        JS_FreeCString(ctx, valText);
    }

    sdp::Media media = section_as_media(ctx, argv[0]);
    JSValue rv = JS_UNDEFINED;
    switch (op) {
        case MutateOp::SetAttr:
            sdp::setAttribute(media, key, value);
            break;
        case MutateOp::SetFlag:
            sdp::setFlag(media, key);
            break;
        case MutateOp::AddAttr:
            sdp::addAttribute(media, key); // key holds the raw value here.
            break;
        case MutateOp::RemoveAttr:
            rv = JS_NewInt32(ctx, static_cast<int32_t>(sdp::removeAttribute(media, key)));
            break;
    }
    write_lines(ctx, argv[0], media.lines);
    return rv;
}

JSValue js_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return js_mutate(ctx, MutateOp::SetAttr, "setAttr", argc, argv);
}
JSValue js_set_flag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return js_mutate(ctx, MutateOp::SetFlag, "setFlag", argc, argv);
}
JSValue js_add_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return js_mutate(ctx, MutateOp::AddAttr, "addAttr", argc, argv);
}
JSValue js_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return js_mutate(ctx, MutateOp::RemoveAttr, "removeAttr", argc, argv);
}

std::vector<int> read_int_array(JSContext* ctx, JSValueConst array) {
    std::vector<int> out;
    if (!JS_IsArray(ctx, array)) {
        return out;
    }
    int64_t n = array_length(ctx, array);
    for (int64_t i = 0; i < n; ++i) {
        JSValue el = JS_GetPropertyUint32(ctx, array, static_cast<uint32_t>(i));
        int32_t v = 0;
        if (JS_ToInt32(ctx, &v, el) == 0) {
            out.push_back(v);
        }
        JS_FreeValue(ctx, el);
    }
    return out;
}

JSValue js_payload_op(JSContext* ctx, bool keep, const char* name, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsArray(ctx, argv[1])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, name, std::string(name) + " requires a media object and a payload array");
    }
    sdp::Media media = js_to_media(ctx, argv[0]);
    std::vector<int> pts = read_int_array(ctx, argv[1]);
    if (keep) {
        sdp::keepPayloads(media, pts);
    } else {
        sdp::reorderPayloads(media, pts);
    }
    // Write the mutated formats and lines back onto the media object.
    JSValue formats = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const std::string& fmt : media.formats) {
        JS_SetPropertyUint32(ctx, formats, i++, JS_NewString(ctx, fmt.c_str()));
    }
    JS_SetPropertyStr(ctx, argv[0], "formats", formats);
    write_lines(ctx, argv[0], media.lines);
    return JS_UNDEFINED;
}

JSValue js_keep_payloads(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return js_payload_op(ctx, /*keep=*/true, "keepPayloads", argc, argv);
}
JSValue js_reorder_payloads(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return js_payload_op(ctx, /*keep=*/false, "reorderPayloads", argc, argv);
}

JSValue js_rtpmaps(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "rtpmaps", "rtpmaps(section) requires a section");
    }
    sdp::Media media = section_as_media(ctx, argv[0]);
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const sdp::Rtpmap& m : sdp::rtpmaps(media)) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "payload", JS_NewInt32(ctx, m.payload));
        JS_SetPropertyStr(ctx, obj, "codec", JS_NewString(ctx, m.codec.c_str()));
        JS_SetPropertyStr(ctx, obj, "clock", JS_NewInt32(ctx, m.clock));
        JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, m.channels));
        JS_SetPropertyUint32(ctx, array, i++, obj);
    }
    return array;
}

JSValue js_candidates(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "candidates", "candidates(section) requires a section");
    }
    sdp::Media media = section_as_media(ctx, argv[0]);
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const sdp::Candidate& c : sdp::candidates(media)) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "foundation", JS_NewString(ctx, c.foundation.c_str()));
        JS_SetPropertyStr(ctx, obj, "component", JS_NewInt32(ctx, c.component));
        JS_SetPropertyStr(ctx, obj, "proto", JS_NewString(ctx, c.proto.c_str()));
        JS_SetPropertyStr(ctx, obj, "priority", JS_NewInt64(ctx, c.priority));
        JS_SetPropertyStr(ctx, obj, "ip", JS_NewString(ctx, c.ip.c_str()));
        JS_SetPropertyStr(ctx, obj, "port", JS_NewInt32(ctx, c.port));
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, c.type.c_str()));
        JSValue ext = JS_NewObject(ctx);
        for (const auto& [k, v] : c.ext) {
            JS_SetPropertyStr(ctx, ext, k.c_str(), JS_NewString(ctx, v.c_str()));
        }
        JS_SetPropertyStr(ctx, obj, "ext", ext);
        JS_SetPropertyUint32(ctx, array, i++, obj);
    }
    return array;
}

JSValue js_fingerprint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "fingerprint", "fingerprint(section) requires a section");
    }
    sdp::Media media = section_as_media(ctx, argv[0]);
    auto fp = sdp::fingerprint(media);
    if (!fp) {
        return JS_NULL;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "hashFunc", JS_NewString(ctx, fp->hashFunc.c_str()));
    JS_SetPropertyStr(ctx, obj, "value", JS_NewString(ctx, fp->value.c_str()));
    return obj;
}

JSValue js_compare(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1])) {
        return throw_sdp_error(ctx, sdp::errors::InvalidArgument, "compare", "compare(a, b) requires two session objects");
    }
    sdp::Session a = js_to_session(ctx, argv[0]);
    sdp::Session b = js_to_session(ctx, argv[1]);
    return JS_NewBool(ctx, sdp::equalNormalized(a, b));
}

JSValue js_error_codes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "INVALID_ARGUMENT", JS_NewString(ctx, sdp::errors::InvalidArgument));
    JS_SetPropertyStr(ctx, obj, "PARSE_FAILED", JS_NewString(ctx, sdp::errors::ParseFailed));
    JS_SetPropertyStr(ctx, obj, "TOO_LARGE", JS_NewString(ctx, sdp::errors::TooLarge));
    JS_SetPropertyStr(ctx, obj, "NO_SESSION", JS_NewString(ctx, sdp::errors::NoSession));
    JS_SetPropertyStr(ctx, obj, "BUILD_FAILED", JS_NewString(ctx, sdp::errors::BuildFailed));
    return obj;
}

constexpr const char* kExportNames[] = {
    "parse",
    "build",
    "getAttr",
    "getAttrs",
    "setAttr",
    "setFlag",
    "addAttr",
    "removeAttr",
    "keepPayloads",
    "reorderPayloads",
    "rtpmaps",
    "candidates",
    "fingerprint",
    "compare",
    "errorCodes",
};

int init_sdp_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "parse", JS_NewCFunction(ctx, js_parse, "parse", 2));
    JS_SetModuleExport(ctx, module, "build", JS_NewCFunction(ctx, js_build, "build", 2));
    JS_SetModuleExport(ctx, module, "getAttr", JS_NewCFunction(ctx, js_get_attr, "getAttr", 2));
    JS_SetModuleExport(ctx, module, "getAttrs", JS_NewCFunction(ctx, js_get_attrs, "getAttrs", 2));
    JS_SetModuleExport(ctx, module, "setAttr", JS_NewCFunction(ctx, js_set_attr, "setAttr", 3));
    JS_SetModuleExport(ctx, module, "setFlag", JS_NewCFunction(ctx, js_set_flag, "setFlag", 2));
    JS_SetModuleExport(ctx, module, "addAttr", JS_NewCFunction(ctx, js_add_attr, "addAttr", 2));
    JS_SetModuleExport(ctx, module, "removeAttr", JS_NewCFunction(ctx, js_remove_attr, "removeAttr", 2));
    JS_SetModuleExport(ctx, module, "keepPayloads", JS_NewCFunction(ctx, js_keep_payloads, "keepPayloads", 2));
    JS_SetModuleExport(ctx, module, "reorderPayloads", JS_NewCFunction(ctx, js_reorder_payloads, "reorderPayloads", 2));
    JS_SetModuleExport(ctx, module, "rtpmaps", JS_NewCFunction(ctx, js_rtpmaps, "rtpmaps", 1));
    JS_SetModuleExport(ctx, module, "candidates", JS_NewCFunction(ctx, js_candidates, "candidates", 1));
    JS_SetModuleExport(ctx, module, "fingerprint", JS_NewCFunction(ctx, js_fingerprint, "fingerprint", 1));
    JS_SetModuleExport(ctx, module, "compare", JS_NewCFunction(ctx, js_compare, "compare", 2));
    JS_SetModuleExport(ctx, module, "errorCodes", JS_NewCFunction(ctx, js_error_codes, "errorCodes", 0));
    return 0;
}

#endif // WL2_HAVE_QUICKJS

} // namespace

wl2::ModuleInfo wl2_sdp_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:sdp", wl2_sdp_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:sdp",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "808fd3a3-3f48-46fc-8767-438d4e5549aa",
        .summary = "Dependency-free SDP parser/builder with fidelity-preserving munging.",
        .api = SdpApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_sdp_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_sdp_module);
    if (!module) {
        return nullptr;
    }
    for (const char* name : kExportNames) {
        JS_AddModuleExport(ctx, module, name);
    }
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_SDP_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:sdp";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "808fd3a3-3f48-46fc-8767-438d4e5549aa";
    out->summary = "Dependency-free SDP parser/builder with fidelity-preserving munging.";
    out->api = SdpApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
