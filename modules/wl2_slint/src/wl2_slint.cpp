// wl2:slint — declarative UI module wrapping the Slint interpreter.
//
// Exposes compile()/compileFile() -> Component, Component.create() -> Instance,
// Instance.get()/set() (number/string/bool/struct/array-model, brush/color hex),
// Instance.on()/invoke() for callbacks, Instance.colorScheme(),
// Instance.show()/hide(), native file/folder dialogs, and
// Component.run()/quit() driving the Slint event loop with a timer that pumps
// the host async queue on the UI thread. Opening a window and running the loop
// are gated by Runtime::authorizeUi() (denied by default). All interpreter calls
// run on the JS/main thread; this module spawns no worker threads.
#include "wl2_slint/wl2_slint.h"

#include "wl2/membus.h"
#include "wl2/runtime.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#include <slint-interpreter.h>
#include <slint_timer.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>

#include "slint_offscreen.h"
#include "slint_runtime.h"
#include "value_bridge.h"

#if WL2_SLINT_HAVE_NATIVE_DIALOGS
#include <nfd.h>
#endif
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif

namespace {

constexpr const char* SlintApi = R"(Exports JavaScript module wl2:slint.

Functions:
  compile(source, options) -> Promise<Component>       compile .slint markup
  compileFile(specifier, options) -> Promise<Component> compile from a wl2:/ or fs path
  openFileDialog(options) -> Promise<string|null>       show a native open-file dialog
  openFilesDialog(options) -> Promise<string[]|null>    show a native multi-open dialog
  saveFileDialog(options) -> Promise<string|null>       show a native save-file dialog
  pickFolderDialog(options) -> Promise<string|null>     show a native folder dialog
  useOffscreenRendering() -> true                       select the headless platform (before compile/create)

Component:
  create() -> Instance                                 instantiate the component
  run() -> Promise<void>                                run the event loop (requires the UI capability)
  quit()                                                stop the event loop

Instance:
  get(property) -> value                                read an in/in-out property
  set(property, value)                                  set an in/in-out property
  setImageFromFrameRing(property, name[, options]) -> metadata
                                                        copy an RGBA memvid frame into an image property
  on(callback, fn)                                      register a JS handler for a callback
  invoke(callback, ...args) -> value                    call a component callback
  colorScheme() -> "unknown"|"dark"|"light"             query the active widget style scheme
  show() / hide()                                       open/close the window (requires the UI capability)
  renderOffscreenTo(name, { size }) -> metadata         software-render to a FrameRing (requires shared memory)
  renderOffscreenFrame() -> metadata                    re-render the bound off-screen target
  injectPointer(x, y, phase[, button]) -> metadata      dispatch a synthetic pointer event + re-render
  injectKey(text[, phase]) -> metadata                  dispatch a synthetic key event + re-render

Value marshaling: number, string, bool, one-level objects (JS object <-> Slint
struct), arrays (JS array <-> Slint model), brush/color properties as CSS hex
strings ("#rrggbb"/"#rrggbbaa"), and explicit RGBA FrameRing-to-image copies via
setImageFromFrameRing().

Event loop: run() blocks until the last window closes or quit() is called; a
timer pumps host async work so other modules' promises keep settling.

Security defaults:
  Compiling, instantiating, get/set, on/invoke are always allowed (no window).
  Opening a window (show()/run()) is denied by default; the host runtime must
  grant the UI capability (allowUi), surfaced as SlintError
  "slint_permission_denied".)";

#if WL2_HAVE_QUICKJS

// --- Native handles ------------------------------------------------------------

// A compiled component definition. The interpreter ComponentDefinition is itself
// a cheap handle, but wrapping it lets instances keep it alive (shared_ptr).
struct CompiledComponent {
    slint::interpreter::ComponentDefinition definition;
};

// Per-instance callback registry. The JS handler functions are stored as
// properties of a "handlers holder" JS object that is itself a hidden property
// of the instance JS object, so they are owned by the JS object graph and
// reclaimed by normal GC with the instance (no module-held JS references that
// would defeat the cycle collector). `holder` is a borrowed, non-owning copy of
// that object's JSValue, valid for as long as the instance — and therefore this
// CallbackData — is alive. Held by the Slint closures only through a weak_ptr so
// they never keep the instance alive.
struct CallbackData {
    JSContext* ctx = nullptr;
    JSValue holder = JS_UNDEFINED;     // borrowed; owned by the instance object
    std::set<std::string> registered;  // callback names already wired to Slint
};

// Off-screen render target (UI-on-3D producer): a SoftwareRenderer-backed
// window adapter writing RGBA8 frames into a FrameRing. Null until the instance
// is bound with renderOffscreenTo().
struct OffscreenTarget {
    wl2_slint_offscreen::OffscreenWindowAdapter* adapter = nullptr;  // owned by Slint
    wl2::VideoBuffer ring;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<slint::Rgb8Pixel> scratch;
};

// A live component instance plus a keepalive on the compiled component and the
// JS callback registry.
struct InstanceState {
    std::shared_ptr<CompiledComponent> component;
    slint::ComponentHandle<slint::interpreter::ComponentInstance> instance;
    std::shared_ptr<CallbackData> callbacks;
    std::shared_ptr<OffscreenTarget> offscreen;  // null until bound
};

JSClassID component_class_id = 0;
JSClassID instance_class_id = 0;

struct ComponentBox {
    std::shared_ptr<CompiledComponent> handle;
};

struct InstanceBox {
    std::shared_ptr<InstanceState> handle;
};

void component_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<ComponentBox*>(JS_GetOpaque(val, component_class_id));
}

void instance_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    // The registered JS handlers live on a hidden property of this instance
    // object (the "handlers holder"), so normal GC reclaims them with the
    // instance — there are no module-held JS references to release here.
    delete static_cast<InstanceBox*>(JS_GetOpaque(val, instance_class_id));
}

std::shared_ptr<CompiledComponent> get_component(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<ComponentBox*>(JS_GetOpaque2(ctx, value, component_class_id));
    return box ? box->handle : nullptr;
}

std::shared_ptr<InstanceState> get_instance(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<InstanceBox*>(JS_GetOpaque2(ctx, value, instance_class_id));
    return box ? box->handle : nullptr;
}

JSValue new_component_object(JSContext* ctx, std::shared_ptr<CompiledComponent> handle) {
    JSValue obj = JS_NewObjectClass(ctx, component_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new ComponentBox{std::move(handle)});
    return obj;
}

// --- Error contract ------------------------------------------------------------

enum class SlintErr {
    Unavailable,
    CompileFailed,
    NoComponent,
    UnknownProperty,
    UnknownCallback,
    TypeError,
    PermissionDenied,
    InvalidArgument,
    Unsupported,
};

const char* code_string(SlintErr code) {
    switch (code) {
        case SlintErr::Unavailable: return "slint_unavailable";
        case SlintErr::CompileFailed: return "slint_compile_failed";
        case SlintErr::NoComponent: return "slint_no_component";
        case SlintErr::UnknownProperty: return "slint_unknown_property";
        case SlintErr::UnknownCallback: return "slint_unknown_callback";
        case SlintErr::TypeError: return "slint_type_error";
        case SlintErr::PermissionDenied: return "slint_permission_denied";
        case SlintErr::InvalidArgument: return "slint_invalid_argument";
        case SlintErr::Unsupported: return "slint_unsupported";
    }
    return "slint_error";
}

