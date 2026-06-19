#include "wl2_membus/wl2_membus.h"

#include "wl2/membus.h"
#include "wl2/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

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

constexpr const char* MembusApi = R"(Exports JavaScript module wl2:membus.

Classes:
  SharedBuffer
  SharedQueue
  VideoBuffer
  AudioBuffer
  CommandChannel
  KeyValueStore
  Selector

All native handles expose close(), isOpen(), and explicit create/attach style
helpers where appropriate. Blocking reads require timeoutMs options or numeric
timeout values.)";

#if WL2_HAVE_QUICKJS
JSClassID shared_buffer_class_id = 0;
JSClassID shared_queue_class_id = 0;
JSClassID video_buffer_class_id = 0;
JSClassID audio_buffer_class_id = 0;
JSClassID command_channel_class_id = 0;
JSClassID key_value_store_class_id = 0;
JSClassID selector_class_id = 0;

template <typename T>
struct NativeBox {
    T value;
};

template <typename T>
void finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<T>*>(JS_GetOpaque(val, 0));
}

void shared_buffer_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::SharedBuffer>*>(JS_GetOpaque(val, shared_buffer_class_id));
}

void shared_queue_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::SharedQueue>*>(JS_GetOpaque(val, shared_queue_class_id));
}

void video_buffer_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::VideoBuffer>*>(JS_GetOpaque(val, video_buffer_class_id));
}

void audio_buffer_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::AudioBuffer>*>(JS_GetOpaque(val, audio_buffer_class_id));
}

void command_channel_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::CommandChannel>*>(JS_GetOpaque(val, command_channel_class_id));
}

void key_value_store_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::KeyValueStore>*>(JS_GetOpaque(val, key_value_store_class_id));
}

void selector_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<NativeBox<wl2::MembusSelector>*>(JS_GetOpaque(val, selector_class_id));
}

template <typename T>
T* get_native(JSContext* ctx, JSValueConst value, JSClassID classId) {
    auto* box = static_cast<NativeBox<T>*>(JS_GetOpaque2(ctx, value, classId));
    return box ? &box->value : nullptr;
}

template <typename T>
JSValue new_native(JSContext* ctx, JSClassID classId, T value) {
    auto* box = new NativeBox<T>{std::move(value)};
    JSValue obj = JS_NewObjectClass(ctx, classId);
    if (JS_IsException(obj)) {
        delete box;
        return obj;
    }
    JS_SetOpaque(obj, box);
    return obj;
}

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

long timeout_ms(JSContext* ctx, JSValueConst value, long fallback = 0) {
    if (JS_IsNumber(value)) {
        int32_t n = 0;
        if (JS_ToInt32(ctx, &n, value) == 0) {
            return std::max(0, n);
        }
    }
    if (JS_IsObject(value)) {
        JSValue timeout = JS_GetPropertyStr(ctx, value, "timeoutMs");
        int32_t n = fallback;
        if (!JS_IsUndefined(timeout) && !JS_IsNull(timeout)) {
            JS_ToInt32(ctx, &n, timeout);
        }
        JS_FreeValue(ctx, timeout);
        return std::max(0, n);
    }
    return fallback;
}

bool js_bool_prop(JSContext* ctx, JSValueConst obj, const char* name, bool fallback) {
    if (!JS_IsObject(obj)) {
        return fallback;
    }
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    bool out = JS_ToBool(ctx, value) != 0;
    JS_FreeValue(ctx, value);
    return out;
}

// Drop any exception left pending by a probing native call such as
// JS_GetArrayBuffer or JS_GetTypedArrayBuffer when the value is not of the
// expected type. payload_string deliberately tries several representations, so
// a type mismatch is expected rather than an error to surface to the caller.
void clear_pending_exception(JSContext* ctx) {
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);
}

// Read the backing bytes of an ArrayBuffer. Sets ok=true on success. On a type
// mismatch the probing exception is cleared and ok is left false.
std::string array_buffer_bytes(JSContext* ctx, JSValueConst value, bool& ok) {
    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, value);
    if (bytes) {
        ok = true;
        return std::string(reinterpret_cast<const char*>(bytes), byteLength);
    }
    clear_pending_exception(ctx);
    ok = false;
    return {};
}

