#include "wl2_media/wl2_media.h"

#include "wl2_media/schema.h"

#include "wl2/runtime.h"

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

namespace media = wl2::media;

constexpr const char* MediaApi = R"(Exports JavaScript module wl2:media.

Backend-agnostic media schemas shared by wl2:gstreamer, wl2:ffmpeg, and future
media backends. Pure schema, validation, and timestamp conversion; no pipelines,
codecs, devices, or transport.

Functions:
  schemaVersion()                         -> { packet, stream }
  StreamDescriptor(descriptor)            -> normalized descriptor (throws on invalid)
  validateStreamDescriptor(descriptor)    -> normalized descriptor (throws on invalid)
  PacketMetadata(metadata)                -> normalized metadata (throws on invalid)
  validatePacketMetadata(metadata)        -> normalized metadata (throws on invalid)
  normalizePacketMetadata(metadata, opt?) -> normalized metadata with defaults filled
  VideoFormat(format)                     -> normalized { format, width, height, framerate }
  AudioFormat(format)                     -> normalized { format, rate, channels, layout }
  backpressureProfiles()                  -> [ "record", "transcode", "preview", "relay" ]
  errorCodes()                            -> { INVALID_ARGUMENT, INVALID_SCHEMA, ... }
  Timestamp.convert(value, fromTimeBase, toTimeBase) -> number

Time bases are "num/den" strings; nanoseconds ("1/1000000000") is the boundary
default. Errors use the shared MediaError shape with stable media_* codes.)";

#if WL2_HAVE_QUICKJS

JSValue throw_media_error(JSContext* ctx, const char* code, const char* operation, const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "MediaError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_media"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}

// Returns false and leaves the value untouched when the property is absent
// (undefined/null). Leaves a pending exception only on conversion failure.
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

JSValue stream_to_js(JSContext* ctx, const media::StreamDescriptor& d) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "schema", JS_NewInt32(ctx, d.schema));
    JS_SetPropertyStr(ctx, obj, "mediaType", JS_NewString(ctx, d.mediaType.c_str()));
    JS_SetPropertyStr(ctx, obj, "codec", JS_NewString(ctx, d.codec.c_str()));
    JS_SetPropertyStr(ctx, obj, "caps", JS_NewString(ctx, d.caps.c_str()));
    JS_SetPropertyStr(ctx, obj, "streamFormat", JS_NewString(ctx, d.streamFormat.c_str()));
    JS_SetPropertyStr(ctx, obj, "alignment", JS_NewString(ctx, d.alignment.c_str()));
    JS_SetPropertyStr(ctx, obj, "track", JS_NewInt64(ctx, d.track));
    return obj;
}

JSValue packet_to_js(JSContext* ctx, const media::PacketMetadata& m) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "schema", JS_NewInt32(ctx, m.schema));
    JS_SetPropertyStr(ctx, obj, "mediaType", JS_NewString(ctx, m.mediaType.c_str()));
    JS_SetPropertyStr(ctx, obj, "codec", JS_NewString(ctx, m.codec.c_str()));
    JS_SetPropertyStr(ctx, obj, "caps", JS_NewString(ctx, m.caps.c_str()));
    JS_SetPropertyStr(ctx, obj, "streamFormat", JS_NewString(ctx, m.streamFormat.c_str()));
    JS_SetPropertyStr(ctx, obj, "alignment", JS_NewString(ctx, m.alignment.c_str()));
    JS_SetPropertyStr(ctx, obj, "track", JS_NewInt64(ctx, m.track));
    JS_SetPropertyStr(ctx, obj, "pts", JS_NewInt64(ctx, m.pts));
    JS_SetPropertyStr(ctx, obj, "dts", JS_NewInt64(ctx, m.dts));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewInt64(ctx, m.duration));
    JS_SetPropertyStr(ctx, obj, "timeBase", JS_NewString(ctx, m.timeBase.c_str()));
    JS_SetPropertyStr(ctx, obj, "flags", JS_NewInt64(ctx, static_cast<int64_t>(m.flags)));
    JS_SetPropertyStr(ctx, obj, "discontinuity", JS_NewBool(ctx, m.discontinuity));
    JS_SetPropertyStr(ctx, obj, "sideData", JS_NewString(ctx, m.sideData.c_str()));
    return obj;
}

// Read a JS object argument into a StreamDescriptor, filling defaults. Returns
// false with a pending MediaError when the argument is not an object.
bool stream_from_js(JSContext* ctx, const char* operation, int argc, JSValueConst* argv, media::StreamDescriptor& out) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        throw_media_error(ctx, media::errors::InvalidArgument, operation,
            std::string(operation) + "(descriptor) requires an object");
        return false;
    }
    int64_t schema = media::kStreamSchema;
    read_int(ctx, argv[0], "schema", schema);
    out.schema = static_cast<int>(schema);
    read_string(ctx, argv[0], "mediaType", out.mediaType);
    read_string(ctx, argv[0], "codec", out.codec);
    read_string(ctx, argv[0], "caps", out.caps);
    read_string(ctx, argv[0], "streamFormat", out.streamFormat);
    read_string(ctx, argv[0], "alignment", out.alignment);
    read_int(ctx, argv[0], "track", out.track);
    return true;
}