// A single compile diagnostic, in the shape exposed to JavaScript.
struct DiagEntry {
    std::string message;
    int line = 0;
    int column = 0;
};

// Builds a SlintError following the shared module error shape. Runs on the JS
// thread. component/property/cause are optional (empty strings omit them);
// diagnostics is attached when non-empty.
JSValue make_error(JSContext* ctx, SlintErr code, const char* operation,
    const std::string& cause = "", const std::string& component = "",
    const std::string& property = "", const std::vector<DiagEntry>& diagnostics = {}) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "SlintError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_slint"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code_string(code)));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    const std::string message = cause.empty()
        ? std::string(operation) + " failed (" + code_string(code) + ")"
        : std::string(operation) + " failed: " + cause;
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    if (!component.empty()) {
        JS_SetPropertyStr(ctx, error, "component", JS_NewString(ctx, component.c_str()));
    }
    if (!property.empty()) {
        JS_SetPropertyStr(ctx, error, "property", JS_NewString(ctx, property.c_str()));
    }
    if (!cause.empty()) {
        JS_SetPropertyStr(ctx, error, "cause", JS_NewString(ctx, cause.c_str()));
    }
    if (!diagnostics.empty()) {
        JSValue array = JS_NewArray(ctx);
        uint32_t index = 0;
        for (const auto& diag : diagnostics) {
            JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "message", JS_NewString(ctx, diag.message.c_str()));
            JS_SetPropertyStr(ctx, entry, "line", JS_NewInt32(ctx, diag.line));
            JS_SetPropertyStr(ctx, entry, "column", JS_NewInt32(ctx, diag.column));
            JS_SetPropertyUint32(ctx, array, index++, entry);
        }
        JS_SetPropertyStr(ctx, error, "diagnostics", array);
    }
    return error;
}

// --- Promise helpers (compilation is synchronous on the JS thread) ------------

// Returns an already-resolved promise carrying `value` (ownership transferred).
JSValue resolved_promise(JSContext* ctx, JSValue value) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

// Returns an already-rejected promise carrying `error` (ownership transferred).
JSValue rejected_promise(JSContext* ctx, JSValue error) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, error);
        return promise;
    }
    JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &error);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, error);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

// --- compile() / compileFile() -------------------------------------------------

// Convert interpreter diagnostics into our JS-facing entries, returning whether
// any are at error level.
bool collect_diagnostics(const slint::SharedVector<slint::interpreter::Diagnostic>& diags,
    std::vector<DiagEntry>& out, std::string& firstError) {
    bool hasError = false;
    for (const auto& diag : diags) {
        DiagEntry entry;
        entry.message = std::string(std::string_view(diag.message));
        entry.line = static_cast<int>(diag.line);
        entry.column = static_cast<int>(diag.column);
        if (diag.level == slint::interpreter::DiagnosticLevel::Error) {
            hasError = true;
            if (firstError.empty()) {
                firstError = entry.message;
            }
        }
        out.push_back(std::move(entry));
    }
    return hasError;
}

// Compile already-loaded markup into a Component. Returns a resolved or rejected
// promise. `includePath` (optional) lets component imports resolve relative to a
// directory. Always runs on the JS thread.
JSValue compile_source(JSContext* ctx, const std::string& source, const std::string& path,
    const std::string& includePath) {
    slint::interpreter::ComponentCompiler compiler;
    if (!includePath.empty()) {
        slint::SharedVector<slint::SharedString> paths;
        paths.push_back(slint::SharedString(std::string_view(includePath)));
        compiler.set_include_paths(paths);
    }

    std::optional<slint::interpreter::ComponentDefinition> definition =
        compiler.build_from_source(source, path);

    std::vector<DiagEntry> diagnostics;
    std::string firstError;
    const bool hasError = collect_diagnostics(compiler.diagnostics(), diagnostics, firstError);

    if (!definition) {
        if (hasError) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::CompileFailed, "compile", firstError, "", "", diagnostics));
        }
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::NoComponent, "compile",
                "no exported component found", "", "", diagnostics));
    }

    auto component = std::make_shared<CompiledComponent>(CompiledComponent{std::move(*definition)});
    return resolved_promise(ctx, new_component_object(ctx, std::move(component)));
}

JSValue slint_compile(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1 || !JS_IsString(argv[0])) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "compile", "source string is required"));
    }
    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "compile", "could not read source"));
    }
    std::string source(text, len);
    JS_FreeCString(ctx, text);
    return compile_source(ctx, source, "<wl2:slint compile>", "");
}

JSValue slint_compile_file(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "runtime unavailable");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "compileFile", "specifier string is required"));
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "compileFile", "could not read specifier"));
    }
    std::string specifier(text);
    JS_FreeCString(ctx, text);

    std::string source;
    std::string includePath;
    std::error_code ec;
    if (specifier.rfind("wl2:", 0) == 0) {
        auto loaded = runtime->loadTextResource(specifier);
        if (!loaded) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::InvalidArgument, "compileFile",
                    loaded.error().code() + ": " + loaded.error().message()));
        }
        source = loaded.value();

        // Map the resource onto a mounted host directory (development-time
        // --map-resource), if any, so its imports resolve from disk. Purely
        // embedded resources have no host directory; their imports must also be
        // embedded — fine for the common single-file UI.
        for (const auto& mount : runtime->resources().mounts()) {
            if (specifier.rfind(mount.prefix, 0) != 0) {
                continue;
            }
            std::string relative = specifier.substr(mount.prefix.size());
            while (!relative.empty() && relative.front() == '/') {
                relative.erase(relative.begin());
            }
            std::filesystem::path hostPath = mount.root / relative;
            if (hostPath.has_parent_path() && std::filesystem::is_directory(hostPath.parent_path(), ec)) {
                includePath = hostPath.parent_path().string();
            }
            break;
        }
    } else {
        std::string fsPath = specifier;
        constexpr std::string_view filePrefix = "file:";
        if (fsPath.rfind(filePrefix, 0) == 0) {
            fsPath = fsPath.substr(filePrefix.size());
        }

        auto resolved = runtime->resolveFilesystemReadPath(fsPath);
        if (!resolved) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::PermissionDenied, "compileFile",
                    "filesystem read is not permitted for this path"));
        }

        auto status = std::filesystem::status(*resolved, ec);
        if (ec || !std::filesystem::exists(status)) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::InvalidArgument, "compileFile", "file not found"));
        }
        if (!std::filesystem::is_regular_file(status)) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::InvalidArgument, "compileFile", "path is not a regular file"));
        }

        std::ifstream in(*resolved, std::ios::binary);
        if (!in) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::InvalidArgument, "compileFile", "unable to open file"));
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        if (in.bad()) {
            return rejected_promise(ctx,
                make_error(ctx, SlintErr::InvalidArgument, "compileFile", "error while reading file"));
        }
        source = ss.str();

        if (resolved->has_parent_path() && std::filesystem::is_directory(resolved->parent_path(), ec)) {
            includePath = resolved->parent_path().string();
        }
    }
    return compile_source(ctx, source, specifier, includePath);
}

