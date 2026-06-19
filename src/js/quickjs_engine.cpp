#include "wl2/js_engine.h"

#include "wl2/buffer.h"
#include "wl2/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif

namespace wl2 {

namespace {

#if WL2_HAVE_QUICKJS
JSClassID buffer_class_id = 0;
JSClassID resource_handle_class_id = 0;
JSClassID thread_worker_class_id = 0;
JSClassID thread_request_class_id = 0;
JSClassID thread_pending_class_id = 0;

struct JsBuffer {
    Buffer buffer;
};

struct JsResourceHandle {
    ResourceHandle handle;
    bool closed = false;
};

struct JsThreadWorker {
    std::shared_ptr<ScriptThread> thread;
    std::string path;
    bool closed = false;
};

struct JsThreadRequest {
    std::optional<ThreadRequest> request;
};

struct JsPendingReply {
    PendingReply pending;
};

JSValue new_buffer_object(JSContext* ctx, Buffer buffer) {
    auto* native = new JsBuffer{std::move(buffer)};
    JSValue obj = JS_NewObjectClass(ctx, buffer_class_id);
    if (JS_IsException(obj)) {
        delete native;
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

JSValue new_buffer_from_bytes(JSContext* ctx, std::span<const std::byte> bytes) {
    return new_buffer_object(ctx, Buffer::copy(bytes));
}

JsBuffer* get_buffer(JSContext* ctx, JSValueConst thisVal) {
    return static_cast<JsBuffer*>(JS_GetOpaque2(ctx, thisVal, buffer_class_id));
}

JsResourceHandle* get_resource_handle(JSContext* ctx, JSValueConst thisVal) {
    return static_cast<JsResourceHandle*>(JS_GetOpaque2(ctx, thisVal, resource_handle_class_id));
}

JsThreadWorker* get_thread_worker(JSContext* ctx, JSValueConst thisVal) {
    return static_cast<JsThreadWorker*>(JS_GetOpaque2(ctx, thisVal, thread_worker_class_id));
}

JsThreadRequest* get_thread_request(JSContext* ctx, JSValueConst thisVal) {
    return static_cast<JsThreadRequest*>(JS_GetOpaque2(ctx, thisVal, thread_request_class_id));
}

JsPendingReply* get_pending_reply(JSContext* ctx, JSValueConst thisVal) {
    return static_cast<JsPendingReply*>(JS_GetOpaque2(ctx, thisVal, thread_pending_class_id));
}

void buffer_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<JsBuffer*>(JS_GetOpaque(val, buffer_class_id));
}

void resource_handle_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<JsResourceHandle*>(JS_GetOpaque(val, resource_handle_class_id));
}

void thread_worker_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<JsThreadWorker*>(JS_GetOpaque(val, thread_worker_class_id));
}

void thread_request_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<JsThreadRequest*>(JS_GetOpaque(val, thread_request_class_id));
}

void thread_pending_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    auto* native = static_cast<JsPendingReply*>(JS_GetOpaque(val, thread_pending_class_id));
    if (native) {
        native->pending.cancel();
    }
    delete native;
}

Runtime* current_runtime(JSContext* ctx) {
    return static_cast<Runtime*>(JS_GetContextOpaque(ctx));
}

std::string js_string(JSContext* ctx, JSValueConst value) {
    size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, value);
    if (!text) {
        return {};
    }
    std::string out(text, length);
    JS_FreeCString(ctx, text);
    return out;
}

std::string js_basename(std::string_view path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(slash + 1));
}

JSValue throw_resource_error(JSContext* ctx, const Error& error) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "ResourceError"));
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, error.code().c_str()));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, error.message().c_str()));
    return JS_Throw(ctx, err);
}

JSValue throw_thread_error(JSContext* ctx, const Error& error) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "ThreadError"));
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, error.code().c_str()));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, error.message().c_str()));
    return JS_Throw(ctx, err);
}

JSValue buffer_ctor(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    (void)newTarget;
    if (argc == 0 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        return new_buffer_object(ctx, Buffer{});
    }

    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, argv[0]);
    if (bytes) {
        return new_buffer_object(ctx,
            Buffer::copy(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes), byteLength)));
    }

    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_ThrowTypeError(ctx, "Buffer input must be a string or ArrayBuffer");
    }
    Buffer buffer = Buffer::fromString(text);
    JS_FreeCString(ctx, text);
    return new_buffer_object(ctx, std::move(buffer));
}

JSValue buffer_from_text(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "fromText(text) requires a string");
    }
    size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!text) {
        return JS_ThrowTypeError(ctx, "fromText(text) requires a string");
    }
    Buffer buffer = Buffer::copy(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text), length));
    JS_FreeCString(ctx, text);
    return new_buffer_object(ctx, std::move(buffer));
}

JSValue buffer_from_array_buffer(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "fromArrayBuffer(arrayBuffer) requires an ArrayBuffer");
    }
    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, argv[0]);
    if (!bytes) {
        return JS_ThrowTypeError(ctx, "fromArrayBuffer(arrayBuffer) requires an ArrayBuffer");
    }
    return new_buffer_object(ctx,
        Buffer::copy(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes), byteLength)));
}

JSValue buffer_is_buffer(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)ctx;
    (void)thisVal;
    if (argc < 1) {
        return JS_FALSE;
    }
    return JS_NewBool(ctx, JS_GetOpaque(argv[0], buffer_class_id) != nullptr);
}

JSValue buffer_get_size(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_buffer(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewUint32(ctx, static_cast<uint32_t>(native->buffer.size()));
}

JSValue new_getter(JSContext* ctx, const char* name, JSValue (*getter)(JSContext*, JSValueConst)) {
    JSCFunctionType function{};
    function.getter = getter;
    return JS_NewCFunction2(ctx, function.generic, name, 0, JS_CFUNC_getter, 0);
}

JSValue buffer_uint8_array(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

JSValue resource_entry_object(JSContext* ctx, const ResourceEntry& entry, bool directory = false) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, entry.name.c_str()));
    auto name = js_basename(entry.name);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, obj, "directory", JS_NewBool(ctx, directory));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewUint32(ctx, static_cast<uint32_t>(entry.originalSize)));
    JS_SetPropertyStr(ctx, obj, "storedSize", JS_NewUint32(ctx, static_cast<uint32_t>(entry.storedSize)));
    JS_SetPropertyStr(ctx, obj, "compression",
        JS_NewString(ctx, entry.compression == ResourceCompression::Stored ? "stored" : "rle"));
    JS_SetPropertyStr(ctx, obj, "compressed", JS_NewBool(ctx, entry.compression != ResourceCompression::Stored));
    JS_SetPropertyStr(ctx, obj, "contentHash", JS_NewString(ctx, entry.contentHash.c_str()));
    JS_SetPropertyStr(ctx, obj, "mimeType", JS_NewString(ctx, entry.mimeType.c_str()));
    JS_SetPropertyStr(ctx, obj, "flags", JS_NewUint32(ctx, entry.flags));
    return obj;
}