// Convert a JavaScript byte payload into raw bytes. Accepts strings,
// ArrayBuffer, any TypedArray/DataView view (Uint8Array, etc.), and objects
// exposing a binary-safe arrayBuffer()/text() method such as wl2.Buffer.
std::string payload_string(JSContext* ctx, JSValueConst value) {
    // Strings: take the raw UTF-8 bytes directly.
    if (JS_IsString(value)) {
        return js_string(ctx, value);
    }

    // ArrayBuffer: read its backing store directly.
    {
        bool ok = false;
        std::string out = array_buffer_bytes(ctx, value, ok);
        if (ok) {
            return out;
        }
    }

    // TypedArray / DataView (Uint8Array, etc.): resolve to the backing buffer
    // and honor the view's byte offset and length.
    {
        size_t byteOffset = 0;
        size_t byteLength = 0;
        size_t bytesPerElement = 0;
        JSValue arrayBuffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElement);
        if (!JS_IsException(arrayBuffer)) {
            size_t bufferLength = 0;
            uint8_t* bytes = JS_GetArrayBuffer(ctx, &bufferLength, arrayBuffer);
            std::string out;
            if (bytes && byteOffset + byteLength <= bufferLength) {
                out.assign(reinterpret_cast<const char*>(bytes) + byteOffset, byteLength);
            } else {
                clear_pending_exception(ctx);
            }
            JS_FreeValue(ctx, arrayBuffer);
            return out;
        }
        clear_pending_exception(ctx);
    }

    // wl2.Buffer and similar: prefer a binary-safe arrayBuffer(), then fall back
    // to text().
    if (JS_IsObject(value)) {
        JSValue arrayBufferFn = JS_GetPropertyStr(ctx, value, "arrayBuffer");
        if (JS_IsFunction(ctx, arrayBufferFn)) {
            JSValue result = JS_Call(ctx, arrayBufferFn, value, 0, nullptr);
            JS_FreeValue(ctx, arrayBufferFn);
            if (!JS_IsException(result)) {
                bool ok = false;
                std::string out = array_buffer_bytes(ctx, result, ok);
                JS_FreeValue(ctx, result);
                if (ok) {
                    return out;
                }
            } else {
                JS_FreeValue(ctx, result);
                clear_pending_exception(ctx);
            }
        } else {
            JS_FreeValue(ctx, arrayBufferFn);
        }

        JSValue text = JS_GetPropertyStr(ctx, value, "text");
        if (JS_IsFunction(ctx, text)) {
            JSValue result = JS_Call(ctx, text, value, 0, nullptr);
            JS_FreeValue(ctx, text);
            if (!JS_IsException(result)) {
                auto out = js_string(ctx, result);
                JS_FreeValue(ctx, result);
                return out;
            }
            JS_FreeValue(ctx, result);
            clear_pending_exception(ctx);
            return {};
        }
        JS_FreeValue(ctx, text);
    }

    return js_string(ctx, value);
}

JSValue throw_error(JSContext* ctx, const wl2::Error& error) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "MembusError"));
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, error.code().c_str()));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, error.message().c_str()));
    return JS_Throw(ctx, err);
}

wl2::Result<void> authorize_shared_memory(JSContext* ctx, std::string_view name) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return wl2::Error("shared_memory_denied", "Shared-memory access is not permitted without a runtime policy");
    }
    return runtime->authorizeSharedMemory(name);
}

JSValue make_buffer(JSContext* ctx, std::string_view bytes) {
    JSValue arrayBuffer = JS_NewArrayBufferCopy(
        ctx,
        reinterpret_cast<const uint8_t*>(bytes.data()),
        bytes.size());
    if (JS_IsException(arrayBuffer)) {
        return arrayBuffer;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    JS_FreeValue(ctx, global);
    JSValue bufferNamespace = JS_GetPropertyStr(ctx, wl2, "buffer");
    JS_FreeValue(ctx, wl2);
    JSValue fromArrayBuffer = JS_GetPropertyStr(ctx, bufferNamespace, "fromArrayBuffer");
    JSValue args[] = {arrayBuffer};
    JSValue result = JS_Call(ctx, fromArrayBuffer, bufferNamespace, 1, args);
    JS_FreeValue(ctx, fromArrayBuffer);
    JS_FreeValue(ctx, bufferNamespace);
    JS_FreeValue(ctx, arrayBuffer);
    return result;
}

JSValue read_result(JSContext* ctx, const wl2::MembusReadResult& result) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "payload", make_buffer(ctx, result.payload));
    JS_SetPropertyStr(ctx, obj, "overrun", JS_NewBool(ctx, result.overrun));
    JS_SetPropertyStr(ctx, obj, "empty", JS_NewBool(ctx, result.payload.empty()));
    return obj;
}

