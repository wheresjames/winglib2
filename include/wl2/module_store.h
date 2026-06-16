#pragma once

/**
 * @file module_store.h
 * @brief Installed-module scopes, install/uninstall, listing, and resolution.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "wl2/errors.h"
#include "wl2/module.h"

namespace wl2 {

/// Where an installed module lives. Resolution prefers earlier scopes.
enum class ModuleScope {
    Local,   ///< ./.wl2/modules
    User,    ///< platform user data directory
    System,  ///< platform system data directory
};

/// Human-readable scope name ("local", "user", "system").
std::string moduleScopeName(ModuleScope scope);

/// Parse a scope name. Returns nullopt for an unknown value.
std::optional<ModuleScope> parseModuleScope(std::string_view value);

/// Resolved module directory for each scope.
struct ModuleScopePaths {
    std::filesystem::path local;   ///< Project-local module directory.
    std::filesystem::path user;    ///< Per-user module directory.
    std::filesystem::path system;  ///< System-wide module directory.

    /// Return the directory for @p scope.
    const std::filesystem::path& forScope(ModuleScope scope) const;
};

/// Resolve scope module directories. Honors test/environment overrides:
/// `projectRoot` for local (`<projectRoot>/.wl2/modules`), `XDG_DATA_HOME` (or
/// the platform default) for user, and `WL2_SYSTEM_MODULE_DIR` for system.
ModuleScopePaths resolveModuleScopePaths(const std::filesystem::path& projectRoot = std::filesystem::current_path());

/// A module installed into a scope.
struct InstalledModuleRecord {
    std::string name;                    ///< Canonical module name, e.g. "wl2:echo".
    std::string version;                 ///< Module version.
    std::string build;                   ///< Build identifier.
    std::uint32_t abiVersion = 0;        ///< Native module ABI version.
    std::string stableId;                ///< Stable module identity.
    std::filesystem::path libraryPath;   ///< Absolute path to the installed library.
    ModuleScope scope = ModuleScope::Local;  ///< Scope the module is installed in.
    bool shadowed = false;               ///< True when a higher-priority scope also provides this name.
    std::vector<ModuleDependencyRequirement> dependencies; ///< Declared module dependencies (ABI v3+).
    std::string checksum;                ///< SHA-256 of the installed library recorded at install time. Empty for legacy installs.
};

/**
 * @brief Manages modules installed under the local/user/system scopes.
 *
 * Each scope directory holds one subdirectory per module (payload library plus
 * `wl2.module.yml`), an `index.yml`, and a hidden `.cache` directory retained
 * across uninstalls unless explicitly purged.
 */
class ModuleStore {
public:
    /// Construct a store backed by the given scope @p paths.
    explicit ModuleStore(ModuleScopePaths paths);

    /// Install a prebuilt module library into @p scope. The library is validated
    /// through the dynamic module ABI to read its name and metadata.
    Result<InstalledModuleRecord> install(const std::filesystem::path& librarySource, ModuleScope scope);

    /**
     * @brief Remove an installed module.
     * @param name Canonical module name.
     * @param scope Explicit scope, or nullopt to infer it.
     * @param force Allow removing a module referenced by @p referencedNames.
     * @param purgeCache Also delete the retained build cache.
     * @param referencedNames Names referenced by the manifest/lockfile.
     */
    Result<void> uninstall(
        const std::string& name,
        std::optional<ModuleScope> scope,
        bool force,
        bool purgeCache,
        const std::set<std::string>& referencedNames = {});

    /// All installed modules across scopes, in resolution order, with shadowing
    /// marked.
    std::vector<InstalledModuleRecord> list() const;

    /// Resolve a module name to its highest-priority installed record.
    std::optional<InstalledModuleRecord> resolve(const std::string& name) const;

    /// Verify an installed module's library against the checksum recorded at
    /// install time. Returns `module_library_missing` when the library is gone or
    /// `module_checksum_mismatch` when its contents have changed. A record with no
    /// recorded checksum (legacy install) is treated as valid.
    Result<void> verifyInstalled(const InstalledModuleRecord& record) const;

    /// Installed modules in a single scope.
    std::vector<InstalledModuleRecord> scanScope(ModuleScope scope) const;

private:
    ModuleScopePaths paths_;
};

/// Convert a module name to a filesystem-safe directory slug ("wl2:echo" -> "wl2_echo").
std::string moduleSlug(std::string_view name);

} // namespace wl2