JSValue resource_directory_entry_object(JSContext* ctx, const ResourceDirectoryEntry& entry) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, entry.path.c_str()));
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, entry.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "directory", JS_NewBool(ctx, entry.directory));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewUint32(ctx, static_cast<uint32_t>(entry.size)));
    return obj;
}

JSValue resource_directory_entries_array(JSContext* ctx, const std::vector<ResourceDirectoryEntry>& entries) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& entry : entries) {
        JS_SetPropertyUint32(ctx, array, index++, resource_directory_entry_object(ctx, entry));
    }
    return array;
}

JSValue new_resource_handle_object(JSContext* ctx, ResourceHandle handle) {
    auto* native = new JsResourceHandle{std::move(handle), false};
    JSValue obj = JS_NewObjectClass(ctx, resource_handle_class_id);
    if (JS_IsException(obj)) {
        delete native;
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

JSValue resource_handle_get_path(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native || native->closed) {
        return JS_ThrowTypeError(ctx, "ResourceHandle is closed");
    }
    return JS_NewString(ctx, native->handle.entry().name.c_str());
}

JSValue resource_handle_get_size(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native || native->closed) {
        return JS_ThrowTypeError(ctx, "ResourceHandle is closed");
    }
    return JS_NewUint32(ctx, static_cast<uint32_t>(native->handle.size()));
}

JSValue resource_handle_get_compressed(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native || native->closed) {
        return JS_ThrowTypeError(ctx, "ResourceHandle is closed");
    }
    return JS_NewBool(ctx, native->handle.entry().compression != ResourceCompression::Stored);
}

JSValue resource_handle_get_closed(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, native->closed);
}

JSValue resource_handle_stat(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native || native->closed) {
        return JS_ThrowTypeError(ctx, "ResourceHandle is closed");
    }
    return resource_entry_object(ctx, native->handle.entry());
}

JSValue resource_handle_text(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native || native->closed) {
        return JS_ThrowTypeError(ctx, "ResourceHandle is closed");
    }
    auto text = native->handle.text();
    return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue resource_handle_bytes(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_resource_handle(ctx, thisVal);
    if (!native || native->closed) {
        return JS_ThrowTypeError(ctx, "ResourceHandle is closed");
    }
    return new_buffer_from_bytes(ctx, native->handle.bytes());
}

JSValue resource_handle_uint8_array(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JSValue buffer = resource_handle_bytes(ctx, thisVal, argc, argv);
    if (JS_IsException(buffer)) {
        return buffer;
    }
    JSValue result = buffer_uint8_array(ctx, buffer, 0, nullptr);
    JS_FreeValue(ctx, buffer);
    return result;
}

JSValue resource_handle_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)ctx;
    (void)argc;
    (void)argv;
    auto* native = static_cast<JsResourceHandle*>(JS_GetOpaque(thisVal, resource_handle_class_id));
    if (native) {
        native->handle = ResourceHandle{};
        native->closed = true;
    }
    return JS_UNDEFINED;
}

JSValue resources_exists(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "exists(path) requires a path");
    }
    auto* runtime = current_runtime(ctx);
    return JS_NewBool(ctx, runtime && runtime->resources().exists(js_string(ctx, argv[0])));
}

JSValue resources_stat(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "stat(path) requires a path");
    }
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    auto path = js_string(ctx, argv[0]);
    if (auto entry = runtime->resources().entry(path)) {
        return resource_entry_object(ctx, *entry);
    }
    if (runtime->resources().isDirectory(path)) {
        ResourceEntry dirEntry;
        dirEntry.name = path;
        return resource_entry_object(ctx, dirEntry, true);
    }
    return JS_NULL;
}

JSValue resources_list(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "list(path) requires a path");
    }
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    return resource_directory_entries_array(ctx, runtime->resources().list(js_string(ctx, argv[0])));
}

JSValue resources_walk(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "walk(path) requires a path");
    }
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    return resource_directory_entries_array(ctx, runtime->resources().walk(js_string(ctx, argv[0])));
}

JSValue resources_open(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "open(path) requires a path");
    }
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    auto opened = runtime->resources().open(js_string(ctx, argv[0]));
    if (!opened) {
        return throw_resource_error(ctx, opened.error());
    }
    return new_resource_handle_object(ctx, std::move(opened.value()));
}

JSValue resources_read_text(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JSValue handle = resources_open(ctx, thisVal, argc, argv);
    if (JS_IsException(handle)) {
        return handle;
    }
    JSValue text = resource_handle_text(ctx, handle, 0, nullptr);
    JS_FreeValue(ctx, handle);
    return text;
}

JSValue resources_read_bytes(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JSValue handle = resources_open(ctx, thisVal, argc, argv);
    if (JS_IsException(handle)) {
        return handle;
    }
    JSValue bytes = resource_handle_bytes(ctx, handle, 0, nullptr);
    JS_FreeValue(ctx, handle);
    return bytes;
}