template <typename T>
JSValue bool_method(JSContext* ctx, JSValueConst thisVal, JSClassID classId, bool (T::*fn)() const) {
    auto* native = get_native<T>(ctx, thisVal, classId);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, (native->*fn)());
}

JSValue sb_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "SharedBuffer.create(name, size, options) requires name and size");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 0;
    JS_ToInt64(ctx, &size, argv[1]);
    bool replace = argc > 2 ? js_bool_prop(ctx, argv[2], "replaceExisting", true) : true;
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::SharedBuffer::create(name, static_cast<size_t>(std::max<int64_t>(0, size)), replace);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, shared_buffer_class_id, std::move(result.value()));
}

JSValue sb_attach(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "SharedBuffer.attach(name, size) requires name");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 0;
    if (argc > 1) {
        JS_ToInt64(ctx, &size, argv[1]);
    }
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::SharedBuffer::attach(name, static_cast<size_t>(std::max<int64_t>(0, size)));
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, shared_buffer_class_id, std::move(result.value()));
}

JSValue sb_write(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::SharedBuffer>(ctx, thisVal, shared_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto payload = argc > 0 ? payload_string(ctx, argv[0]) : std::string{};
    auto result = native->write(payload);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_NewInt64(ctx, static_cast<int64_t>(result.value()));
}

JSValue sb_read(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::SharedBuffer>(ctx, thisVal, shared_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t maxBytes = static_cast<int64_t>(wl2::SharedBuffer::AllBytes);
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        JS_ToInt64(ctx, &maxBytes, argv[0]);
    }
    auto result = native->read(static_cast<size_t>(maxBytes));
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return make_buffer(ctx, result.value());
}

JSValue sb_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::SharedBuffer>(ctx, thisVal, shared_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->close();
    return JS_UNDEFINED;
}

JSValue sb_is_open(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    return bool_method(ctx, thisVal, shared_buffer_class_id, &wl2::SharedBuffer::isOpen);
}

JSValue sb_existing(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    return bool_method(ctx, thisVal, shared_buffer_class_id, &wl2::SharedBuffer::existing);
}

JSValue sq_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "SharedQueue.create(name, size, writable) requires name, size, writable");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 0;
    JS_ToInt64(ctx, &size, argv[1]);
    bool writable = JS_ToBool(ctx, argv[2]) != 0;
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::SharedQueue::create(name, static_cast<size_t>(std::max<int64_t>(0, size)), writable);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, shared_queue_class_id, std::move(result.value()));
}

JSValue sq_attach(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "SharedQueue.attach(name, size, writable) requires name, size, writable");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 0;
    JS_ToInt64(ctx, &size, argv[1]);
    bool writable = JS_ToBool(ctx, argv[2]) != 0;
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::SharedQueue::attach(name, static_cast<size_t>(std::max<int64_t>(0, size)), writable);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, shared_queue_class_id, std::move(result.value()));
}

JSValue sq_write(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::SharedQueue>(ctx, thisVal, shared_queue_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto result = native->write(argc > 0 ? payload_string(ctx, argv[0]) : std::string{});
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_UNDEFINED;
}

JSValue sq_read(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::SharedQueue>(ctx, thisVal, shared_queue_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto result = native->readWithStatus(std::chrono::milliseconds{timeout_ms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0)});
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return read_result(ctx, result.value());
}

JSValue sq_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::SharedQueue>(ctx, thisVal, shared_queue_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, native->poll());
}

