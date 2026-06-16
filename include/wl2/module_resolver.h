#pragma once

/**
 * @file module_resolver.h
 * @brief Native module source metadata parsing and dependency graph resolution.
 */

#include <filesystem>
#include <string>
#include <vector>

#include "wl2/errors.h"
#include "wl2/manifest.h"
#include "wl2/module.h"

namespace wl2 {

/// Source-tree metadata from `wl2.module.source.yml`.
struct ModuleSourceMetadata {
    std::filesystem::path path;     ///< Path the metadata was loaded from.
    std::filesystem::path baseDir;  ///< Directory the metadata is relative to.
    std::string schema = "wl2.module-source.v1";  ///< Declared schema identifier.
    std::string provides;           ///< Canonical module name provided (e.g. `wl2:asio`).
    std::string version;            ///< Module version.
    std::string stableId;           ///< Stable module identity.
    std::filesystem::path modulePath = ".";  ///< Module root relative to baseDir.
    std::vector<ModuleDependencyRequirement> dependencies;  ///< Native module dependencies.
    std::vector<ModuleDependency> sourceDependencies;       ///< Git-pinned source dependencies.
};

/// Load and validate a module source metadata file.
Result<ModuleSourceMetadata> loadModuleSourceMetadata(const std::filesystem::path& path);

/// A module provider available to the resolver.
struct ModuleProvider {
    /// Where a provider was discovered, in increasing priority order.
    enum class Source {
        Explicit,  ///< Explicitly supplied (e.g. `--load-module`).
        Project,   ///< Project-local module directory.
        Local,     ///< Local install scope.
        User,      ///< Per-user install scope.
        System,    ///< System install scope.
        Builtin,   ///< Built-in static module.
    };

    ModuleInfo info;                   ///< Module metadata.
    Source source = Source::Builtin;   ///< How the provider was discovered.
    std::filesystem::path path;        ///< Provider location, if file-backed.
};

/// A requested dependency root.
struct ModuleResolutionRoot {
    std::string name;  ///< Module name to resolve.
    ModuleDependencyKind kind = ModuleDependencyKind::Required;  ///< Required or optional.
};

/// Resolver input.
struct ModuleResolutionRequest {
    std::vector<ModuleResolutionRoot> roots;     ///< Top-level modules to resolve.
    std::vector<ModuleProvider> providers;       ///< Available providers to draw from.
};

/// Resolver diagnostic.
struct ModuleResolutionDiagnostic {
    std::string code;                 ///< Stable diagnostic code.
    std::string message;              ///< Human-readable description.
    std::vector<std::string> chain;   ///< Dependency chain leading to the issue.
};

/// A selected module provider.
struct ResolvedModule {
    ModuleProvider provider;   ///< The chosen provider.
    bool optional = false;     ///< True when reached only through optional edges.
};

/// Resolver output.
struct ModuleResolutionPlan {
    std::vector<ResolvedModule> loadOrder;                    ///< Modules in dependency order.
    std::vector<ModuleResolutionDiagnostic> diagnostics;      ///< Warnings and errors.
};

/// Human-readable provider source name.
std::string moduleProviderSourceName(ModuleProvider::Source source);

/// Return true when @p version satisfies @p range.
Result<bool> moduleVersionSatisfies(const std::string& version, const std::string& range);

/// Resolve a module dependency graph from available providers.
Result<ModuleResolutionPlan> resolveModuleGraph(const ModuleResolutionRequest& request);

} // namespace wl2