bool packet_from_js(JSContext* ctx, const char* operation, int argc, JSValueConst* argv, media::PacketMetadata& out) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        throw_media_error(ctx, media::errors::InvalidArgument, operation,
            std::string(operation) + "(metadata) requires an object");
        return false;
    }
    int64_t schema = media::kPacketSchema;
    read_int(ctx, argv[0], "schema", schema);
    out.schema = static_cast<int>(schema);
    read_string(ctx, argv[0], "mediaType", out.mediaType);
    read_string(ctx, argv[0], "codec", out.codec);
    read_string(ctx, argv[0], "caps", out.caps);
    read_string(ctx, argv[0], "streamFormat", out.streamFormat);
    read_string(ctx, argv[0], "alignment", out.alignment);
    read_int(ctx, argv[0], "track", out.track);
    read_int(ctx, argv[0], "pts", out.pts);
    read_int(ctx, argv[0], "dts", out.dts);
    read_int(ctx, argv[0], "duration", out.duration);
    read_string(ctx, argv[0], "timeBase", out.timeBase);
    int64_t flags = 0;
    if (read_int(ctx, argv[0], "flags", flags)) {
        out.flags = static_cast<uint32_t>(flags);
    }
    read_bool(ctx, argv[0], "discontinuity", out.discontinuity);
    read_string(ctx, argv[0], "sideData", out.sideData);
    return true;
}

JSValue media_schema_version(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "packet", JS_NewInt32(ctx, media::kPacketSchema));
    JS_SetPropertyStr(ctx, obj, "stream", JS_NewInt32(ctx, media::kStreamSchema));
    return obj;
}

JSValue media_stream_descriptor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    media::StreamDescriptor descriptor;
    if (!stream_from_js(ctx, "StreamDescriptor", argc, argv, descriptor)) {
        return JS_EXCEPTION;
    }
    if (auto ok = media::validate(descriptor); !ok) {
        return throw_media_error(ctx, ok.error().code().c_str(), "StreamDescriptor", ok.error().message());
    }
    return stream_to_js(ctx, descriptor);
}

JSValue media_packet_metadata(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    media::PacketMetadata metadata;
    if (!packet_from_js(ctx, "PacketMetadata", argc, argv, metadata)) {
        return JS_EXCEPTION;
    }
    if (auto ok = media::validate(metadata); !ok) {
        return throw_media_error(ctx, ok.error().code().c_str(), "PacketMetadata", ok.error().message());
    }
    return packet_to_js(ctx, metadata);
}

JSValue media_normalize_packet_metadata(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    media::PacketMetadata metadata;
    if (!packet_from_js(ctx, "normalizePacketMetadata", argc, argv, metadata)) {
        return JS_EXCEPTION;
    }
    if (auto ok = media::validate(metadata); !ok) {
        return throw_media_error(ctx, ok.error().code().c_str(), "normalizePacketMetadata", ok.error().message());
    }
    return packet_to_js(ctx, metadata);
}

JSValue media_video_format(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "VideoFormat", "VideoFormat(format) requires an object");
    }
    media::VideoFormat format;
    read_string(ctx, argv[0], "format", format.format);
    read_int(ctx, argv[0], "width", format.width);
    read_int(ctx, argv[0], "height", format.height);
    read_string(ctx, argv[0], "framerate", format.framerate);
    if (format.format.empty()) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "VideoFormat", "VideoFormat requires a format string");
    }
    if (format.width < 0 || format.height < 0) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "VideoFormat", "VideoFormat width/height must be non-negative");
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, format.format.c_str()));
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt64(ctx, format.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt64(ctx, format.height));
    JS_SetPropertyStr(ctx, obj, "framerate", JS_NewString(ctx, format.framerate.c_str()));
    return obj;
}

JSValue media_audio_format(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "AudioFormat", "AudioFormat(format) requires an object");
    }
    media::AudioFormat format;
    read_string(ctx, argv[0], "format", format.format);
    read_int(ctx, argv[0], "rate", format.rate);
    read_int(ctx, argv[0], "channels", format.channels);
    read_string(ctx, argv[0], "layout", format.layout);
    if (format.format.empty()) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "AudioFormat", "AudioFormat requires a format string");
    }
    if (format.rate < 0 || format.channels < 0) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "AudioFormat", "AudioFormat rate/channels must be non-negative");
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, format.format.c_str()));
    JS_SetPropertyStr(ctx, obj, "rate", JS_NewInt64(ctx, format.rate));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt64(ctx, format.channels));
    JS_SetPropertyStr(ctx, obj, "layout", JS_NewString(ctx, format.layout.c_str()));
    return obj;
}

JSValue media_backpressure_profiles(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue array = JS_NewArray(ctx);
    const char* names[] = {
        media::backpressure::Record,
        media::backpressure::Transcode,
        media::backpressure::Preview,
        media::backpressure::Relay,
    };
    for (uint32_t i = 0; i < 4; ++i) {
        JS_SetPropertyUint32(ctx, array, i, JS_NewString(ctx, names[i]));
    }
    return array;
}