JSValue sq_session(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::SharedQueue>(ctx, thisVal, shared_queue_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewInt64(ctx, native->sessionId());
}

JSValue sq_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::SharedQueue>(ctx, thisVal, shared_queue_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->close();
    return JS_UNDEFINED;
}

JSValue sq_is_open(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    return bool_method(ctx, thisVal, shared_queue_class_id, &wl2::SharedQueue::isOpen);
}

JSValue sq_existing(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    return bool_method(ctx, thisVal, shared_queue_class_id, &wl2::SharedQueue::existing);
}

JSValue vb_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 5) {
        return JS_ThrowTypeError(ctx, "VideoBuffer.create(name, width, height, fps, buffers) requires five arguments");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t width = 0;
    int64_t height = 0;
    int64_t fps = 0;
    int64_t buffers = 0;
    JS_ToInt64(ctx, &width, argv[1]);
    JS_ToInt64(ctx, &height, argv[2]);
    JS_ToInt64(ctx, &fps, argv[3]);
    JS_ToInt64(ctx, &buffers, argv[4]);
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::VideoBuffer::create(name, width, height, fps, buffers);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, video_buffer_class_id, std::move(result.value()));
}

JSValue vb_open_existing(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "VideoBuffer.openExisting(name) requires name");
    }
    auto name = js_string(ctx, argv[0]);
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::VideoBuffer::openExisting(name);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, video_buffer_class_id, std::move(result.value()));
}

JSValue vb_fill(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::VideoBuffer>(ctx, thisVal, video_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t index = 0;
    int32_t value = 0;
    JS_ToInt64(ctx, &index, argc > 0 ? argv[0] : JS_UNDEFINED);
    JS_ToInt32(ctx, &value, argc > 1 ? argv[1] : JS_UNDEFINED);
    auto result = native->fill(index, value);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_UNDEFINED;
}

JSValue vb_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::VideoBuffer>(ctx, thisVal, video_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t index = 0;
    JS_ToInt64(ctx, &index, argc > 0 ? argv[0] : JS_UNDEFINED);
    auto result = native->frame(index);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    const auto& frame = result.value();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt64(ctx, frame.width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt64(ctx, frame.height));
    JS_SetPropertyStr(ctx, obj, "scanWidth", JS_NewInt64(ctx, frame.scanWidth));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(frame.size)));
    JS_SetPropertyStr(ctx, obj, "data", make_buffer(ctx, std::string_view(frame.data, frame.size)));
    return obj;
}

JSValue vb_meta(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::VideoBuffer>(ctx, thisVal, video_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt64(ctx, native->width()));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt64(ctx, native->height()));
    JS_SetPropertyStr(ctx, obj, "bitsPerPixel", JS_NewInt64(ctx, native->bitsPerPixel()));
    JS_SetPropertyStr(ctx, obj, "bytesPerPixel", JS_NewInt64(ctx, native->bytesPerPixel()));
    JS_SetPropertyStr(ctx, obj, "fps", JS_NewInt64(ctx, native->fps()));
    JS_SetPropertyStr(ctx, obj, "buffers", JS_NewInt64(ctx, native->buffers()));
    JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, native->sequence()));
    JS_SetPropertyStr(ctx, obj, "sessionId", JS_NewInt64(ctx, native->sessionId()));
    JS_SetPropertyStr(ctx, obj, "formatName", JS_NewString(ctx, native->formatName().c_str()));
    return obj;
}

JSValue vb_next(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::VideoBuffer>(ctx, thisVal, video_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t increment = 1;
    if (argc > 0) {
        JS_ToInt64(ctx, &increment, argv[0]);
    }
    return JS_NewInt64(ctx, native->next(increment));
}

JSValue vb_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::VideoBuffer>(ctx, thisVal, video_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->close();
    return JS_UNDEFINED;
}

JSValue ab_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 6) {
        return JS_ThrowTypeError(ctx, "AudioBuffer.create(name, channels, bitsPerSample, sampleRate, fps, buffers) requires six arguments");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t channels = 0;
    int64_t bits = 0;
    int64_t sampleRate = 0;
    int64_t fps = 0;
    int64_t buffers = 0;
    JS_ToInt64(ctx, &channels, argv[1]);
    JS_ToInt64(ctx, &bits, argv[2]);
    JS_ToInt64(ctx, &sampleRate, argv[3]);
    JS_ToInt64(ctx, &fps, argv[4]);
    JS_ToInt64(ctx, &buffers, argv[5]);
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::AudioBuffer::create(name, channels, bits, sampleRate, fps, buffers);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, audio_buffer_class_id, std::move(result.value()));
}