// --- Native dialogs ------------------------------------------------------------

JSValue authorize_dialog(JSContext* ctx, const char* operation) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return make_error(ctx, SlintErr::Unavailable, operation, "runtime unavailable");
    }
    if (auto ok = runtime->authorizeUi(); !ok) {
        return make_error(ctx, SlintErr::PermissionDenied, operation,
            ok.error().code() + ": " + ok.error().message());
    }
    return JS_UNDEFINED;
}

bool js_to_std_string(JSContext* ctx, JSValueConst value, std::string& out) {
    if (!JS_IsString(value)) {
        return false;
    }
    const char* text = JS_ToCString(ctx, value);
    if (!text) {
        return false;
    }
    out = text;
    JS_FreeCString(ctx, text);
    return true;
}

#if WL2_SLINT_HAVE_NATIVE_DIALOGS

std::once_flag nfd_init_once;
bool nfd_initialized = false;

bool read_string_property(JSContext* ctx, JSValueConst obj, const char* property,
    std::string& out, std::string& error, bool required = false) {
    JSValue value = JS_GetPropertyStr(ctx, obj, property);
    if (JS_IsException(value)) {
        error = std::string("could not read option '") + property + "'";
        return false;
    }
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        if (required) {
            error = std::string("option '") + property + "' is required";
            return false;
        }
        return true;
    }
    if (!JS_IsString(value)) {
        JS_FreeValue(ctx, value);
        error = std::string("option '") + property + "' must be a string";
        return false;
    }
    const char* text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (!text) {
        error = std::string("could not read option '") + property + "'";
        return false;
    }
    out = text;
    JS_FreeCString(ctx, text);
    return true;
}

struct NativeDialogOptions {
    std::string title;
    std::string acceptLabel;
    std::string cancelLabel;
    std::string defaultPath;
    std::string defaultName;
    std::vector<std::string> filterNames;
    std::vector<std::string> filterSpecs;
    std::vector<nfdu8filteritem_t> filters;
};

bool read_filter_extensions(JSContext* ctx, JSValueConst value, std::string& out, std::string& error) {
    if (JS_IsString(value)) {
        const char* text = JS_ToCString(ctx, value);
        if (!text) {
            error = "could not read filter extensions";
            return false;
        }
        out = text;
        JS_FreeCString(ctx, text);
        return true;
    }

    if (!JS_IsArray(ctx, value)) {
        error = "filter extensions must be a string or array";
        return false;
    }

    JSValue lengthValue = JS_GetPropertyStr(ctx, value, "length");
    uint32_t length = 0;
    if (JS_ToUint32(ctx, &length, lengthValue) < 0) {
        JS_FreeValue(ctx, lengthValue);
        error = "could not read filter extensions length";
        return false;
    }
    JS_FreeValue(ctx, lengthValue);

    for (uint32_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, value, i);
        if (!JS_IsString(item)) {
            JS_FreeValue(ctx, item);
            error = "filter extension entries must be strings";
            return false;
        }
        const char* text = JS_ToCString(ctx, item);
        JS_FreeValue(ctx, item);
        if (!text) {
            error = "could not read filter extension";
            return false;
        }
        if (!out.empty()) {
            out += ",";
        }
        std::string ext(text);
        JS_FreeCString(ctx, text);
        while (!ext.empty() && ext.front() == '.') {
            ext.erase(ext.begin());
        }
        out += ext;
    }
    return true;
}

bool read_filters(JSContext* ctx, JSValueConst obj, NativeDialogOptions& options, std::string& error) {
    JSValue filters = JS_GetPropertyStr(ctx, obj, "filters");
    if (JS_IsException(filters)) {
        error = "could not read option 'filters'";
        return false;
    }
    if (JS_IsUndefined(filters) || JS_IsNull(filters)) {
        JS_FreeValue(ctx, filters);
        return true;
    }
    if (!JS_IsArray(ctx, filters)) {
        JS_FreeValue(ctx, filters);
        error = "option 'filters' must be an array";
        return false;
    }

    JSValue lengthValue = JS_GetPropertyStr(ctx, filters, "length");
    uint32_t length = 0;
    if (JS_ToUint32(ctx, &length, lengthValue) < 0) {
        JS_FreeValue(ctx, lengthValue);
        JS_FreeValue(ctx, filters);
        error = "could not read filters length";
        return false;
    }
    JS_FreeValue(ctx, lengthValue);

    options.filterNames.reserve(length);
    options.filterSpecs.reserve(length);
    options.filters.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, filters, i);
        if (!JS_IsObject(item)) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, filters);
            error = "filter entries must be objects";
            return false;
        }
        std::string name;
        std::string spec;
        if (!read_string_property(ctx, item, "name", name, error, true)) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, filters);
            return false;
        }

        JSValue extensions = JS_GetPropertyStr(ctx, item, "extensions");
        if (JS_IsUndefined(extensions) || JS_IsNull(extensions)) {
            JS_FreeValue(ctx, extensions);
            extensions = JS_GetPropertyStr(ctx, item, "spec");
        }
        if (!read_filter_extensions(ctx, extensions, spec, error)) {
            JS_FreeValue(ctx, extensions);
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, filters);
            return false;
        }
        JS_FreeValue(ctx, extensions);
        JS_FreeValue(ctx, item);

        options.filterNames.push_back(std::move(name));
        options.filterSpecs.push_back(std::move(spec));
    }
    JS_FreeValue(ctx, filters);

    for (std::size_t i = 0; i < options.filterNames.size(); ++i) {
        options.filters.push_back(
            nfdu8filteritem_t{options.filterNames[i].c_str(), options.filterSpecs[i].c_str()});
    }
    return true;
}

bool read_dialog_options(JSContext* ctx, JSValueConst value, NativeDialogOptions& options, std::string& error) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        return true;
    }
    if (!JS_IsObject(value)) {
        error = "dialog options must be an object";
        return false;
    }
    return read_string_property(ctx, value, "title", options.title, error)
        && read_string_property(ctx, value, "acceptLabel", options.acceptLabel, error)
        && read_string_property(ctx, value, "cancelLabel", options.cancelLabel, error)
        && read_string_property(ctx, value, "defaultPath", options.defaultPath, error)
        && read_string_property(ctx, value, "defaultName", options.defaultName, error)
        && read_filters(ctx, value, options, error);
}

bool ensure_nfd_initialized(std::string& error) {
    std::call_once(nfd_init_once, [&] {
        nfd_initialized = NFD_Init() == NFD_OKAY;
        if (!nfd_initialized && NFD_GetError()) {
            error = NFD_GetError();
        }
    });
    if (!nfd_initialized && error.empty() && NFD_GetError()) {
        error = NFD_GetError();
    }
    return nfd_initialized;
}

void shutdown_nfd() {
    if (nfd_initialized) {
        NFD_Quit();
        nfd_initialized = false;
    }
}

