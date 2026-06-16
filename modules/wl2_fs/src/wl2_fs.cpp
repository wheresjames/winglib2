#include "wl2_fs/wl2_fs.h"

#include "wl2/runtime.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
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

namespace fs = std::filesystem;

constexpr const char* FsApi = R"(Exports JavaScript module wl2:fs.

Functions:
  readText(path)   -> string
  readBytes(path)  -> wl2.Buffer
  exists(path)     -> boolean
  stat(path)       -> { path, exists, isFile, isDirectory, isSymlink, size }
  list(path)       -> [ { name, isFile, isDirectory, isSymlink, size } ]
  walk(path)       -> [ { path, isFile, isDirectory, isSymlink, size } ]

Security model:
  Reads are disabled unless the host enables RuntimeOptions.allowFilesystemReads.
  Even when enabled, access is confined to RuntimeOptions.filesystemReadRoots.
  Paths are host filesystem paths, not wl2: resource specifiers.
  This module is read-only; it provides no write or delete operations.)";

#if WL2_HAVE_QUICKJS

wl2::Runtime* current_runtime(JSContext* ctx) {
    return static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
}

JSValue throw_fs_error(
    JSContext* ctx,
    const char* code,
    const char* operation,
    const std::string& path,
    const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "FsError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_fs"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "path", JS_NewString(ctx, path.c_str()));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}

// Read argv[0] as a path string. Returns false and leaves a pending exception
// when the argument is missing or not a string.
bool read_path_argument(JSContext* ctx, const char* operation, int argc, JSValueConst* argv, std::string& out) {
    if (argc < 1) {
        throw_fs_error(ctx, "fs_invalid_argument", operation, "",
            std::string(operation) + "(path) requires a path string");
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

// Resolve and authorize a requested path. On denial, leaves a pending exception
// and returns nullopt.
std::optional<fs::path> authorize(JSContext* ctx, const char* operation, const std::string& requested) {
    wl2::Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        JS_ThrowInternalError(ctx, "Runtime is unavailable");
        return std::nullopt;
    }
    auto resolved = runtime->resolveFilesystemReadPath(requested);
    if (!resolved) {
        throw_fs_error(ctx, "fs_permission_denied", operation, requested,
            "Filesystem read is not permitted for this path");
        return std::nullopt;
    }
    return resolved;
}

JSValue make_wl2_buffer(JSContext* ctx, const std::string& bytes) {
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
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2 host API is unavailable");
    }

    JSValue bufferNamespace = JS_GetPropertyStr(ctx, wl2, "buffer");
    JS_FreeValue(ctx, wl2);
    if (!JS_IsObject(bufferNamespace)) {
        JS_FreeValue(ctx, bufferNamespace);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2.buffer host API is unavailable");
    }

    JSValue fromArrayBuffer = JS_GetPropertyStr(ctx, bufferNamespace, "fromArrayBuffer");
    if (!JS_IsFunction(ctx, fromArrayBuffer)) {
        JS_FreeValue(ctx, fromArrayBuffer);
        JS_FreeValue(ctx, bufferNamespace);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2.buffer.fromArrayBuffer is unavailable");
    }

    JSValue args[] = {arrayBuffer};
    JSValue result = JS_Call(ctx, fromArrayBuffer, bufferNamespace, 1, args);
    JS_FreeValue(ctx, fromArrayBuffer);
    JS_FreeValue(ctx, bufferNamespace);
    JS_FreeValue(ctx, arrayBuffer);
    return result;
}

// Read the entire contents of a permitted regular file.
bool read_file(JSContext* ctx, const char* operation, const fs::path& resolved, const std::string& requested, std::string& out) {
    std::error_code ec;
    auto status = fs::status(resolved, ec);
    if (ec || !fs::exists(status)) {
        throw_fs_error(ctx, "fs_not_found", operation, requested, "No such file");
        return false;
    }
    if (!fs::is_regular_file(status)) {
        throw_fs_error(ctx, "fs_not_a_file", operation, requested, "Path is not a regular file");
        return false;
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        throw_fs_error(ctx, "fs_read_failed", operation, requested, "Unable to open file");
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw_fs_error(ctx, "fs_read_failed", operation, requested, "Error while reading file");
        return false;
    }
    return true;
}

void set_entry_metadata(JSContext* ctx, JSValue obj, const fs::path& path, const fs::file_status& symlinkStatus) {
    const bool isSymlink = fs::is_symlink(symlinkStatus);
    // Follow the link to classify the final target for isFile/isDirectory.
    std::error_code ec;
    fs::file_status targetStatus = isSymlink ? fs::status(path, ec) : symlinkStatus;
    const bool isFile = fs::is_regular_file(targetStatus);
    const bool isDirectory = fs::is_directory(targetStatus);

    JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, isFile));
    JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, isDirectory));
    JS_SetPropertyStr(ctx, obj, "isSymlink", JS_NewBool(ctx, isSymlink));

    uint64_t size = 0;
    if (isFile) {
        std::error_code sizeEc;
        auto fileSize = fs::file_size(path, sizeEc);
        if (!sizeEc) {
            size = static_cast<uint64_t>(fileSize);
        }
    }
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(size)));
}

JSValue fs_read_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string requested;
    if (!read_path_argument(ctx, "readText", argc, argv, requested)) {
        return JS_EXCEPTION;
    }
    auto resolved = authorize(ctx, "readText", requested);
    if (!resolved) {
        return JS_EXCEPTION;
    }
    std::string contents;
    if (!read_file(ctx, "readText", *resolved, requested, contents)) {
        return JS_EXCEPTION;
    }
    return JS_NewStringLen(ctx, contents.data(), contents.size());
}