JSValue ab_open_existing(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "AudioBuffer.openExisting(name) requires name");
    }
    auto name = js_string(ctx, argv[0]);
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::AudioBuffer::openExisting(name);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, audio_buffer_class_id, std::move(result.value()));
}

JSValue ab_fill(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::AudioBuffer>(ctx, thisVal, audio_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t index = 0;
    int32_t value = 0;
    JS_ToInt64(ctx, &index, argc > 0 ? argv[0] : JS_UNDEFINED);
    JS_ToInt32(ctx, &value, argc > 1 ? argv[1] : JS_UNDEFINED);
    auto result = native->fill(index, value);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_UNDEFINED;
}

JSValue ab_buffer(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::AudioBuffer>(ctx, thisVal, audio_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t index = 0;
    JS_ToInt64(ctx, &index, argc > 0 ? argv[0] : JS_UNDEFINED);
    auto result = native->buffer(index);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    const auto& buffer = result.value();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt64(ctx, buffer.channels));
    JS_SetPropertyStr(ctx, obj, "bitsPerSample", JS_NewInt64(ctx, buffer.bitsPerSample));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(buffer.size)));
    JS_SetPropertyStr(ctx, obj, "data", make_buffer(ctx, std::string_view(buffer.data, buffer.size)));
    return obj;
}

JSValue ab_meta(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::AudioBuffer>(ctx, thisVal, audio_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt64(ctx, native->channels()));
    JS_SetPropertyStr(ctx, obj, "bitsPerSample", JS_NewInt64(ctx, native->bitsPerSample()));
    JS_SetPropertyStr(ctx, obj, "bytesPerSample", JS_NewInt64(ctx, native->bytesPerSample()));
    JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt64(ctx, native->sampleRate()));
    JS_SetPropertyStr(ctx, obj, "fps", JS_NewInt64(ctx, native->fps()));
    JS_SetPropertyStr(ctx, obj, "buffers", JS_NewInt64(ctx, native->buffers()));
    JS_SetPropertyStr(ctx, obj, "bufferSize", JS_NewInt64(ctx, native->bufferSize()));
    JS_SetPropertyStr(ctx, obj, "sequence", JS_NewInt64(ctx, native->sequence()));
    JS_SetPropertyStr(ctx, obj, "sessionId", JS_NewInt64(ctx, native->sessionId()));
    JS_SetPropertyStr(ctx, obj, "formatName", JS_NewString(ctx, native->formatName().c_str()));
    return obj;
}

JSValue ab_next(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::AudioBuffer>(ctx, thisVal, audio_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t increment = 1;
    if (argc > 0) {
        JS_ToInt64(ctx, &increment, argv[0]);
    }
    return JS_NewInt64(ctx, native->next(increment));
}

JSValue ab_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::AudioBuffer>(ctx, thisVal, audio_buffer_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->close();
    return JS_UNDEFINED;
}

JSValue cc_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "CommandChannel.create(name, size, reader) requires name, size, reader");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 0;
    JS_ToInt64(ctx, &size, argv[1]);
    bool reader = JS_ToBool(ctx, argv[2]) != 0;
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::CommandChannel::create(name, static_cast<size_t>(std::max<int64_t>(0, size)), reader);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, command_channel_class_id, std::move(result.value()));
}

JSValue cc_attach(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "CommandChannel.attach(name, size, reader) requires name and size");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t size = 0;
    JS_ToInt64(ctx, &size, argv[1]);
    bool reader = argc > 2 ? JS_ToBool(ctx, argv[2]) != 0 : false;
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::CommandChannel::attach(name, static_cast<size_t>(std::max<int64_t>(0, size)), reader);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, command_channel_class_id, std::move(result.value()));
}

JSValue cc_write(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::CommandChannel>(ctx, thisVal, command_channel_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto result = native->write(argc > 0 ? payload_string(ctx, argv[0]) : std::string{});
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_UNDEFINED;
}

JSValue cc_read(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::CommandChannel>(ctx, thisVal, command_channel_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto result = native->read(std::chrono::milliseconds{timeout_ms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0)});
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return read_result(ctx, result.value());
}

JSValue cc_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::CommandChannel>(ctx, thisVal, command_channel_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, native->poll());
}

JSValue cc_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::CommandChannel>(ctx, thisVal, command_channel_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->close();
    return JS_UNDEFINED;
}