JSValue buffer_text(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_buffer(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto text = native->buffer.text();
    return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue buffer_slice(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_buffer(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    int64_t start = 0;
    int64_t length = static_cast<int64_t>(native->buffer.size());
    if (argc > 0 && JS_ToInt64(ctx, &start, argv[0]) != 0) {
        return JS_EXCEPTION;
    }
    if (argc > 1 && JS_ToInt64(ctx, &length, argv[1]) != 0) {
        return JS_EXCEPTION;
    }
    if (start < 0) {
        start = 0;
    }
    if (length < 0) {
        length = 0;
    }
    return new_buffer_object(ctx,
        native->buffer.slice(static_cast<size_t>(start), static_cast<size_t>(length)));
}

JSValue buffer_array_buffer(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_buffer(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto view = native->buffer.read();
    return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(view.data()), view.size());
}

JSValue buffer_uint8_array(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JSValue arrayBuffer = buffer_array_buffer(ctx, thisVal, argc, argv);
    if (JS_IsException(arrayBuffer)) {
        return arrayBuffer;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowTypeError(ctx, "Uint8Array constructor is unavailable");
    }

    JSValue args[] = {arrayBuffer};
    JSValue typedArray = JS_CallConstructor(ctx, ctor, 1, args);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, arrayBuffer);
    return typedArray;
}

JSValue buffer_to_string(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    return buffer_text(ctx, thisVal, argc, argv);
}

JSValue console_log(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    for (int i = 0; i < argc; ++i) {
        const char* text = JS_ToCString(ctx, argv[i]);
        if (i) {
            std::cout << ' ';
        }
        std::cout << (text ? text : "");
        if (text) {
            JS_FreeCString(ctx, text);
        }
    }
    std::cout << std::endl;
    return JS_UNDEFINED;
}

int64_t option_timeout_ms(JSContext* ctx, JSValueConst options, int64_t fallback) {
    if (!JS_IsObject(options)) {
        return fallback;
    }
    JSValue value = JS_GetPropertyStr(ctx, options, "timeoutMs");
    if (JS_IsUndefined(value)) {
        JS_FreeValue(ctx, value);
        return fallback;
    }
    int64_t out = fallback;
    JS_ToInt64(ctx, &out, value);
    JS_FreeValue(ctx, value);
    return out < 0 ? 0 : out;
}

std::string thread_parent_path(std::string_view path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos || slash == 0) {
        return {};
    }
    return std::string(path.substr(0, slash));
}

std::string child_path_from_name(std::string_view parent, std::string name) {
    if (!name.empty() && name.front() == '/') {
        return name;
    }
    std::string out(parent);
    if (out.empty()) {
        out = "/main";
    }
    if (out.back() != '/') {
        out += '/';
    }
    out += name.empty() ? "worker" : name;
    return out;
}

std::string stringify_json(JSContext* ctx, JSValueConst value) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
    JSValue textValue = JS_UNDEFINED;
    if (JS_IsFunction(ctx, stringify)) {
        JSValue args[] = {JS_DupValue(ctx, value)};
        textValue = JS_Call(ctx, stringify, json, 1, args);
        JS_FreeValue(ctx, args[0]);
    }
    JS_FreeValue(ctx, stringify);
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, global);
    if (JS_IsException(textValue) || JS_IsUndefined(textValue)) {
        JS_FreeValue(ctx, textValue);
        return js_string(ctx, value);
    }
    std::string out = js_string(ctx, textValue);
    JS_FreeValue(ctx, textValue);
    return out;
}

std::string js_payload(JSContext* ctx, JSValueConst value) {
    if (auto* native = static_cast<JsBuffer*>(JS_GetOpaque(value, buffer_class_id))) {
        auto view = native->buffer.read();
        return std::string(reinterpret_cast<const char*>(view.data()), view.size());
    }

    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, value);
    if (bytes) {
        return std::string(reinterpret_cast<const char*>(bytes), byteLength);
    }

#ifdef JS_TAG_OBJECT
    size_t byteOffset = 0;
    size_t typedLength = 0;
    size_t bytesPerElement = 0;
    JSValue arrayBuffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &typedLength, &bytesPerElement);
    if (!JS_IsException(arrayBuffer)) {
        size_t arrayBufferLength = 0;
        uint8_t* typedBytes = JS_GetArrayBuffer(ctx, &arrayBufferLength, arrayBuffer);
        std::string out;
        if (typedBytes && byteOffset <= arrayBufferLength) {
            typedLength = std::min(typedLength, arrayBufferLength - byteOffset);
            out.assign(reinterpret_cast<const char*>(typedBytes + byteOffset), typedLength);
        }
        JS_FreeValue(ctx, arrayBuffer);
        if (typedBytes) {
            return out;
        }
    }
    JS_FreeValue(ctx, arrayBuffer);
#endif

    if (JS_IsString(value)) {
        return js_string(ctx, value);
    }
    if (JS_IsObject(value)) {
        return stringify_json(ctx, value);
    }
    return js_string(ctx, value);
}

Message js_message(JSContext* ctx, JSValueConst value, std::string source, std::string destination) {
    Message message;
    message.source = std::move(source);
    message.destination = std::move(destination);
    message.type = "message";

    if (JS_IsObject(value)) {
        JSValue type = JS_GetPropertyStr(ctx, value, "type");
        if (!JS_IsUndefined(type)) {
            message.type = js_string(ctx, type);
        }
        JS_FreeValue(ctx, type);

        JSValue payload = JS_GetPropertyStr(ctx, value, "payload");
        if (!JS_IsUndefined(payload)) {
            message.payload = js_payload(ctx, payload);
        }
        JS_FreeValue(ctx, payload);
        return message;
    }

    message.payload = js_payload(ctx, value);
    return message;
}

const char* reply_status_name(ReplyStatus status) {
    switch (status) {
    case ReplyStatus::Ok:
        return "ok";
    case ReplyStatus::Rejected:
        return "rejected";
    case ReplyStatus::Timeout:
        return "timeout";
    case ReplyStatus::Cancelled:
        return "cancelled";
    case ReplyStatus::Unreachable:
        return "unreachable";
    }
    return "unknown";
}

JSValue thread_reply_object(JSContext* ctx, const ThreadReply& reply) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, reply.ok()));
    JS_SetPropertyStr(ctx, obj, "status", JS_NewString(ctx, reply_status_name(reply.status)));
    JS_SetPropertyStr(ctx, obj, "requestId", JS_NewUint32(ctx, static_cast<uint32_t>(reply.requestId)));
    JS_SetPropertyStr(ctx, obj, "source", JS_NewString(ctx, reply.source.c_str()));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, reply.type.c_str()));
    JS_SetPropertyStr(ctx, obj, "payload", JS_NewStringLen(ctx, reply.payload.data(), reply.payload.size()));
    JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, reply.error.c_str()));
    return obj;
}

JSValue thread_reply_error(JSContext* ctx, const ThreadReply& reply) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "ThreadReplyError"));
    JS_SetPropertyStr(ctx, err, "status", JS_NewString(ctx, reply_status_name(reply.status)));
    JS_SetPropertyStr(ctx, err, "requestId", JS_NewUint32(ctx, static_cast<uint32_t>(reply.requestId)));
    JS_SetPropertyStr(ctx, err, "source", JS_NewString(ctx, reply.source.c_str()));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, reply.error.c_str()));
    return err;
}