JSValue fs_read_bytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string requested;
    if (!read_path_argument(ctx, "readBytes", argc, argv, requested)) {
        return JS_EXCEPTION;
    }
    auto resolved = authorize(ctx, "readBytes", requested);
    if (!resolved) {
        return JS_EXCEPTION;
    }
    std::string contents;
    if (!read_file(ctx, "readBytes", *resolved, requested, contents)) {
        return JS_EXCEPTION;
    }
    return make_wl2_buffer(ctx, contents);
}

JSValue fs_exists(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string requested;
    if (!read_path_argument(ctx, "exists", argc, argv, requested)) {
        return JS_EXCEPTION;
    }
    auto resolved = authorize(ctx, "exists", requested);
    if (!resolved) {
        return JS_EXCEPTION;
    }
    std::error_code ec;
    bool present = fs::exists(*resolved, ec);
    return JS_NewBool(ctx, present && !ec);
}

JSValue fs_stat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string requested;
    if (!read_path_argument(ctx, "stat", argc, argv, requested)) {
        return JS_EXCEPTION;
    }
    auto resolved = authorize(ctx, "stat", requested);
    if (!resolved) {
        return JS_EXCEPTION;
    }

    std::error_code ec;
    fs::file_status symlinkStatus = fs::symlink_status(*resolved, ec);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, resolved->string().c_str()));

    const bool present = !ec && fs::exists(symlinkStatus);
    JS_SetPropertyStr(ctx, obj, "exists", JS_NewBool(ctx, present));
    if (!present) {
        JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, obj, "isSymlink", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, 0));
        return obj;
    }
    set_entry_metadata(ctx, obj, *resolved, symlinkStatus);
    return obj;
}

JSValue fs_list(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string requested;
    if (!read_path_argument(ctx, "list", argc, argv, requested)) {
        return JS_EXCEPTION;
    }
    auto resolved = authorize(ctx, "list", requested);
    if (!resolved) {
        return JS_EXCEPTION;
    }

    std::error_code ec;
    auto status = fs::status(*resolved, ec);
    if (ec || !fs::exists(status)) {
        return throw_fs_error(ctx, "fs_not_found", "list", requested, "No such directory");
    }
    if (!fs::is_directory(status)) {
        return throw_fs_error(ctx, "fs_not_a_directory", "list", requested, "Path is not a directory");
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (fs::directory_iterator it(*resolved, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, entry.path().filename().string().c_str()));
        std::error_code statusEc;
        set_entry_metadata(ctx, obj, entry.path(), fs::symlink_status(entry.path(), statusEc));
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    return array;
}

JSValue fs_walk(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string requested;
    if (!read_path_argument(ctx, "walk", argc, argv, requested)) {
        return JS_EXCEPTION;
    }
    auto resolved = authorize(ctx, "walk", requested);
    if (!resolved) {
        return JS_EXCEPTION;
    }

    std::error_code ec;
    auto status = fs::status(*resolved, ec);
    if (ec || !fs::exists(status)) {
        return throw_fs_error(ctx, "fs_not_found", "walk", requested, "No such directory");
    }
    if (!fs::is_directory(status)) {
        return throw_fs_error(ctx, "fs_not_a_directory", "walk", requested, "Path is not a directory");
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    // Symlinked directories are not followed, which keeps traversal inside the
    // authorized root.
    fs::recursive_directory_iterator it(*resolved, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code relEc;
        fs::path relative = fs::relative(entry.path(), *resolved, relEc);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "path",
            JS_NewString(ctx, (relEc ? entry.path() : relative).generic_string().c_str()));
        std::error_code statusEc;
        set_entry_metadata(ctx, obj, entry.path(), fs::symlink_status(entry.path(), statusEc));
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    return array;
}

int init_fs_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "readText", JS_NewCFunction(ctx, fs_read_text, "readText", 1));
    JS_SetModuleExport(ctx, module, "readBytes", JS_NewCFunction(ctx, fs_read_bytes, "readBytes", 1));
    JS_SetModuleExport(ctx, module, "exists", JS_NewCFunction(ctx, fs_exists, "exists", 1));
    JS_SetModuleExport(ctx, module, "stat", JS_NewCFunction(ctx, fs_stat, "stat", 1));
    JS_SetModuleExport(ctx, module, "list", JS_NewCFunction(ctx, fs_list, "list", 1));
    JS_SetModuleExport(ctx, module, "walk", JS_NewCFunction(ctx, fs_walk, "walk", 1));
    return 0;
}
#endif

} // namespace

wl2::ModuleInfo wl2_fs_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:fs", wl2_fs_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:fs",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "b3a1f2d4-6c8e-4a17-9b2f-0d5e7c1a4e90",
        .summary = "Read-only host filesystem module gated by runtime policy.",
        .api = FsApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_fs_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_fs_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "readText");
    JS_AddModuleExport(ctx, module, "readBytes");
    JS_AddModuleExport(ctx, module, "exists");
    JS_AddModuleExport(ctx, module, "stat");
    JS_AddModuleExport(ctx, module, "list");
    JS_AddModuleExport(ctx, module, "walk");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_FS_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:fs";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "b3a1f2d4-6c8e-4a17-9b2f-0d5e7c1a4e90";
    out->summary = "Read-only host filesystem module gated by runtime policy.";
    out->api = FsApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