JSValue kv_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "KeyValueStore.create(name, count, maxNameLength, maxValueLength) requires four arguments");
    }
    auto name = js_string(ctx, argv[0]);
    int64_t count = 0;
    int64_t maxName = 0;
    int64_t maxValue = 0;
    JS_ToInt64(ctx, &count, argv[1]);
    JS_ToInt64(ctx, &maxName, argv[2]);
    JS_ToInt64(ctx, &maxValue, argv[3]);
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::KeyValueStore::create(name, count, maxName, maxValue, true);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, key_value_store_class_id, std::move(result.value()));
}

JSValue kv_open(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "KeyValueStore.open(name) requires name");
    }
    auto name = js_string(ctx, argv[0]);
    if (auto allowed = authorize_shared_memory(ctx, name); !allowed) {
        return throw_error(ctx, allowed.error());
    }
    auto result = wl2::KeyValueStore::open(name);
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return new_native(ctx, key_value_store_class_id, std::move(result.value()));
}

JSValue kv_set_name(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::KeyValueStore>(ctx, thisVal, key_value_store_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t index = 0;
    JS_ToInt64(ctx, &index, argc > 0 ? argv[0] : JS_UNDEFINED);
    auto result = native->setName(index, argc > 1 ? js_string(ctx, argv[1]) : std::string{});
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_UNDEFINED;
}

JSValue kv_set_value(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::KeyValueStore>(ctx, thisVal, key_value_store_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "setValue(indexOrName, value) requires two arguments");
    }
    auto value = js_string(ctx, argv[1]);
    wl2::Result<void> result;
    if (JS_IsNumber(argv[0])) {
        int64_t index = 0;
        JS_ToInt64(ctx, &index, argv[0]);
        result = native->setValue(index, value);
    } else {
        result = native->setValue(js_string(ctx, argv[0]), value);
    }
    if (!result) {
        return throw_error(ctx, result.error());
    }
    return JS_UNDEFINED;
}

JSValue kv_value(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_native<wl2::KeyValueStore>(ctx, thisVal, key_value_store_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "value(indexOrName) requires one argument");
    }
    if (JS_IsNumber(argv[0])) {
        int64_t index = 0;
        JS_ToInt64(ctx, &index, argv[0]);
        auto result = native->value(index);
        if (!result) {
            return throw_error(ctx, result.error());
        }
        return JS_NewStringLen(ctx, result.value().data(), result.value().size());
    } else {
        auto result = native->value(js_string(ctx, argv[0]));
        if (!result) {
            return throw_error(ctx, result.error());
        }
        return JS_NewStringLen(ctx, result.value().data(), result.value().size());
    }
}

JSValue kv_all(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::KeyValueStore>(ctx, thisVal, key_value_store_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto result = native->all();
    if (!result) {
        return throw_error(ctx, result.error());
    }
    JSValue obj = JS_NewObject(ctx);
    for (const auto& [key, value] : result.value()) {
        JS_SetPropertyStr(ctx, obj, key.c_str(), JS_NewStringLen(ctx, value.data(), value.size()));
    }
    return obj;
}

JSValue kv_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_native<wl2::KeyValueStore>(ctx, thisVal, key_value_store_class_id);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->close();
    return JS_UNDEFINED;
}

JSValue selector_ctor(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    (void)newTarget;
    (void)argc;
    (void)argv;
    return new_native(ctx, selector_class_id, wl2::MembusSelector{});
}