const nfdu8filteritem_t* filter_ptr(const NativeDialogOptions& options) {
    return options.filters.empty() ? nullptr : options.filters.data();
}

nfdfiltersize_t filter_count(const NativeDialogOptions& options) {
    return static_cast<nfdfiltersize_t>(options.filters.size());
}

const nfdu8char_t* cstr_or_null(const std::string& value) {
    return value.empty() ? nullptr : reinterpret_cast<const nfdu8char_t*>(value.c_str());
}

JSValue dialog_result(JSContext* ctx, const char* operation, nfdresult_t result, nfdu8char_t* outPath) {
    if (result == NFD_CANCEL) {
        return resolved_promise(ctx, JS_NULL);
    }
    if (result != NFD_OKAY || !outPath) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, operation, NFD_GetError() ? NFD_GetError() : "native dialog failed"));
    }
    JSValue path = JS_NewString(ctx, reinterpret_cast<const char*>(outPath));
    NFD_FreePathU8(outPath);
    return resolved_promise(ctx, path);
}

template <typename Args>
void apply_common_dialog_options(Args& args, const NativeDialogOptions& options) {
    args.filterList = filter_ptr(options);
    args.filterCount = filter_count(options);
    args.defaultPath = cstr_or_null(options.defaultPath);
}

#endif

JSValue native_dialog_unavailable(JSContext* ctx, const char* operation) {
    return rejected_promise(ctx, make_error(ctx, SlintErr::Unsupported, operation,
        "native dialogs were not built for this target"));
}

JSValue open_file_dialog(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    JSValue auth = authorize_dialog(ctx, "openFileDialog");
    if (!JS_IsUndefined(auth)) {
        return rejected_promise(ctx, auth);
    }
#if WL2_SLINT_HAVE_NATIVE_DIALOGS
    NativeDialogOptions options;
    std::string error;
    if (!read_dialog_options(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, options, error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::InvalidArgument, "openFileDialog", error));
    }
    if (!ensure_nfd_initialized(error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::Unavailable, "openFileDialog", error));
    }
    nfdopendialogu8args_t args{0};
    apply_common_dialog_options(args, options);
    nfdu8char_t* outPath = nullptr;
    // NFD writes *outPath; sequence the call before reading outPath. Passing both
    // NFD_*(&outPath, ...) and outPath as arguments to one call is unsequenced (UB)
    // and can hand dialog_result a stale null pointer.
    nfdresult_t result = NFD_OpenDialogU8_With(&outPath, &args);
    return dialog_result(ctx, "openFileDialog", result, outPath);
#else
    (void)argc;
    (void)argv;
    return native_dialog_unavailable(ctx, "openFileDialog");
#endif
}

JSValue save_file_dialog(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    JSValue auth = authorize_dialog(ctx, "saveFileDialog");
    if (!JS_IsUndefined(auth)) {
        return rejected_promise(ctx, auth);
    }
#if WL2_SLINT_HAVE_NATIVE_DIALOGS
    NativeDialogOptions options;
    std::string error;
    if (!read_dialog_options(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, options, error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::InvalidArgument, "saveFileDialog", error));
    }
    if (!ensure_nfd_initialized(error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::Unavailable, "saveFileDialog", error));
    }
    nfdsavedialogu8args_t args{0};
    apply_common_dialog_options(args, options);
    args.defaultName = cstr_or_null(options.defaultName);
    nfdu8char_t* outPath = nullptr;
    nfdresult_t result = NFD_SaveDialogU8_With(&outPath, &args);
    return dialog_result(ctx, "saveFileDialog", result, outPath);
#else
    (void)argc;
    (void)argv;
    return native_dialog_unavailable(ctx, "saveFileDialog");
#endif
}

JSValue pick_folder_dialog(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    JSValue auth = authorize_dialog(ctx, "pickFolderDialog");
    if (!JS_IsUndefined(auth)) {
        return rejected_promise(ctx, auth);
    }
#if WL2_SLINT_HAVE_NATIVE_DIALOGS
    NativeDialogOptions options;
    std::string error;
    if (!read_dialog_options(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, options, error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::InvalidArgument, "pickFolderDialog", error));
    }
    if (!ensure_nfd_initialized(error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::Unavailable, "pickFolderDialog", error));
    }
    nfdpickfolderu8args_t args{0};
    args.defaultPath = cstr_or_null(options.defaultPath);
    nfdu8char_t* outPath = nullptr;
    nfdresult_t result = NFD_PickFolderU8_With(&outPath, &args);
    return dialog_result(ctx, "pickFolderDialog", result, outPath);
#else
    (void)argc;
    (void)argv;
    return native_dialog_unavailable(ctx, "pickFolderDialog");
#endif
}

JSValue open_files_dialog(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    JSValue auth = authorize_dialog(ctx, "openFilesDialog");
    if (!JS_IsUndefined(auth)) {
        return rejected_promise(ctx, auth);
    }
#if WL2_SLINT_HAVE_NATIVE_DIALOGS
    NativeDialogOptions options;
    std::string error;
    if (!read_dialog_options(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, options, error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::InvalidArgument, "openFilesDialog", error));
    }
    if (!ensure_nfd_initialized(error)) {
        return rejected_promise(ctx, make_error(ctx, SlintErr::Unavailable, "openFilesDialog", error));
    }
    nfdopendialogu8args_t args{0};
    apply_common_dialog_options(args, options);
    const nfdpathset_t* outPaths = nullptr;
    nfdresult_t result = NFD_OpenDialogMultipleU8_With(&outPaths, &args);
    if (result == NFD_CANCEL) {
        return resolved_promise(ctx, JS_NULL);
    }
    if (result != NFD_OKAY) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "openFilesDialog", NFD_GetError() ? NFD_GetError() : "native dialog failed"));
    }
    nfdpathsetsize_t count = 0;
    if (NFD_PathSet_GetCount(outPaths, &count) != NFD_OKAY) {
        NFD_PathSet_Free(outPaths);
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "openFilesDialog", NFD_GetError() ? NFD_GetError() : "could not read selected paths"));
    }
    JSValue array = JS_NewArray(ctx);
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        nfdu8char_t* path = nullptr;
        if (NFD_PathSet_GetPathU8(outPaths, i, &path) == NFD_OKAY && path) {
            JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), JS_NewString(ctx, reinterpret_cast<const char*>(path)));
            NFD_PathSet_FreePathU8(path);
        }
    }
    NFD_PathSet_Free(outPaths);
    return resolved_promise(ctx, array);
#else
    (void)argc;
    (void)argv;
    return native_dialog_unavailable(ctx, "openFilesDialog");
#endif
}

// --- Component.create() --------------------------------------------------------