JSValue media_error_codes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "INVALID_ARGUMENT", JS_NewString(ctx, media::errors::InvalidArgument));
    JS_SetPropertyStr(ctx, obj, "INVALID_SCHEMA", JS_NewString(ctx, media::errors::InvalidSchema));
    JS_SetPropertyStr(ctx, obj, "UNSUPPORTED_MEDIA_TYPE", JS_NewString(ctx, media::errors::UnsupportedMediaType));
    JS_SetPropertyStr(ctx, obj, "INVALID_TIME_BASE", JS_NewString(ctx, media::errors::InvalidTimeBase));
    JS_SetPropertyStr(ctx, obj, "PARSE_FAILED", JS_NewString(ctx, media::errors::ParseFailed));
    return obj;
}

JSValue media_timestamp_convert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) {
        return throw_media_error(ctx, media::errors::InvalidArgument, "Timestamp.convert",
            "Timestamp.convert(value, fromTimeBase, toTimeBase) requires three arguments");
    }
    int64_t value = 0;
    if (JS_ToInt64(ctx, &value, argv[0]) < 0) {
        return JS_EXCEPTION;
    }
    const char* fromText = JS_ToCString(ctx, argv[1]);
    if (!fromText) {
        return JS_EXCEPTION;
    }
    const char* toText = JS_ToCString(ctx, argv[2]);
    if (!toText) {
        JS_FreeCString(ctx, fromText);
        return JS_EXCEPTION;
    }
    std::string from = fromText;
    std::string to = toText;
    JS_FreeCString(ctx, fromText);
    JS_FreeCString(ctx, toText);

    int64_t num = 0;
    int64_t den = 0;
    if (!media::parseTimeBase(from, num, den)) {
        return throw_media_error(ctx, media::errors::InvalidTimeBase, "Timestamp.convert", "Malformed fromTimeBase: " + from);
    }
    if (!media::parseTimeBase(to, num, den)) {
        return throw_media_error(ctx, media::errors::InvalidTimeBase, "Timestamp.convert", "Malformed toTimeBase: " + to);
    }
    return JS_NewInt64(ctx, media::convertTimestamp(value, from, to));
}

JSValue make_timestamp_namespace(JSContext* ctx) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "convert", JS_NewCFunction(ctx, media_timestamp_convert, "convert", 3));
    return obj;
}

// The exported names, kept in one place so the module factory and initializer
// stay in sync.
constexpr const char* kExportNames[] = {
    "schemaVersion",
    "StreamDescriptor",
    "validateStreamDescriptor",
    "PacketMetadata",
    "validatePacketMetadata",
    "normalizePacketMetadata",
    "VideoFormat",
    "AudioFormat",
    "backpressureProfiles",
    "errorCodes",
    "Timestamp",
};

int init_media_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "schemaVersion", JS_NewCFunction(ctx, media_schema_version, "schemaVersion", 0));
    JS_SetModuleExport(ctx, module, "StreamDescriptor", JS_NewCFunction(ctx, media_stream_descriptor, "StreamDescriptor", 1));
    JS_SetModuleExport(ctx, module, "validateStreamDescriptor", JS_NewCFunction(ctx, media_stream_descriptor, "validateStreamDescriptor", 1));
    JS_SetModuleExport(ctx, module, "PacketMetadata", JS_NewCFunction(ctx, media_packet_metadata, "PacketMetadata", 1));
    JS_SetModuleExport(ctx, module, "validatePacketMetadata", JS_NewCFunction(ctx, media_packet_metadata, "validatePacketMetadata", 1));
    JS_SetModuleExport(ctx, module, "normalizePacketMetadata", JS_NewCFunction(ctx, media_normalize_packet_metadata, "normalizePacketMetadata", 2));
    JS_SetModuleExport(ctx, module, "VideoFormat", JS_NewCFunction(ctx, media_video_format, "VideoFormat", 1));
    JS_SetModuleExport(ctx, module, "AudioFormat", JS_NewCFunction(ctx, media_audio_format, "AudioFormat", 1));
    JS_SetModuleExport(ctx, module, "backpressureProfiles", JS_NewCFunction(ctx, media_backpressure_profiles, "backpressureProfiles", 0));
    JS_SetModuleExport(ctx, module, "errorCodes", JS_NewCFunction(ctx, media_error_codes, "errorCodes", 0));
    JS_SetModuleExport(ctx, module, "Timestamp", make_timestamp_namespace(ctx));
    return 0;
}

#endif // WL2_HAVE_QUICKJS

} // namespace

wl2::ModuleInfo wl2_media_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:media", wl2_media_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:media",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "dbea8df8-c257-4e4d-9772-0cb3083ca87f",
        .summary = "Backend-agnostic media schemas shared by Winglib2 media modules.",
        .api = MediaApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_media_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_media_module);
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

#if !WL2_MEDIA_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:media";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "dbea8df8-c257-4e4d-9772-0cb3083ca87f";
    out->summary = "Backend-agnostic media schemas shared by Winglib2 media modules.";
    out->api = MediaApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