JSValue new_pending_reply_object(JSContext* ctx, PendingReply pending) {
    auto* native = new JsPendingReply{std::move(pending)};
    JSValue obj = JS_NewObjectClass(ctx, thread_pending_class_id);
    if (JS_IsException(obj)) {
        delete native;
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

JSValue thread_pending_get_id(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_pending_reply(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewUint32(ctx, static_cast<uint32_t>(native->pending.id()));
}

JSValue thread_pending_get_done(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_pending_reply(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, native->pending.done());
}

JSValue thread_pending_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_pending_reply(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    auto reply = native->pending.poll();
    if (!reply) {
        return JS_NULL;
    }
    return thread_reply_object(ctx, *reply);
}

JSValue thread_pending_wait(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_pending_reply(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    if (argc == 0 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        return thread_reply_object(ctx, native->pending.wait());
    }
    auto reply = native->pending.wait(
        std::chrono::milliseconds(option_timeout_ms(ctx, argv[0], 30000)));
    if (!reply) {
        return JS_NULL;
    }
    return thread_reply_object(ctx, *reply);
}

JSValue thread_pending_cancel(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_pending_reply(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    native->pending.cancel();
    return JS_UNDEFINED;
}

JSValue thread_pending_then(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_pending_reply(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    JSValue onFulfilled = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue onRejected = argc > 1 ? argv[1] : JS_UNDEFINED;
    ThreadReply reply = native->pending.wait();
    if (reply.ok()) {
        if (JS_IsFunction(ctx, onFulfilled)) {
            JSValue arg = thread_reply_object(ctx, reply);
            JSValue result = JS_Call(ctx, onFulfilled, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, arg);
            return result;
        }
        return thread_reply_object(ctx, reply);
    }
    if (JS_IsFunction(ctx, onRejected)) {
        JSValue arg = thread_reply_error(ctx, reply);
        JSValue result = JS_Call(ctx, onRejected, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        return result;
    }
    return JS_Throw(ctx, thread_reply_error(ctx, reply));
}

JSValue new_thread_worker_object(JSContext* ctx, std::shared_ptr<ScriptThread> thread) {
    auto* native = new JsThreadWorker{thread, thread->path(), false};
    JSValue obj = JS_NewObjectClass(ctx, thread_worker_class_id);
    if (JS_IsException(obj)) {
        delete native;
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

JSValue thread_worker_get_path(JSContext* ctx, JSValueConst thisVal) {
    auto* native = get_thread_worker(ctx, thisVal);
    if (!native) {
        return JS_EXCEPTION;
    }
    return JS_NewString(ctx, native->path.c_str());
}

JSValue thread_worker_post(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_thread_worker(ctx, thisVal);
    auto* runtime = current_runtime(ctx);
    if (!native || native->closed || !runtime) {
        return JS_ThrowTypeError(ctx, "ThreadWorker is closed");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "post(message) requires a message");
    }
    auto message = js_message(ctx, argv[0], currentThreadPath(), native->path);
    return JS_NewBool(ctx, runtime->threadTree().send(std::move(message)));
}

JSValue thread_worker_request(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_thread_worker(ctx, thisVal);
    auto* runtime = current_runtime(ctx);
    if (!native || native->closed || !runtime) {
        return JS_ThrowTypeError(ctx, "ThreadWorker is closed");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "request(message, options) requires a message");
    }
    auto message = js_message(ctx, argv[0], currentThreadPath(), native->path);
    auto pending = runtime->threadTree().request(std::move(message),
        std::chrono::milliseconds(option_timeout_ms(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 30000)));
    return thread_reply_object(ctx, pending.wait());
}

JSValue thread_worker_request_pending(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_thread_worker(ctx, thisVal);
    auto* runtime = current_runtime(ctx);
    if (!native || native->closed || !runtime) {
        return JS_ThrowTypeError(ctx, "ThreadWorker is closed");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "requestPending(message, options) requires a message");
    }
    auto message = js_message(ctx, argv[0], currentThreadPath(), native->path);
    auto pending = runtime->threadTree().request(std::move(message),
        std::chrono::milliseconds(option_timeout_ms(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 30000)));
    return new_pending_reply_object(ctx, std::move(pending));
}

JSValue thread_worker_wait(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_thread_worker(ctx, thisVal);
    if (!native || !native->thread) {
        return JS_ThrowTypeError(ctx, "ThreadWorker is unavailable");
    }
    const auto timeout = std::chrono::milliseconds(option_timeout_ms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 30000));
    return JS_NewBool(ctx, native->thread->wait(timeout));
}

JSValue thread_worker_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* native = get_thread_worker(ctx, thisVal);
    auto* runtime = current_runtime(ctx);
    if (!native || !runtime) {
        return JS_EXCEPTION;
    }
    if (!native->closed && native->thread) {
        if (!native->thread->finished()) {
            native->thread->interrupt();
        }
        native->thread->join();
        runtime->threadTree().remove(native->path);
        native->closed = true;
    }
    return JS_UNDEFINED;
}

JSValue new_thread_request_object(JSContext* ctx, ThreadRequest request) {
    auto* native = new JsThreadRequest{std::move(request)};
    JSValue obj = JS_NewObjectClass(ctx, thread_request_class_id);
    if (JS_IsException(obj)) {
        delete native;
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

JSValue thread_request_reply(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_thread_request(ctx, thisVal);
    if (!native || !native->request) {
        return JS_ThrowTypeError(ctx, "ThreadRequest is no longer active");
    }
    std::string payload = argc > 0 ? js_payload(ctx, argv[0]) : "";
    std::string type = "reply";
    if (argc > 1) {
        type = js_string(ctx, argv[1]);
    }
    const bool ok = native->request->reply(std::move(payload), std::move(type));
    native->request.reset();
    return JS_NewBool(ctx, ok);
}

JSValue thread_request_reject(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* native = get_thread_request(ctx, thisVal);
    if (!native || !native->request) {
        return JS_ThrowTypeError(ctx, "ThreadRequest is no longer active");
    }
    std::string error = argc > 0 ? js_string(ctx, argv[0]) : "rejected";
    const bool ok = native->request->reject(std::move(error));
    native->request.reset();
    return JS_NewBool(ctx, ok);
}

JSValue thread_message_object(JSContext* ctx, ThreadRequest request) {
    JSValue obj = new_thread_request_object(ctx, std::move(request));
    auto* native = static_cast<JsThreadRequest*>(JS_GetOpaque(obj, thread_request_class_id));
    const auto& message = native->request->message();
    JS_SetPropertyStr(ctx, obj, "id", JS_NewUint32(ctx, static_cast<uint32_t>(message.id)));
    JS_SetPropertyStr(ctx, obj, "source", JS_NewString(ctx, message.source.c_str()));
    JS_SetPropertyStr(ctx, obj, "destination", JS_NewString(ctx, message.destination.c_str()));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, message.type.c_str()));
    JS_SetPropertyStr(ctx, obj, "payload", JS_NewStringLen(ctx, message.payload.data(), message.payload.size()));
    JS_SetPropertyStr(ctx, obj, "expectsReply", JS_NewBool(ctx, message.expectsReply));
    return obj;
}

JSValue thread_get_path(JSContext* ctx, JSValueConst thisVal) {
    (void)thisVal;
    return JS_NewString(ctx, currentThreadPath().c_str());
}

JSValue thread_get_parent(JSContext* ctx, JSValueConst thisVal) {
    (void)thisVal;
    auto parent = thread_parent_path(currentThreadPath());
    return parent.empty() ? JS_NULL : JS_NewString(ctx, parent.c_str());
}

JSValue thread_children(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    (void)argc;
    (void)argv;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    const std::string prefix = currentThreadPath() + "/";
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& path : runtime->threadTree().paths()) {
        if (path.rfind(prefix, 0) == 0 && path.find('/', prefix.size()) == std::string::npos) {
            JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, path.c_str()));
        }
    }
    return array;
}

JSValue thread_spawn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "spawn(script, options) requires a script specifier");
    }
    ScriptThreadOptions options;
    options.script = js_string(ctx, argv[0]);
    options.path = child_path_from_name(currentThreadPath(), "worker");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue name = JS_GetPropertyStr(ctx, argv[1], "name");
        if (!JS_IsUndefined(name)) {
            options.path = child_path_from_name(currentThreadPath(), js_string(ctx, name));
        }
        JS_FreeValue(ctx, name);
        JSValue path = JS_GetPropertyStr(ctx, argv[1], "path");
        if (!JS_IsUndefined(path)) {
            options.path = js_string(ctx, path);
        }
        JS_FreeValue(ctx, path);
        JSValue source = JS_GetPropertyStr(ctx, argv[1], "source");
        if (!JS_IsUndefined(source)) {
            options.source = js_string(ctx, source);
        }
        JS_FreeValue(ctx, source);
    }
    auto spawned = runtime->threadTree().spawn(*runtime, std::move(options));
    if (!spawned) {
        return throw_thread_error(ctx, spawned.error());
    }
    return new_thread_worker_object(ctx, spawned.value());
}