JSValue component_create(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto component = get_component(ctx, thisVal);
    if (!component) {
        return JS_ThrowTypeError(ctx, "create() called on a non-component");
    }
    auto callbacks = std::make_shared<CallbackData>();
    callbacks->ctx = ctx;
    // ComponentHandle has no default constructor, so build the state in-place
    // (aggregate init) rather than default-constructing and assigning.
    auto instance = std::make_shared<InstanceState>(
        InstanceState{component, component->definition.create(), callbacks});

    JSValue obj = JS_NewObjectClass(ctx, instance_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new InstanceBox{std::move(instance)});

    // Hidden, non-enumerable holder object that owns the registered JS handler
    // functions. The instance object owns the holder; CallbackData keeps only a
    // borrowed copy of its JSValue for lookup from the Slint closures.
    JSValue holder = JS_NewObject(ctx);
    callbacks->holder = holder;
    JS_DefinePropertyValueStr(ctx, obj, "\xff handlers", holder, 0);
    return obj;
}

// --- Instance.get() / set() ----------------------------------------------------

JSValue instance_get(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "get() called on a non-instance");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "get", "property name is required"));
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "get", "could not read property name"));
    }
    std::string property(name);
    JS_FreeCString(ctx, name);

    std::optional<slint::interpreter::Value> value =
        instance->instance->get_property(std::string_view(property));
    if (!value) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::UnknownProperty, "get", "no such property", "", property));
    }
    return wl2_slint_bridge::value_to_js(ctx, *value);
}

JSValue instance_set(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "set() called on a non-instance");
    }
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "set", "set(property, value) is required"));
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "set", "could not read property name"));
    }
    std::string property(name);
    JS_FreeCString(ctx, name);

    slint::interpreter::Value value;
    std::string error;
    if (!wl2_slint_bridge::js_to_value(ctx, argv[1], /*allowComposite=*/true, value, error)) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::TypeError, "set", error, "", property));
    }

    if (!instance->instance->set_property(std::string_view(property), value)) {
        // set_property fails for an unknown property or a type the property does
        // not accept; the interpreter does not distinguish, so report the more
        // common case while keeping the property name for diagnosis.
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::UnknownProperty, "set",
                "no such property, or value type mismatch", "", property));
    }
    return JS_UNDEFINED;
}

JSValue instance_set_image_from_frame_ring(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "setImageFromFrameRing() called on a non-instance");
    }
    if (argc < 2) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "setImageFromFrameRing", "property and shared-memory name are required"));
    }

    std::string property;
    if (!js_to_std_string(ctx, argv[0], property)) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "setImageFromFrameRing", "could not read property name"));
    }
    std::string name;
    if (!js_to_std_string(ctx, argv[1], name)) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "setImageFromFrameRing", "could not read shared-memory name"));
    }

    int64_t lastSequence = -1;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue last = JS_GetPropertyStr(ctx, argv[2], "lastSequence");
        if (!JS_IsUndefined(last) && !JS_IsNull(last)) {
            JS_ToInt64(ctx, &lastSequence, last);
        }
        JS_FreeValue(ctx, last);
    }

    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::PermissionDenied, "setImageFromFrameRing", "shared-memory access is not permitted without a runtime policy"));
    }
    if (auto allowed = runtime->authorizeSharedMemory(name); !allowed) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::PermissionDenied, "setImageFromFrameRing", allowed.error().message()));
    }

    auto opened = wl2::VideoBuffer::openExisting(name);
    if (!opened) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "setImageFromFrameRing", opened.error().message(), "", property));
    }
    auto video = std::move(opened.value());
    const int64_t sequence = video.sequence();
    if (lastSequence >= 0 && sequence <= lastSequence) {
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "updated", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, out, "sequence", JS_NewInt64(ctx, sequence));
        video.close();
        return out;
    }

    const auto format = video.format();
    if (!format || *format != wl2::VideoPixelFormat::Rgba32) {
        video.close();
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::Unsupported, "setImageFromFrameRing", "only RGBA32 FrameRing images are supported", "", property));
    }
    auto frame = video.frame(0);
    if (!frame) {
        video.close();
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "setImageFromFrameRing", frame.error().message(), "", property));
    }
    if (!frame.value().data || frame.value().width <= 0 || frame.value().height <= 0 ||
        frame.value().width > UINT32_MAX || frame.value().height > UINT32_MAX ||
        frame.value().scanWidth < frame.value().width * 4 ||
        frame.value().size < static_cast<size_t>(frame.value().scanWidth * frame.value().height)) {
        video.close();
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "setImageFromFrameRing", "FrameRing returned invalid RGBA frame metadata", "", property));
    }

    slint::SharedPixelBuffer<slint::Rgba8Pixel> pixels{
        static_cast<uint32_t>(frame.value().width),
        static_cast<uint32_t>(frame.value().height)};
    auto* dest = reinterpret_cast<unsigned char*>(pixels.begin());
    const auto* source = reinterpret_cast<const unsigned char*>(frame.value().data);
    const auto rowBytes = static_cast<size_t>(frame.value().width * 4);
    for (int64_t y = 0; y < frame.value().height; ++y) {
        std::memcpy(dest + static_cast<size_t>(y) * rowBytes,
            source + static_cast<size_t>(y * frame.value().scanWidth),
            rowBytes);
    }

    slint::interpreter::Value value{slint::Image{pixels}};
    if (!instance->instance->set_property(std::string_view(property), value)) {
        video.close();
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::UnknownProperty, "setImageFromFrameRing",
                "no such image property, or property is not assignable from an image", "", property));
    }

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "updated", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, out, "width", JS_NewInt64(ctx, frame.value().width));
    JS_SetPropertyStr(ctx, out, "height", JS_NewInt64(ctx, frame.value().height));
    JS_SetPropertyStr(ctx, out, "stride", JS_NewInt64(ctx, frame.value().scanWidth));
    JS_SetPropertyStr(ctx, out, "sequence", JS_NewInt64(ctx, sequence));
    JS_SetPropertyStr(ctx, out, "format", JS_NewString(ctx, "rgba8"));
    JS_SetPropertyStr(ctx, out, "origin", JS_NewString(ctx, "top-left"));
    video.close();
    return out;
}

// --- Off-screen rendering (UI-on-3D producer) ---------------------------------

// Select the headless off-screen platform for this process. Slint's platform is
// process-global and one-shot, so this must be called before compile()/create()
// and is mutually exclusive with the windowed backend (Component.run()/show()).
JSValue slint_use_offscreen_rendering(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    (void)argc;
    (void)argv;
    if (!wl2_slint_offscreen::ensure_offscreen_platform()) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::Unsupported, "useOffscreenRendering",
                "off-screen platform could not be installed (a windowed UI is already active in this process)"));
    }
    return JS_NewBool(ctx, true);
}

slint::PointerEventButton parse_pointer_button(const std::string& name) {
    if (name == "right") return slint::PointerEventButton::Right;
    if (name == "middle") return slint::PointerEventButton::Middle;
    return slint::PointerEventButton::Left;
}

