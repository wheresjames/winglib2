#pragma once

/**
 * @file module.h
 * @brief Native module metadata and registration interfaces.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wl2/errors.h"

namespace wl2 {

class Runtime;

/// Current native module ABI version. Bumped to 2 when the dynamic module
/// registration ABI (`wl2_module_register`) and `required_wl2_version` were added.
/// Bumped to 3 when ABI-safe dependency metadata
/// (`wl2_module_info::dependencies`) was added.
/// Bumped to 4 when module build metadata (`wl2_module_info::build`) was added.
constexpr uint32_t ModuleAbiVersion = 4;

/// Oldest dynamic module ABI version the host still accepts. Modules reporting
/// an ABI in [ModuleMinAbiVersion, ModuleAbiVersion] load successfully. ABI v2
/// modules predate dependency metadata and are treated as declaring no
/// dependencies.
constexpr uint32_t ModuleMinAbiVersion = 2;

/// Whether a module dependency must be resolved or is only feature-enabling.
enum class ModuleDependencyKind {
    Required,
    Optional,
};

/// A dependency from one native module to another.
struct ModuleDependencyRequirement {
    /// Canonical dependency name, for example `wl2:threads`.
    std::string name;

    /// Version constraint. Supported initially: empty, `=x.y.z`,
    /// `>=x.y.z <a.b.c`.
    std::string versionRange;

    /// Optional stable module identifier that must match when provided.
    std::string stableId;

    /// Required dependencies fail resolution when missing; optional dependencies
    /// produce diagnostics and are skipped when unavailable or incompatible.
    ModuleDependencyKind kind = ModuleDependencyKind::Required;
};

/**
 * @brief Metadata describing a native module.
 *
 * ModuleInfo is used by statically linked modules and by the `wl2 showapi`
 * command. Dynamic modules expose equivalent data through `wl2_module_info`.
 *
 * @code{.cpp}
 * wl2::ModuleInfo info;
 * info.name = "wl2:example";
 * info.version = "0.1.0";
 * info.summary = "Example module.";
 * info.api = "Exports wl2:example.";
 * @endcode
 */
struct ModuleInfo {
    /// ABI version expected by the host.
    uint32_t abiVersion = ModuleAbiVersion;

    /// Canonical module name, for example `wl2:curl`.
    std::string name;

    /// Module implementation version.
    std::string version;

    /// Build stamp for this module build, separate from semantic version.
    std::string build;

    /// Stable module identifier suitable for tooling and manifests.
    std::string stableId;

    /// Short human-readable summary.
    std::string summary;

    /// Human-readable API description shown by `wl2 showapi`.
    std::string api;

    /// Whether the module claims it can be unloaded safely.
    bool unloadSafe = true;

    /// wl2 version the module was built against / requires. Empty for static
    /// modules compiled in the same tree. Dynamic modules report this through
    /// `wl2_module_info::required_wl2_version` and it is checked at load.
    std::string requiredWL2Version;

    /// Filesystem path the module was loaded from, for dynamic modules. Empty
    /// for statically linked modules.
    std::filesystem::path libraryPath;

    /// Native modules this module requires or can use when present.
    std::vector<ModuleDependencyRequirement> dependencies;
};

/// Policy for a dynamic module whose name is already registered (for example a
/// built-in). Deny rejects the duplicate; Allow lets the dynamic module shadow
/// the existing registration.
enum class ModuleShadowPolicy {
    Deny,
    Allow,
};

/**
 * @brief Function signature used to register a statically linked module.
 * @param runtime Host runtime receiving any engine-specific factories.
 * @return Metadata describing the registered module.
 */
using StaticModuleRegister = std::function<ModuleInfo(Runtime&)>;

/**
 * @brief QuickJS module factory function hidden behind a void pointer ABI boundary.
 * @param context Engine-specific JavaScript context pointer.
 * @param moduleName JavaScript module specifier being loaded.
 * @return Engine-specific module definition pointer, or null on failure.
 */
using QuickJsModuleFactory = void* (*)(void* context, const char* moduleName);

/// Native module factory registered with the QuickJS backend.
struct QuickJsNativeModule {
    /// JavaScript module specifier handled by this factory.
    std::string name;

    /// Factory called by the QuickJS module loader.
    QuickJsModuleFactory factory = nullptr;
};

/**
 * @brief Registry for static and discovered native modules.
 *
 * Runtime owns a ModuleLoader and initializes static modules once. Modules use
 * the Runtime passed to their register function to attach engine-specific
 * factories or host services.
 *
 * @code{.cpp}
 * wl2::RuntimeOptions options;
 * options.staticModules.push_back(wl2_curl_register_module);
 * wl2::Runtime runtime{std::move(options)};
 * runtime.initialize();
 *
 * if (auto* module = runtime.modules().find("wl2:curl")) {
 *     std::cout << module->summary << "\n";
 * }
 * @endcode
 */
class ModuleLoader {
public:
    /**
     * @brief Add a statically linked module registration function.
     * @param fn Registration callback. It is invoked during initializeStaticModules().
     */
    void registerStaticModule(StaticModuleRegister fn);

    /**
     * @brief Initialize all registered static modules.
     * @param runtime Host runtime passed to each module registration callback.
     * @return Metadata for initialized modules.
     */
    std::vector<ModuleInfo> initializeStaticModules(Runtime& runtime);