JSValue selector_wait(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1 || !JS_IsArray(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Selector.wait(predicates, options) requires an array of functions");
    }

    uint32_t length = 0;
    JSValue lengthValue = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &length, lengthValue);
    JS_FreeValue(ctx, lengthValue);

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{timeout_ms(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 0)};

    do {
        for (uint32_t i = 0; i < length; ++i) {
            JSValue predicate = JS_GetPropertyUint32(ctx, argv[0], i);
            if (!JS_IsFunction(ctx, predicate)) {
                JS_FreeValue(ctx, predicate);
                return JS_ThrowTypeError(ctx, "Selector predicates must be functions");
            }
            JSValue result = JS_Call(ctx, predicate, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, predicate);
            if (JS_IsException(result)) {
                return result;
            }
            const bool ready = JS_ToBool(ctx, result) != 0;
            JS_FreeValue(ctx, result);
            if (ready) {
                return JS_NewInt32(ctx, static_cast<int32_t>(i));
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (true);

    return JS_NewInt32(ctx, -1);
}

void add_class(JSContext* ctx, JSClassID* id, const char* name, JSClassFinalizer* finalizer, JSValue ctor) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (*id == 0) {
        JS_NewClassID(id);
    }
    JSClassDef def{};
    def.class_name = name;
    def.finalizer = finalizer;
    JS_NewClass(rt, *id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, *id, proto);
    JS_SetConstructor(ctx, ctor, proto);
}

void set_method(JSContext* ctx, JSClassID classId, const char* name, JSCFunction* fn, int argc) {
    JSValue proto = JS_GetClassProto(ctx, classId);
    JS_SetPropertyStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, argc));
    JS_FreeValue(ctx, proto);
}