JSValue thread_post(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "post(destination, message) requires destination and message");
    }
    auto destination = js_string(ctx, argv[0]);
    auto message = js_message(ctx, argv[1], currentThreadPath(), std::move(destination));
    return JS_NewBool(ctx, runtime->threadTree().send(std::move(message)));
}

JSValue thread_request(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "request(destination, message, options) requires destination and message");
    }
    auto destination = js_string(ctx, argv[0]);
    auto message = js_message(ctx, argv[1], currentThreadPath(), std::move(destination));
    auto pending = runtime->threadTree().request(std::move(message),
        std::chrono::milliseconds(option_timeout_ms(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, 30000)));
    return thread_reply_object(ctx, pending.wait());
}

JSValue thread_request_pending(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "requestPending(destination, message, options) requires destination and message");
    }
    auto destination = js_string(ctx, argv[0]);
    auto message = js_message(ctx, argv[1], currentThreadPath(), std::move(destination));
    auto pending = runtime->threadTree().request(std::move(message),
        std::chrono::milliseconds(option_timeout_ms(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, 30000)));
    return new_pending_reply_object(ctx, std::move(pending));
}

JSValue thread_recv(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    const auto timeout = std::chrono::milliseconds(option_timeout_ms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0));
    auto request = runtime->threadTree().take(currentThreadPath(), timeout);
    if (!request) {
        return JS_NULL;
    }
    return thread_message_object(ctx, std::move(*request));
}

JSValue thread_requests(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    (void)argc;
    (void)argv;
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    while (true) {
        JSValue args[] = {JS_NewObject(ctx)};
        JS_SetPropertyStr(ctx, args[0], "timeoutMs", JS_NewInt32(ctx, 0));
        JSValue item = thread_recv(ctx, JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, args[0]);
        if (JS_IsNull(item)) {
            JS_FreeValue(ctx, item);
            break;
        }
        JS_SetPropertyUint32(ctx, array, index++, item);
    }
    return array;
}

JSValue thread_shutdown(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    runtime->threadTree().shutdown(
        std::chrono::milliseconds(option_timeout_ms(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 2000)));
    return JS_UNDEFINED;
}

void add_console(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, console_log, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

void add_env_string(JSContext* ctx, const char* name) {
    const char* value = std::getenv(name);
    if (!value) {
        return;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name, JS_NewString(ctx, value));
    JS_FreeValue(ctx, global);
}

void add_test_environment(JSContext* ctx) {
    add_env_string(ctx, "WL2_CURL_TEST_URL");
    add_env_string(ctx, "WL2_CURL_TEST_POST_URL");
    add_env_string(ctx, "WL2_ASIO_TEST_HOST");
    add_env_string(ctx, "WL2_ASIO_TEST_PORT");
}

JSValue runtime_modules(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& info : runtime->modules().modules()) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, info.name.c_str()));
        JS_SetPropertyStr(ctx, entry, "version", JS_NewString(ctx, info.version.c_str()));
        JS_SetPropertyStr(ctx, entry, "build", JS_NewString(ctx, info.build.c_str()));
        JS_SetPropertyStr(ctx, entry, "stableId", JS_NewString(ctx, info.stableId.c_str()));
        JS_SetPropertyStr(ctx, entry, "summary", JS_NewString(ctx, info.summary.c_str()));
        JS_SetPropertyUint32(ctx, array, index++, entry);
    }
    return array;
}

JSValue runtime_env(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "Runtime is unavailable");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "wl2.runtime.env(name) requires a variable name");
    }

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    if (!runtime->environmentAccessAllowed(name)) {
        JSValue error = JS_ThrowTypeError(ctx,
            "wl2.runtime.env: environment access for '%s' is not permitted", name);
        JS_FreeCString(ctx, name);
        return error;
    }

    const char* value = std::getenv(name);
    JSValue result = value ? JS_NewString(ctx, value) : JS_NULL;
    JS_FreeCString(ctx, name);
    return result;
}