// Render the bound component into its FrameRing slot (RGB8 -> RGBA8, top-left
// origin, opaque/premultiplied) and advance the ring. Returns an exception on
// failure, JS_UNDEFINED on success (with the new sequence in *outSeq).
JSValue offscreen_publish(JSContext* ctx, OffscreenTarget& target, const char* op, int64_t* outSeq) {
    slint::platform::update_timers_and_animations();
    const size_t count = static_cast<size_t>(target.width) * target.height;
    if (target.scratch.size() != count) {
        target.scratch.assign(count, slint::Rgb8Pixel{0, 0, 0});
    }
    target.adapter->software_renderer().render(
        std::span<slint::Rgb8Pixel>(target.scratch.data(), count), target.width);

    auto frame = target.ring.frame(0);
    if (!frame) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::InvalidArgument, op, frame.error().message()));
    }
    auto& f = frame.value();
    if (!f.data || f.scanWidth < static_cast<int64_t>(target.width) * 4) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "FrameRing returned an invalid RGBA frame"));
    }
    auto* dst = reinterpret_cast<unsigned char*>(f.data);
    const slint::Rgb8Pixel* src = target.scratch.data();
    for (uint32_t y = 0; y < target.height; ++y) {
        unsigned char* row = dst + static_cast<size_t>(y) * f.scanWidth;
        const slint::Rgb8Pixel* srow = src + static_cast<size_t>(y) * target.width;
        for (uint32_t x = 0; x < target.width; ++x) {
            row[x * 4 + 0] = srow[x].r;
            row[x * 4 + 1] = srow[x].g;
            row[x * 4 + 2] = srow[x].b;
            row[x * 4 + 3] = 255;
        }
    }
    target.ring.next(1);
    if (outSeq) {
        *outSeq = target.ring.sequence();
    }
    return JS_UNDEFINED;
}

JSValue offscreen_metadata(JSContext* ctx, OffscreenTarget& target, int64_t seq) {
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "updated", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, out, "width", JS_NewInt64(ctx, target.width));
    JS_SetPropertyStr(ctx, out, "height", JS_NewInt64(ctx, target.height));
    JS_SetPropertyStr(ctx, out, "sequence", JS_NewInt64(ctx, seq));
    JS_SetPropertyStr(ctx, out, "format", JS_NewString(ctx, "rgba8"));
    JS_SetPropertyStr(ctx, out, "origin", JS_NewString(ctx, "top-left"));
    JS_SetPropertyStr(ctx, out, "alpha", JS_NewString(ctx, "premultiplied"));
    return out;
}

JSValue instance_render_offscreen_to(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "renderOffscreenTo() called on a non-instance");
    }
    const char* op = "renderOffscreenTo";
    if (argc < 1) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::InvalidArgument, op, "shared-memory name is required"));
    }
    std::string name;
    if (!js_to_std_string(ctx, argv[0], name)) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::InvalidArgument, op, "could not read shared-memory name"));
    }
    int64_t width = 0;
    int64_t height = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue size = JS_GetPropertyStr(ctx, argv[1], "size");
        if (JS_IsArray(ctx, size) > 0) {
            JSValue w = JS_GetPropertyUint32(ctx, size, 0);
            JSValue h = JS_GetPropertyUint32(ctx, size, 1);
            JS_ToInt64(ctx, &width, w);
            JS_ToInt64(ctx, &height, h);
            JS_FreeValue(ctx, w);
            JS_FreeValue(ctx, h);
        }
        JS_FreeValue(ctx, size);
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "options.size = [width, height] is required"));
    }

    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::PermissionDenied, op, "shared-memory access is not permitted without a runtime policy"));
    }
    if (auto allowed = runtime->authorizeSharedMemory(name); !allowed) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::PermissionDenied, op, allowed.error().message()));
    }

    auto& reg = wl2_slint_offscreen::registry();
    if (!reg.installed) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::Unsupported, op,
                "call useOffscreenRendering() before compile()/create() to enable off-screen UI-on-3D"));
    }
    reg.pendingWidth = static_cast<uint32_t>(width);
    reg.pendingHeight = static_cast<uint32_t>(height);
    reg.lastAdapter = nullptr;

    // Touching window() lazily creates the WindowAdapter through our platform.
    // window() is const but the dispatch_*/resize entry points are non-const;
    // slint::Window is a refcounted handle, so const_cast is the idiomatic path.
    slint::Window& window = const_cast<slint::Window&>(instance->instance->window());
    wl2_slint_offscreen::OffscreenWindowAdapter* adapter = reg.lastAdapter;
    if (!adapter) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::Unsupported, op,
                "off-screen window adapter was not created (was the instance already shown?)"));
    }
    window.dispatch_resize_event(
        slint::LogicalSize({static_cast<float>(width), static_cast<float>(height)}));

    auto created = wl2::VideoBuffer::create(name, width, height, wl2::VideoPixelFormat::Rgba32, 30, 3);
    if (!created) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::InvalidArgument, op, created.error().message()));
    }
    auto target = std::make_shared<OffscreenTarget>();
    target->adapter = adapter;
    target->ring = std::move(created.value());
    target->width = static_cast<uint32_t>(width);
    target->height = static_cast<uint32_t>(height);
    instance->offscreen = target;

    int64_t seq = 0;
    JSValue err = offscreen_publish(ctx, *target, op, &seq);
    if (JS_IsException(err)) {
        instance->offscreen.reset();
        return err;
    }
    return offscreen_metadata(ctx, *target, seq);
}

JSValue instance_render_offscreen_frame(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "renderOffscreenFrame() called on a non-instance");
    }
    if (!instance->offscreen) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "renderOffscreenFrame", "instance is not bound to an off-screen target"));
    }
    int64_t seq = 0;
    JSValue err = offscreen_publish(ctx, *instance->offscreen, "renderOffscreenFrame", &seq);
    if (JS_IsException(err)) {
        return err;
    }
    return offscreen_metadata(ctx, *instance->offscreen, seq);
}

JSValue instance_inject_pointer(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "injectPointer() called on a non-instance");
    }
    const char* op = "injectPointer";
    if (!instance->offscreen) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "instance is not bound to an off-screen target"));
    }
    if (argc < 3) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "injectPointer(x, y, phase[, button]) requires x, y and a phase"));
    }
    double x = 0;
    double y = 0;
    if (JS_ToFloat64(ctx, &x, argv[0]) != 0 || JS_ToFloat64(ctx, &y, argv[1]) != 0) {
        return JS_Throw(ctx, make_error(ctx, SlintErr::InvalidArgument, op, "x and y must be numbers"));
    }
    std::string phase;
    js_to_std_string(ctx, argv[2], phase);
    std::string buttonName = "left";
    if (argc >= 4 && JS_IsString(argv[3])) {
        js_to_std_string(ctx, argv[3], buttonName);
    }
    const slint::PointerEventButton button = parse_pointer_button(buttonName);
    slint::LogicalPosition pos({static_cast<float>(x), static_cast<float>(y)});
    slint::Window& window = const_cast<slint::Window&>(instance->instance->window());
    if (phase == "press") {
        window.dispatch_pointer_press_event(pos, button);
    } else if (phase == "release") {
        window.dispatch_pointer_release_event(pos, button);
    } else if (phase == "move") {
        window.dispatch_pointer_move_event(pos);
    } else if (phase == "click") {
        window.dispatch_pointer_press_event(pos, button);
        window.dispatch_pointer_release_event(pos, button);
    } else {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "phase must be press, release, move, or click"));
    }
    int64_t seq = 0;
    JSValue err = offscreen_publish(ctx, *instance->offscreen, op, &seq);
    if (JS_IsException(err)) {
        return err;
    }
    return offscreen_metadata(ctx, *instance->offscreen, seq);
}