int init_membus_module(JSContext* ctx, JSModuleDef* module) {
    JSValue sharedBuffer = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "use SharedBuffer.create() or SharedBuffer.attach()"); }, "SharedBuffer", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, sharedBuffer, "create", JS_NewCFunction(ctx, sb_create, "create", 3));
    JS_SetPropertyStr(ctx, sharedBuffer, "attach", JS_NewCFunction(ctx, sb_attach, "attach", 2));
    add_class(ctx, &shared_buffer_class_id, "SharedBuffer", shared_buffer_finalizer, sharedBuffer);
    set_method(ctx, shared_buffer_class_id, "write", sb_write, 1);
    set_method(ctx, shared_buffer_class_id, "read", sb_read, 1);
    set_method(ctx, shared_buffer_class_id, "close", sb_close, 0);
    set_method(ctx, shared_buffer_class_id, "isOpen", sb_is_open, 0);
    set_method(ctx, shared_buffer_class_id, "existing", sb_existing, 0);

    JSValue sharedQueue = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "use SharedQueue.create() or SharedQueue.attach()"); }, "SharedQueue", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, sharedQueue, "create", JS_NewCFunction(ctx, sq_create, "create", 3));
    JS_SetPropertyStr(ctx, sharedQueue, "attach", JS_NewCFunction(ctx, sq_attach, "attach", 3));
    add_class(ctx, &shared_queue_class_id, "SharedQueue", shared_queue_finalizer, sharedQueue);
    set_method(ctx, shared_queue_class_id, "write", sq_write, 1);
    set_method(ctx, shared_queue_class_id, "read", sq_read, 1);
    set_method(ctx, shared_queue_class_id, "close", sq_close, 0);
    set_method(ctx, shared_queue_class_id, "poll", sq_poll, 0);
    set_method(ctx, shared_queue_class_id, "sessionId", sq_session, 0);
    set_method(ctx, shared_queue_class_id, "isOpen", sq_is_open, 0);
    set_method(ctx, shared_queue_class_id, "existing", sq_existing, 0);

    JSValue videoBuffer = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "use VideoBuffer.create() or VideoBuffer.openExisting()"); }, "VideoBuffer", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, videoBuffer, "create", JS_NewCFunction(ctx, vb_create, "create", 5));
    JS_SetPropertyStr(ctx, videoBuffer, "openExisting", JS_NewCFunction(ctx, vb_open_existing, "openExisting", 1));
    add_class(ctx, &video_buffer_class_id, "VideoBuffer", video_buffer_finalizer, videoBuffer);
    set_method(ctx, video_buffer_class_id, "fill", vb_fill, 2);
    set_method(ctx, video_buffer_class_id, "frame", vb_frame, 1);
    set_method(ctx, video_buffer_class_id, "metadata", vb_meta, 0);
    set_method(ctx, video_buffer_class_id, "next", vb_next, 1);
    set_method(ctx, video_buffer_class_id, "close", vb_close, 0);

    JSValue audioBuffer = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "use AudioBuffer.create() or AudioBuffer.openExisting()"); }, "AudioBuffer", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, audioBuffer, "create", JS_NewCFunction(ctx, ab_create, "create", 6));
    JS_SetPropertyStr(ctx, audioBuffer, "openExisting", JS_NewCFunction(ctx, ab_open_existing, "openExisting", 1));
    add_class(ctx, &audio_buffer_class_id, "AudioBuffer", audio_buffer_finalizer, audioBuffer);
    set_method(ctx, audio_buffer_class_id, "fill", ab_fill, 2);
    set_method(ctx, audio_buffer_class_id, "buffer", ab_buffer, 1);
    set_method(ctx, audio_buffer_class_id, "metadata", ab_meta, 0);
    set_method(ctx, audio_buffer_class_id, "next", ab_next, 1);
    set_method(ctx, audio_buffer_class_id, "close", ab_close, 0);

    JSValue commandChannel = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "use CommandChannel.create() or CommandChannel.attach()"); }, "CommandChannel", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, commandChannel, "create", JS_NewCFunction(ctx, cc_create, "create", 3));
    JS_SetPropertyStr(ctx, commandChannel, "attach", JS_NewCFunction(ctx, cc_attach, "attach", 3));
    add_class(ctx, &command_channel_class_id, "CommandChannel", command_channel_finalizer, commandChannel);
    set_method(ctx, command_channel_class_id, "write", cc_write, 1);
    set_method(ctx, command_channel_class_id, "read", cc_read, 1);
    set_method(ctx, command_channel_class_id, "poll", cc_poll, 0);
    set_method(ctx, command_channel_class_id, "close", cc_close, 0);

    JSValue keyValueStore = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) { return JS_ThrowTypeError(c, "use KeyValueStore.create() or KeyValueStore.open()"); }, "KeyValueStore", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, keyValueStore, "create", JS_NewCFunction(ctx, kv_create, "create", 4));
    JS_SetPropertyStr(ctx, keyValueStore, "open", JS_NewCFunction(ctx, kv_open, "open", 1));
    add_class(ctx, &key_value_store_class_id, "KeyValueStore", key_value_store_finalizer, keyValueStore);
    set_method(ctx, key_value_store_class_id, "setName", kv_set_name, 2);
    set_method(ctx, key_value_store_class_id, "setValue", kv_set_value, 2);
    set_method(ctx, key_value_store_class_id, "value", kv_value, 1);
    set_method(ctx, key_value_store_class_id, "all", kv_all, 0);
    set_method(ctx, key_value_store_class_id, "close", kv_close, 0);

    JSValue selector = JS_NewCFunction2(ctx, selector_ctor, "Selector", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, selector, "wait", JS_NewCFunction(ctx, selector_wait, "wait", 2));
    add_class(ctx, &selector_class_id, "Selector", selector_finalizer, selector);

    JS_SetModuleExport(ctx, module, "SharedBuffer", sharedBuffer);
    JS_SetModuleExport(ctx, module, "SharedQueue", sharedQueue);
    JS_SetModuleExport(ctx, module, "VideoBuffer", videoBuffer);
    JS_SetModuleExport(ctx, module, "AudioBuffer", audioBuffer);
    JS_SetModuleExport(ctx, module, "CommandChannel", commandChannel);
    JS_SetModuleExport(ctx, module, "KeyValueStore", keyValueStore);
    JS_SetModuleExport(ctx, module, "Selector", selector);
    JS_SetModuleExport(ctx, module, "hasV12Surface", JS_NewBool(ctx, wl2::libmembusHasV12Surface()));
    return 0;
}
#endif

} // namespace

wl2::ModuleInfo wl2_membus_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:membus", wl2_membus_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:membus",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "2a05080e-c801-4a84-8fc3-d0e2361fdb64",
        .summary = "JavaScript bindings for Winglib2 libmembus shared-memory wrappers.",
        .api = MembusApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_membus_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_membus_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "SharedBuffer");
    JS_AddModuleExport(ctx, module, "SharedQueue");
    JS_AddModuleExport(ctx, module, "VideoBuffer");
    JS_AddModuleExport(ctx, module, "AudioBuffer");
    JS_AddModuleExport(ctx, module, "CommandChannel");
    JS_AddModuleExport(ctx, module, "KeyValueStore");
    JS_AddModuleExport(ctx, module, "Selector");
    JS_AddModuleExport(ctx, module, "hasV12Surface");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_MEMBUS_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:membus";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "2a05080e-c801-4a84-8fc3-d0e2361fdb64";
    out->summary = "JavaScript bindings for Winglib2 libmembus shared-memory wrappers.";
    out->api = MembusApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