JSValue runtime_now(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    (void)ctx;
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return JS_NewFloat64(ctx, static_cast<double>(micros) / 1000.0);
}

void add_runtime_api(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        wl2 = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "wl2", JS_DupValue(ctx, wl2));
    }

    JSValue runtimeObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, runtimeObj, "version", JS_NewString(ctx, WL2_VERSION));
    JS_SetPropertyStr(ctx, runtimeObj, "build", JS_NewString(ctx, WL2_BUILD));
    JS_SetPropertyStr(ctx, runtimeObj, "engine", JS_NewString(ctx, configuredJsEngineName()));

    JSValue argvArray = JS_NewArray(ctx);
    if (Runtime* runtime = current_runtime(ctx)) {
        uint32_t index = 0;
        for (const auto& arg : runtime->scriptArgs()) {
            JS_SetPropertyUint32(ctx, argvArray, index++, JS_NewString(ctx, arg.c_str()));
        }
    }
    JS_SetPropertyStr(ctx, runtimeObj, "argv", argvArray);

    JS_SetPropertyStr(ctx, runtimeObj, "modules", JS_NewCFunction(ctx, runtime_modules, "modules", 0));
    JS_SetPropertyStr(ctx, runtimeObj, "env", JS_NewCFunction(ctx, runtime_env, "env", 1));
    JS_SetPropertyStr(ctx, runtimeObj, "now", JS_NewCFunction(ctx, runtime_now, "now", 0));

    JS_SetPropertyStr(ctx, wl2, "runtime", runtimeObj);
    JS_FreeValue(ctx, wl2);
    JS_FreeValue(ctx, global);
}

void add_buffer_api(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (buffer_class_id == 0) {
        JS_NewClassID(&buffer_class_id);
    }

    JSClassDef classDef{};
    classDef.class_name = "Buffer";
    classDef.finalizer = buffer_finalizer;
    JS_NewClass(rt, buffer_class_id, &classDef);

    JSValue proto = JS_NewObject(ctx);
    JSAtom byteLengthAtom = JS_NewAtom(ctx, "byteLength");
    JS_DefinePropertyGetSet(ctx, proto, byteLengthAtom,
        new_getter(ctx, "get byteLength", buffer_get_size),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, byteLengthAtom);

    JSAtom sizeAtom = JS_NewAtom(ctx, "size");
    JS_DefinePropertyGetSet(ctx, proto, sizeAtom,
        new_getter(ctx, "get size", buffer_get_size),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, sizeAtom);

    JS_SetPropertyStr(ctx, proto, "text", JS_NewCFunction(ctx, buffer_text, "text", 0));
    JS_SetPropertyStr(ctx, proto, "slice", JS_NewCFunction(ctx, buffer_slice, "slice", 2));
    JS_SetPropertyStr(ctx, proto, "arrayBuffer", JS_NewCFunction(ctx, buffer_array_buffer, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, proto, "uint8Array", JS_NewCFunction(ctx, buffer_uint8_array, "uint8Array", 0));
    JS_SetPropertyStr(ctx, proto, "toString", JS_NewCFunction(ctx, buffer_to_string, "toString", 0));
    JS_SetClassProto(ctx, buffer_class_id, proto);

    JSValue ctor = JS_NewCFunction2(ctx, buffer_ctor, "Buffer", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        wl2 = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "wl2", JS_DupValue(ctx, wl2));
    }

    JSValue bufferNamespace = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, bufferNamespace, "fromText", JS_NewCFunction(ctx, buffer_from_text, "fromText", 1));
    JS_SetPropertyStr(ctx, bufferNamespace, "fromArrayBuffer", JS_NewCFunction(ctx, buffer_from_array_buffer, "fromArrayBuffer", 1));
    JS_SetPropertyStr(ctx, bufferNamespace, "isBuffer", JS_NewCFunction(ctx, buffer_is_buffer, "isBuffer", 1));
    JS_SetPropertyStr(ctx, wl2, "Buffer", ctor);
    JS_SetPropertyStr(ctx, wl2, "buffer", bufferNamespace);

    JS_FreeValue(ctx, wl2);
    JS_FreeValue(ctx, global);
}

void add_resources_api(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (resource_handle_class_id == 0) {
        JS_NewClassID(&resource_handle_class_id);
    }

    JSClassDef classDef{};
    classDef.class_name = "ResourceHandle";
    classDef.finalizer = resource_handle_finalizer;
    JS_NewClass(rt, resource_handle_class_id, &classDef);

    JSValue proto = JS_NewObject(ctx);
    JSAtom pathAtom = JS_NewAtom(ctx, "path");
    JS_DefinePropertyGetSet(ctx, proto, pathAtom,
        new_getter(ctx, "get path", resource_handle_get_path),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, pathAtom);

    JSAtom sizeAtom = JS_NewAtom(ctx, "size");
    JS_DefinePropertyGetSet(ctx, proto, sizeAtom,
        new_getter(ctx, "get size", resource_handle_get_size),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, sizeAtom);

    JSAtom compressedAtom = JS_NewAtom(ctx, "compressed");
    JS_DefinePropertyGetSet(ctx, proto, compressedAtom,
        new_getter(ctx, "get compressed", resource_handle_get_compressed),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, compressedAtom);

    JSAtom closedAtom = JS_NewAtom(ctx, "closed");
    JS_DefinePropertyGetSet(ctx, proto, closedAtom,
        new_getter(ctx, "get closed", resource_handle_get_closed),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, closedAtom);

    JS_SetPropertyStr(ctx, proto, "stat", JS_NewCFunction(ctx, resource_handle_stat, "stat", 0));
    JS_SetPropertyStr(ctx, proto, "text", JS_NewCFunction(ctx, resource_handle_text, "text", 0));
    JS_SetPropertyStr(ctx, proto, "bytes", JS_NewCFunction(ctx, resource_handle_bytes, "bytes", 0));
    JS_SetPropertyStr(ctx, proto, "uint8Array", JS_NewCFunction(ctx, resource_handle_uint8_array, "uint8Array", 0));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, resource_handle_close, "close", 0));
    JS_SetClassProto(ctx, resource_handle_class_id, proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        wl2 = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "wl2", JS_DupValue(ctx, wl2));
    }

    JSValue resources = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resources, "exists", JS_NewCFunction(ctx, resources_exists, "exists", 1));
    JS_SetPropertyStr(ctx, resources, "stat", JS_NewCFunction(ctx, resources_stat, "stat", 1));
    JS_SetPropertyStr(ctx, resources, "list", JS_NewCFunction(ctx, resources_list, "list", 1));
    JS_SetPropertyStr(ctx, resources, "walk", JS_NewCFunction(ctx, resources_walk, "walk", 1));
    JS_SetPropertyStr(ctx, resources, "open", JS_NewCFunction(ctx, resources_open, "open", 1));
    JS_SetPropertyStr(ctx, resources, "readText", JS_NewCFunction(ctx, resources_read_text, "readText", 1));
    JS_SetPropertyStr(ctx, resources, "readBytes", JS_NewCFunction(ctx, resources_read_bytes, "readBytes", 1));
    JS_SetPropertyStr(ctx, wl2, "resources", resources);

    JS_FreeValue(ctx, wl2);
    JS_FreeValue(ctx, global);
}