    /**
     * @brief Metadata for initialized modules.
     * @return Reference to the current initialized module list.
     */
    const std::vector<ModuleInfo>& modules() const noexcept { return modules_; }

    /**
     * @brief Find initialized module metadata by name.
     * @param name Canonical module name, for example `wl2:curl`.
     * @return Pointer to metadata when found, otherwise null.
     */
    const ModuleInfo* find(std::string_view name) const noexcept;

    /**
     * @brief Load and register a dynamic module from a shared library path.
     *
     * Opens the library, validates its ABI and wl2 version requirement, calls its
     * `wl2_module_register` entry point, and records its metadata. The library
     * handle is retained until process exit; there is no unload path.
     *
     * @param path Filesystem path to the module shared library.
     * @param runtime Host runtime receiving the module's factories.
     * @param policy Whether a name that is already registered may be shadowed.
     * @return Module metadata on success, or an Error describing the failure.
     */
    Result<ModuleInfo> loadDynamicModule(
        const std::filesystem::path& path,
        Runtime& runtime,
        ModuleShadowPolicy policy = ModuleShadowPolicy::Deny);

    /**
     * @brief Read a dynamic module's metadata without registering or running it.
     *
     * Opens the library, reads and validates `wl2_module_get_info`, and returns
     * the metadata. Used by `wl2 module validate`. The library is not retained.
     *
     * @param path Filesystem path to the module shared library.
     * @return Module metadata on success, or an Error describing the failure.
     */
    static Result<ModuleInfo> inspectDynamicModule(const std::filesystem::path& path);

private:
    std::vector<StaticModuleRegister> staticModules_;
    std::vector<ModuleInfo> modules_;
    // Opened dynamic library handles, retained for the process lifetime. They
    // are intentionally never closed: loaded modules remain loaded until exit.
    std::vector<void*> dynamicHandles_;
};

} // namespace wl2

extern "C" {
/**
 * @brief ABI-safe description of one module dependency.
 *
 * Added in module ABI v3. Dynamic modules expose an array of these through
 * `wl2_module_info::dependencies`. All pointers are owned by the module and must
 * remain valid for the duration of the `wl2_module_get_info` call; the host
 * copies the values immediately.
 */
struct wl2_module_dependency_info {
    /// Canonical dependency name, for example "wl2:threads". Required.
    const char* name;

    /// Version constraint such as ">=0.1.0 <0.2.0". May be null or empty.
    const char* version_range;

    /// Optional stable identifier that must match when provided. May be null.
    const char* stable_id;

    /// Non-zero for a required dependency, zero for an optional dependency.
    uint32_t required;
};

/**
 * @brief C ABI metadata shape for dynamic native modules.
 *
 * Dynamic modules should fill this structure without transferring ownership of
 * the pointed-to strings. The host copies or consumes the values immediately.
 */
struct wl2_module_info {
    /// ABI version implemented by the module.
    uint32_t abi_version;

    /// Canonical module name.
    const char* name;

    /// Module implementation version.
    const char* version;

    /// Stable module identifier.
    const char* stable_id;

    /// Short human-readable summary.
    const char* summary;

    /// Human-readable API description.
    const char* api;

    /// Non-zero when the module claims safe unload behavior.
    uint32_t unload_safe;

    /// wl2 version the module requires, for example "0.1.0". May be null. The
    /// host rejects the module when its major version differs from the host or
    /// when the host version is older than this.
    const char* required_wl2_version;

    /// Module dependency array (ABI v3+). May be null when there are no
    /// dependencies. The host reads this only when abi_version >= 3; ABI v2
    /// modules are treated as declaring no dependencies.
    const wl2_module_dependency_info* dependencies;

    /// Number of entries in `dependencies` (ABI v3+).
    uint32_t dependency_count;

    /// Build stamp for this module build (ABI v4+). May be null.
    const char* build;
};

/**
 * @brief QuickJS native module factory exposed across the C ABI boundary.
 * @param context Engine-specific JavaScript context pointer.
 * @param module_name JavaScript module specifier being loaded.
 * @return Engine-specific module definition pointer, or null on failure.
 */
typedef void* (*wl2_quickjs_module_factory_fn)(void* context, const char* module_name);

/**
 * @brief Host services handed to a dynamic module's `wl2_module_register`.
 *
 * The module uses these callbacks to attach its factories to the host. The
 * struct and the pointed-to data are owned by the host and valid only for the
 * duration of the `wl2_module_register` call.
 */
struct wl2_module_host {
    /// Host ABI version. Modules may compare against their own ABI version.
    uint32_t abi_version;

    /// Opaque host context passed back to the callbacks below.
    void* host;

    /// Host wl2 version string, for diagnostics.
    const char* wl2_version;

    /// Register a QuickJS native module factory with the host.
    void (*register_quickjs_module)(void* host, const char* name, wl2_quickjs_module_factory_fn factory);
};

/// Signature of a dynamic module's metadata entry point.
typedef int (*wl2_module_get_info_fn)(wl2_module_info* out);

/// Signature of a dynamic module's registration entry point. Returns 0 on
/// success and non-zero on failure.
typedef int (*wl2_module_register_fn)(const wl2_module_host* host);
}