JSValue instance_inject_key(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "injectKey() called on a non-instance");
    }
    const char* op = "injectKey";
    if (!instance->offscreen) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "instance is not bound to an off-screen target"));
    }
    if (argc < 1) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "injectKey(text[, phase]) requires text"));
    }
    std::string text;
    js_to_std_string(ctx, argv[0], text);
    std::string phase = "click";
    if (argc >= 2 && JS_IsString(argv[1])) {
        js_to_std_string(ctx, argv[1], phase);
    }
    slint::Window& window = const_cast<slint::Window&>(instance->instance->window());
    const slint::SharedString shared{text};
    if (phase == "press") {
        window.dispatch_key_press_event(shared);
    } else if (phase == "release") {
        window.dispatch_key_release_event(shared);
    } else if (phase == "click") {
        window.dispatch_key_press_event(shared);
        window.dispatch_key_release_event(shared);
    } else {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, op, "phase must be press, release, or click"));
    }
    int64_t seq = 0;
    JSValue err = offscreen_publish(ctx, *instance->offscreen, op, &seq);
    if (JS_IsException(err)) {
        return err;
    }
    return offscreen_metadata(ctx, *instance->offscreen, seq);
}

// --- Instance.on() / invoke() --------------------------------------------------

// The Slint closure registered for a callback. Captures only a weak reference to
// the callback registry plus the callback name, so it never keeps the instance
// alive. When fired (on the UI/JS thread, during the event loop or invoke()), it
// looks up the current JS handler, marshals the arguments to JS, calls it, and
// marshals the return value back to a Slint Value.
slint::interpreter::Value dispatch_callback(std::weak_ptr<CallbackData> weak,
    const std::string& name, std::span<const slint::interpreter::Value> args) {
    auto data = weak.lock();
    if (!data || !data->ctx) {
        return slint::interpreter::Value();
    }
    JSContext* ctx = data->ctx;
    JSValue fn = JS_GetPropertyStr(ctx, data->holder, name.c_str());  // owned; freed below
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return slint::interpreter::Value();
    }

    std::vector<JSValue> jsArgs;
    jsArgs.reserve(args.size());
    for (const auto& arg : args) {
        jsArgs.push_back(wl2_slint_bridge::value_to_js(ctx, arg));
    }
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, static_cast<int>(jsArgs.size()),
        jsArgs.empty() ? nullptr : jsArgs.data());
    for (JSValue arg : jsArgs) {
        JS_FreeValue(ctx, arg);
    }

    slint::interpreter::Value value;  // Void by default (for void callbacks / errors)
    if (JS_IsException(result)) {
        // A throwing handler must not propagate into the Slint event loop; report
        // it to the console and return Void so the loop keeps running.
        JSValue exception = JS_GetException(ctx);
        const char* text = JS_ToCString(ctx, exception);
        if (text) {
            // Best effort; the engine's console captures uncaught errors elsewhere.
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, exception);
    } else {
        std::string error;
        if (!wl2_slint_bridge::js_to_value(ctx, result, /*allowComposite=*/true, value, error)) {
            value = slint::interpreter::Value();
        }
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, fn);
    return value;
}

JSValue instance_on(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "on() called on a non-instance");
    }
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "on", "on(callback, function) is required"));
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "on", "could not read callback name"));
    }
    std::string callback(name);
    JS_FreeCString(ctx, name);

    auto& data = *instance->callbacks;

    // Store (or replace) the handler on the holder object; JS_SetPropertyStr
    // frees any previous value and the holder takes ownership of the new ref.
    JS_SetPropertyStr(ctx, data.holder, callback.c_str(), JS_DupValue(ctx, argv[1]));

    // Wire the Slint callback once per name; the closure re-reads the current
    // handler from the holder each time it fires.
    if (data.registered.find(callback) == data.registered.end()) {
        std::weak_ptr<CallbackData> weak = instance->callbacks;
        const bool ok = instance->instance->set_callback(std::string_view(callback),
            [weak, callback](std::span<const slint::interpreter::Value> args) {
                return dispatch_callback(weak, callback, args);
            });
        if (!ok) {
            JS_SetPropertyStr(ctx, data.holder, callback.c_str(), JS_UNDEFINED);
            return JS_Throw(ctx,
                make_error(ctx, SlintErr::UnknownCallback, "on", "no such callback", "", callback));
        }
        data.registered.insert(callback);
    }
    return JS_UNDEFINED;
}

JSValue instance_invoke(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "invoke() called on a non-instance");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "invoke", "callback name is required"));
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "invoke", "could not read callback name"));
    }
    std::string callback(name);
    JS_FreeCString(ctx, name);

    std::vector<slint::interpreter::Value> args;
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        slint::interpreter::Value value;
        std::string error;
        if (!wl2_slint_bridge::js_to_value(ctx, argv[i], /*allowComposite=*/true, value, error)) {
            return JS_Throw(ctx, make_error(ctx, SlintErr::TypeError, "invoke", error, "", callback));
        }
        args.push_back(std::move(value));
    }

    std::optional<slint::interpreter::Value> result = instance->instance->invoke(
        std::string_view(callback), std::span<const slint::interpreter::Value>(args.data(), args.size()));
    if (!result) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::UnknownCallback, "invoke",
                "no such callback, or argument mismatch", "", callback));
    }
    return wl2_slint_bridge::value_to_js(ctx, *result);
}

// --- Instance.colorScheme() ----------------------------------------------------

JSValue instance_color_scheme(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto instance = get_instance(ctx, thisVal);
    if (!instance) {
        return JS_ThrowTypeError(ctx, "colorScheme() called on a non-instance");
    }

    const char* name = "unknown";
    switch (instance->instance->window().window_handle().color_scheme()) {
        case slint::cbindgen_private::ColorScheme::Dark:
            name = "dark";
            break;
        case slint::cbindgen_private::ColorScheme::Light:
            name = "light";
            break;
        case slint::cbindgen_private::ColorScheme::Unknown:
            break;
    }
    return JS_NewString(ctx, name);
}

// --- Instance.show() / hide() --------------------------------------------------
// Opening a window is gated by Runtime::authorizeUi() (denied by default).
// Headless tests never reach show().

JSValue instance_show(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto instance = get_instance(ctx, thisVal);
    if (!runtime || !instance) {
        return JS_ThrowTypeError(ctx, "show() called on a non-instance");
    }
    if (auto ok = runtime->authorizeUi(); !ok) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::PermissionDenied, "show",
                ok.error().code() + ": " + ok.error().message()));
    }
    instance->instance->show();
    return JS_UNDEFINED;
}

JSValue instance_hide(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto instance = get_instance(ctx, thisVal);
    if (!runtime || !instance) {
        return JS_ThrowTypeError(ctx, "hide() called on a non-instance");
    }
    if (auto ok = runtime->authorizeUi(); !ok) {
        return JS_Throw(ctx,
            make_error(ctx, SlintErr::PermissionDenied, "hide",
                ok.error().code() + ": " + ok.error().message()));
    }
    instance->instance->hide();
    return JS_UNDEFINED;
}