void add_thread_api(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (thread_worker_class_id == 0) {
        JS_NewClassID(&thread_worker_class_id);
    }
    if (thread_request_class_id == 0) {
        JS_NewClassID(&thread_request_class_id);
    }
    if (thread_pending_class_id == 0) {
        JS_NewClassID(&thread_pending_class_id);
    }

    JSClassDef workerClass{};
    workerClass.class_name = "ThreadWorker";
    workerClass.finalizer = thread_worker_finalizer;
    JS_NewClass(rt, thread_worker_class_id, &workerClass);

    JSValue workerProto = JS_NewObject(ctx);
    JSAtom workerPathAtom = JS_NewAtom(ctx, "path");
    JS_DefinePropertyGetSet(ctx, workerProto, workerPathAtom,
        new_getter(ctx, "get path", thread_worker_get_path),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, workerPathAtom);
    JS_SetPropertyStr(ctx, workerProto, "post", JS_NewCFunction(ctx, thread_worker_post, "post", 1));
    JS_SetPropertyStr(ctx, workerProto, "request", JS_NewCFunction(ctx, thread_worker_request, "request", 2));
    JS_SetPropertyStr(ctx, workerProto, "requestPending", JS_NewCFunction(ctx, thread_worker_request_pending, "requestPending", 2));
    JS_SetPropertyStr(ctx, workerProto, "close", JS_NewCFunction(ctx, thread_worker_close, "close", 0));
    JS_SetPropertyStr(ctx, workerProto, "wait", JS_NewCFunction(ctx, thread_worker_wait, "wait", 1));
    JS_SetClassProto(ctx, thread_worker_class_id, workerProto);

    JSClassDef requestClass{};
    requestClass.class_name = "ThreadRequest";
    requestClass.finalizer = thread_request_finalizer;
    JS_NewClass(rt, thread_request_class_id, &requestClass);

    JSValue requestProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, requestProto, "reply", JS_NewCFunction(ctx, thread_request_reply, "reply", 2));
    JS_SetPropertyStr(ctx, requestProto, "reject", JS_NewCFunction(ctx, thread_request_reject, "reject", 1));
    JS_SetClassProto(ctx, thread_request_class_id, requestProto);

    JSClassDef pendingClass{};
    pendingClass.class_name = "PendingReply";
    pendingClass.finalizer = thread_pending_finalizer;
    JS_NewClass(rt, thread_pending_class_id, &pendingClass);

    JSValue pendingProto = JS_NewObject(ctx);
    JSAtom pendingIdAtom = JS_NewAtom(ctx, "id");
    JS_DefinePropertyGetSet(ctx, pendingProto, pendingIdAtom,
        new_getter(ctx, "get id", thread_pending_get_id),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, pendingIdAtom);

    JSAtom pendingDoneAtom = JS_NewAtom(ctx, "done");
    JS_DefinePropertyGetSet(ctx, pendingProto, pendingDoneAtom,
        new_getter(ctx, "get done", thread_pending_get_done),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, pendingDoneAtom);

    JS_SetPropertyStr(ctx, pendingProto, "poll", JS_NewCFunction(ctx, thread_pending_poll, "poll", 0));
    JS_SetPropertyStr(ctx, pendingProto, "wait", JS_NewCFunction(ctx, thread_pending_wait, "wait", 1));
    JS_SetPropertyStr(ctx, pendingProto, "cancel", JS_NewCFunction(ctx, thread_pending_cancel, "cancel", 0));
    JS_SetPropertyStr(ctx, pendingProto, "then", JS_NewCFunction(ctx, thread_pending_then, "then", 2));
    JS_SetClassProto(ctx, thread_pending_class_id, pendingProto);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        wl2 = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "wl2", JS_DupValue(ctx, wl2));
    }

    JSValue thread = JS_NewObject(ctx);
    JSAtom pathAtom = JS_NewAtom(ctx, "path");
    JS_DefinePropertyGetSet(ctx, thread, pathAtom,
        new_getter(ctx, "get path", thread_get_path),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, pathAtom);

    JSAtom parentAtom = JS_NewAtom(ctx, "parent");
    JS_DefinePropertyGetSet(ctx, thread, parentAtom,
        new_getter(ctx, "get parent", thread_get_parent),
        JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, parentAtom);

    JS_SetPropertyStr(ctx, thread, "children", JS_NewCFunction(ctx, thread_children, "children", 0));
    JS_SetPropertyStr(ctx, thread, "spawn", JS_NewCFunction(ctx, thread_spawn, "spawn", 2));
    JS_SetPropertyStr(ctx, thread, "post", JS_NewCFunction(ctx, thread_post, "post", 2));
    JS_SetPropertyStr(ctx, thread, "request", JS_NewCFunction(ctx, thread_request, "request", 3));
    JS_SetPropertyStr(ctx, thread, "requestPending", JS_NewCFunction(ctx, thread_request_pending, "requestPending", 3));
    JS_SetPropertyStr(ctx, thread, "recv", JS_NewCFunction(ctx, thread_recv, "recv", 1));
    JS_SetPropertyStr(ctx, thread, "requests", JS_NewCFunction(ctx, thread_requests, "requests", 0));
    JS_SetPropertyStr(ctx, thread, "shutdown", JS_NewCFunction(ctx, thread_shutdown, "shutdown", 1));
    JS_SetPropertyStr(ctx, wl2, "thread", thread);

    JS_FreeValue(ctx, wl2);
    JS_FreeValue(ctx, global);
}

