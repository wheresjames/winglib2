#pragma once

/**
 * @file manifest.h
 * @brief wl2 project/resource manifest parsing.
 */

#include <filesystem>
#include <string>
#include <vector>

#include "wl2/errors.h"

namespace wl2 {

/// A set of file and directory glob patterns selecting manifest entries.
struct ResourceManifestPolicy {
    std::vector<std::string> files;        ///< File glob patterns.
    std::vector<std::string> directories;  ///< Directory glob patterns.
};

/// A Git-pinned module dependency declared under `dependencies.modules`.
/// Dependencies must pin an immutable ref: a `tag` (resolved to a commit in the
/// lockfile) or an explicit `commit`. Floating `branch` pins are rejected.
struct ModuleDependency {
    std::string name;    ///< Dependency name (repository name by default).
    std::string git;     ///< Git remote URL.
    std::string tag;     ///< Pinned tag, resolved to a commit in the lockfile.
    std::string commit;  ///< Explicit pinned commit.
    std::string path;    ///< Subpath within the repository, if any.
    /// Optional canonical module name this repository provides (for example
    /// `wl2:asio`) when it differs from the repository `name`. Used to map a
    /// source dependency to the module graph.
    std::string provides;
};

/// A parsed wl2 project/resource manifest.
struct ResourceManifest {
    std::filesystem::path path;     ///< Path the manifest was loaded from.
    std::filesystem::path baseDir;  ///< Directory the manifest is relative to.
    std::string schema;             ///< Declared schema identifier.
    std::filesystem::path root;     ///< Resource root directory (relative to baseDir).
    std::string prefix = "wl2:/app";  ///< Resource specifier prefix for mounted files.
    std::string entry;              ///< Entry script, relative to the resource root.
    ResourceManifestPolicy store;       ///< Patterns stored uncompressed.
    ResourceManifestPolicy compress;    ///< Patterns stored compressed.
    ResourceManifestPolicy autoPolicy;  ///< Patterns selected by automatic policy.
    std::vector<std::string> excludePatterns;  ///< Patterns excluded from packaging.
    std::vector<std::string> requiredModules;  ///< Modules that must be present.
    std::vector<std::string> optionalModules;  ///< Modules used when available.
    std::vector<ModuleDependency> moduleDependencies;  ///< Git-pinned module dependencies.
    std::vector<std::string> testRoots;     ///< Directories searched for tests.
    std::string testPattern = "*.test.js";  ///< Glob matching test files.

    /// Absolute resource root (baseDir / root).
    std::filesystem::path resolvedRoot() const;
    /// Fully-qualified entry resource specifier (prefix + entry).
    std::string entrySpecifier() const;
};

/// Load and parse the manifest at @p path.
Result<ResourceManifest> loadResourceManifest(const std::filesystem::path& path);

} // namespace wl2