// --- Component.run() / quit() --------------------------------------------------
// First-version event-loop model: Slint owns the loop and run() blocks the
// JS/main thread until the last window closes or quit() is called. A short
// recurring slint::Timer pumps the engine's async queue and pending JS jobs on
// the UI thread, so JavaScript promises and other async modules keep settling
// while the UI is up. run() requires the UI capability.

JSValue component_run(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto component = get_component(ctx, thisVal);
    if (!runtime || !component) {
        return JS_ThrowTypeError(ctx, "run() called on a non-component");
    }
    if (auto ok = runtime->authorizeUi(); !ok) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::PermissionDenied, "run",
                ok.error().code() + ": " + ok.error().message()));
    }
    if (!wl2_slint_runtime::state().beginLoop()) {
        return rejected_promise(ctx,
            make_error(ctx, SlintErr::InvalidArgument, "run", "the event loop is already running"));
    }

    JSRuntime* rt = JS_GetRuntime(ctx);
    // Bridge the two loops: drain native completions (which resolve/reject
    // promises) and run the resulting JS jobs on the UI thread every tick.
    slint::Timer pump;
    pump.start(slint::TimerMode::Repeated, std::chrono::milliseconds(5), [rt, runtime] {
        runtime->async().drain();
        JSContext* jobContext = nullptr;
        while (JS_ExecutePendingJob(rt, &jobContext) > 0) {
        }
    });

    slint::run_event_loop();  // blocks until quit_event_loop() / last window closes

    pump.stop();
    wl2_slint_runtime::state().endLoop();
    return resolved_promise(ctx, JS_UNDEFINED);
}

JSValue component_quit(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto component = get_component(ctx, thisVal);
    if (!component) {
        return JS_ThrowTypeError(ctx, "quit() called on a non-component");
    }
    slint::quit_event_loop();  // no-op if the loop is not running
    return JS_UNDEFINED;
}

// --- Module wiring -------------------------------------------------------------

void register_component_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (component_class_id == 0) {
        JS_NewClassID(&component_class_id);
    }
    JSClassDef def{};
    def.class_name = "SlintComponent";
    def.finalizer = component_finalizer;
    JS_NewClass(rt, component_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "create", JS_NewCFunction(ctx, component_create, "create", 0));
    JS_SetPropertyStr(ctx, proto, "run", JS_NewCFunction(ctx, component_run, "run", 0));
    JS_SetPropertyStr(ctx, proto, "quit", JS_NewCFunction(ctx, component_quit, "quit", 0));
    JS_SetClassProto(ctx, component_class_id, proto);
}

void register_instance_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (instance_class_id == 0) {
        JS_NewClassID(&instance_class_id);
    }
    JSClassDef def{};
    def.class_name = "SlintInstance";
    def.finalizer = instance_finalizer;
    JS_NewClass(rt, instance_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "get", JS_NewCFunction(ctx, instance_get, "get", 1));
    JS_SetPropertyStr(ctx, proto, "set", JS_NewCFunction(ctx, instance_set, "set", 2));
    JS_SetPropertyStr(ctx, proto, "setImageFromFrameRing", JS_NewCFunction(ctx, instance_set_image_from_frame_ring, "setImageFromFrameRing", 2));
    JS_SetPropertyStr(ctx, proto, "renderOffscreenTo", JS_NewCFunction(ctx, instance_render_offscreen_to, "renderOffscreenTo", 2));
    JS_SetPropertyStr(ctx, proto, "renderOffscreenFrame", JS_NewCFunction(ctx, instance_render_offscreen_frame, "renderOffscreenFrame", 0));
    JS_SetPropertyStr(ctx, proto, "injectPointer", JS_NewCFunction(ctx, instance_inject_pointer, "injectPointer", 4));
    JS_SetPropertyStr(ctx, proto, "injectKey", JS_NewCFunction(ctx, instance_inject_key, "injectKey", 2));
    JS_SetPropertyStr(ctx, proto, "on", JS_NewCFunction(ctx, instance_on, "on", 2));
    JS_SetPropertyStr(ctx, proto, "invoke", JS_NewCFunction(ctx, instance_invoke, "invoke", 1));
    JS_SetPropertyStr(ctx, proto, "colorScheme", JS_NewCFunction(ctx, instance_color_scheme, "colorScheme", 0));
    JS_SetPropertyStr(ctx, proto, "show", JS_NewCFunction(ctx, instance_show, "show", 0));
    JS_SetPropertyStr(ctx, proto, "hide", JS_NewCFunction(ctx, instance_hide, "hide", 0));
    JS_SetClassProto(ctx, instance_class_id, proto);
}

int init_slint_module(JSContext* ctx, JSModuleDef* module) {
    register_component_class(ctx);
    register_instance_class(ctx);
    JS_SetModuleExport(ctx, module, "compile", JS_NewCFunction(ctx, slint_compile, "compile", 2));
    JS_SetModuleExport(ctx, module, "compileFile",
        JS_NewCFunction(ctx, slint_compile_file, "compileFile", 2));
    JS_SetModuleExport(ctx, module, "openFileDialog",
        JS_NewCFunction(ctx, open_file_dialog, "openFileDialog", 1));
    JS_SetModuleExport(ctx, module, "openFilesDialog",
        JS_NewCFunction(ctx, open_files_dialog, "openFilesDialog", 1));
    JS_SetModuleExport(ctx, module, "saveFileDialog",
        JS_NewCFunction(ctx, save_file_dialog, "saveFileDialog", 1));
    JS_SetModuleExport(ctx, module, "pickFolderDialog",
        JS_NewCFunction(ctx, pick_folder_dialog, "pickFolderDialog", 1));
    JS_SetModuleExport(ctx, module, "useOffscreenRendering",
        JS_NewCFunction(ctx, slint_use_offscreen_rendering, "useOffscreenRendering", 0));
    return 0;
}

#endif  // WL2_HAVE_QUICKJS

}  // namespace

wl2::ModuleInfo wl2_slint_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:slint", wl2_slint_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:slint",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "ac80212f-1920-4b74-adb0-36e49971d278",
        .summary = "Declarative UI module backed by the Slint interpreter.",
        .api = SlintApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_slint_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    if (auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx))) {
        runtime->async().registerShutdownHook([] { wl2_slint_runtime::state().shutdown(); });
#if WL2_SLINT_HAVE_NATIVE_DIALOGS
        runtime->async().registerShutdownHook([] { shutdown_nfd(); });
#endif
    }
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_slint_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "compile");
    JS_AddModuleExport(ctx, module, "compileFile");
    JS_AddModuleExport(ctx, module, "openFileDialog");
    JS_AddModuleExport(ctx, module, "openFilesDialog");
    JS_AddModuleExport(ctx, module, "saveFileDialog");
    JS_AddModuleExport(ctx, module, "pickFolderDialog");
    JS_AddModuleExport(ctx, module, "useOffscreenRendering");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_SLINT_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:slint";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "ac80212f-1920-4b74-adb0-36e49971d278";
    out->summary = "Declarative UI module backed by the Slint interpreter.";
    out->api = SlintApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