JSModuleDef* load_native_module(JSContext* ctx, const char* moduleName, void* opaque) {
    auto* runtime = static_cast<Runtime*>(opaque);
    if (!runtime) {
        return nullptr;
    }
    auto factory = runtime->findQuickJsModule(moduleName);
    if (!factory) {
        return nullptr;
    }
    return static_cast<JSModuleDef*>(factory(ctx, moduleName));
}

void drain_jobs(JSRuntime* rt) {
    JSContext* jobContext = nullptr;
    while (JS_ExecutePendingJob(rt, &jobContext) > 0) {
    }
}

bool is_thenable(JSContext* ctx, JSValueConst value) {
    if (!JS_IsObject(value)) {
        return false;
    }
    JSValue then = JS_GetPropertyStr(ctx, value, "then");
    const bool result = JS_IsFunction(ctx, then);
    JS_FreeValue(ctx, then);
    return result;
}

std::string value_to_string(JSContext* ctx, JSValueConst value, const char* fallback) {
    const char* text = JS_ToCString(ctx, value);
    std::string out = text ? text : fallback;
    if (text) {
        JS_FreeCString(ctx, text);
    }
    return out;
}

std::optional<std::string> property_string(JSContext* ctx, JSValueConst value, const char* name) {
    JSValue property = JS_GetPropertyStr(ctx, value, name);
    if (JS_IsException(property) || JS_IsUndefined(property) || JS_IsNull(property)) {
        JS_FreeValue(ctx, property);
        return std::nullopt;
    }
    const char* text = JS_ToCString(ctx, property);
    if (!text) {
        JS_FreeValue(ctx, property);
        return std::nullopt;
    }
    std::string out(text);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, property);
    return out;
}

struct JsFailure {
    std::string message;
    std::string details;
};

JsFailure describe_failure(JSContext* ctx, JSValueConst value, const char* fallback) {
    auto name = property_string(ctx, value, "name");
    auto message = property_string(ctx, value, "message");
    auto stack = property_string(ctx, value, "stack");

    JsFailure failure;
    if (name && message && !name->empty() && !message->empty()) {
        failure.message = *name + ": " + *message;
    } else if (message && !message->empty()) {
        failure.message = *message;
    } else {
        failure.message = value_to_string(ctx, value, fallback);
    }

    if (stack && !stack->empty() && *stack != failure.message) {
        failure.details = "JavaScript stack:\n" + *stack;
    }
    return failure;
}

// Called periodically by QuickJS while a script runs. Returning non-zero aborts
// execution with an uncatchable exception, which is how forced thread shutdown
// stops a runaway script without unsafe OS-level thread termination.
int cancel_interrupt_handler(JSRuntime*, void* opaque) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(opaque);
    return cancel && cancel->load(std::memory_order_relaxed) ? 1 : 0;
}
#endif

class QuickJsEngine final : public JsEngine {
public:
    Result<int> runModule(Runtime& runtime, std::string_view specifier, std::string_view source,
        const std::atomic<bool>* cancel) override {
#if WL2_HAVE_QUICKJS
        JSRuntime* rt = JS_NewRuntime();
        if (!rt) {
            return Error("quickjs_runtime_create_failed", "Unable to create QuickJS runtime");
        }

        if (cancel) {
            JS_SetInterruptHandler(rt, cancel_interrupt_handler,
                const_cast<void*>(static_cast<const void*>(cancel)));
        }

        JS_SetModuleLoaderFunc(rt, nullptr, load_native_module, &runtime);

        JSContext* ctx = JS_NewContext(rt);
        if (!ctx) {
            JS_FreeRuntime(rt);
            return Error("quickjs_context_create_failed", "Unable to create QuickJS context");
        }
        JS_SetContextOpaque(ctx, &runtime);

        add_console(ctx);
        add_runtime_api(ctx);
        add_buffer_api(ctx);
        add_resources_api(ctx);
        add_thread_api(ctx);
        add_test_environment(ctx);

        JSValue result = JS_Eval(ctx,
            source.data(),
            source.size(),
            std::string(specifier).c_str(),
            JS_EVAL_TYPE_MODULE);

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(ctx);
            auto failure = describe_failure(ctx, exception, "QuickJS exception");
            JS_FreeValue(ctx, exception);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return Error("quickjs_interrupted", "Script execution was interrupted: " + failure.message, failure.details);
            }
            return Error("quickjs_exception", failure.message, failure.details);
        }

        drain_jobs(rt);
        // Event loop: keep running while the top-level module promise is pending
        // or native async work is outstanding. Each turn drains native
        // completions (which resolve/reject promises and may enqueue jobs) and
        // engine jobs; when nothing is runnable but native work is outstanding,
        // wait for a completion instead of busy-looping.
        {
            AsyncHost& async = runtime.async();
            auto topPending = [&]() {
                return is_thenable(ctx, result) && JS_PromiseState(ctx, result) == JS_PROMISE_PENDING;
            };
            while (topPending() || async.hasPendingWork()) {
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    break;
                }
                const std::size_t completionsRun = async.drain();
                bool jobRan = false;
                JSContext* jobContext = nullptr;
                while (JS_ExecutePendingJob(rt, &jobContext) > 0) {
                    jobRan = true;
                }
                if (completionsRun == 0 && !jobRan) {
                    if (async.hasPendingWork()) {
                        async.waitForWork(std::chrono::milliseconds(25));
                    } else {
                        break;
                    }
                }
            }
        }
        if (is_thenable(ctx, result)) {
            if (JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
                JSValue rejection = JS_PromiseResult(ctx, result);
                auto failure = describe_failure(ctx, rejection, "QuickJS promise rejection");
                JS_FreeValue(ctx, rejection);
                JS_FreeValue(ctx, result);
                JS_FreeContext(ctx);
                JS_FreeRuntime(rt);
                return Error("quickjs_promise_rejection", failure.message, failure.details);
            }
        }

        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
#else
        (void)runtime;
        (void)specifier;
        (void)source;
        return Error("quickjs_not_available",
            "Winglib2 was configured with WL2_JS_ENGINE=quickjs, but QuickJS development files were not found. "
            "Install QuickJS and set WL2_QUICKJS_ROOT, or configure with -DWL2_JS_ENGINE=v8.");
#endif
    }
};

} // namespace

std::unique_ptr<JsEngine> createConfiguredJsEngine() {
    return std::make_unique<QuickJsEngine>();
}

const char* configuredJsEngineName() {
    return "quickjs";
}

} // namespace wl2
