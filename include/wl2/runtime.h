#pragma once

/**
 * @file runtime.h
 * @brief Top-level runtime object for embedding and running scripts.
 */

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wl2/async_host.h"
#include "wl2/errors.h"
#include "wl2/js_engine.h"
#include "wl2/module.h"
#include "wl2/resources.h"
#include "wl2/thread_tree.h"

namespace wl2 {

/**
 * @brief A dynamic module to load by explicit library path before scripts run.
 */
struct DynamicModuleSpec {
    /// Path to the module shared library (`.so`/`.dylib`).
    std::filesystem::path path;

    /// Allow this module to shadow an already-registered name (for example a
    /// built-in). When false, a name collision fails the load.
    bool allowShadow = false;
};

/**
 * @brief Runtime construction options.
 *
 * Options are consumed when Runtime is constructed. Static modules should be
 * registered here so they are initialized consistently before scripts run.
 *
 * @code{.cpp}
 * wl2::RuntimeOptions options;
 * options.allowFilesystem = false;
 * options.staticModules.push_back(wl2_curl_register_module);
 *
 * wl2::Runtime runtime{std::move(options)};
 * @endcode
 */
struct RuntimeOptions {
    /// Allow loading scripts from the host filesystem.
    bool allowFilesystem = true;

    /// Future development override root for resource lookup.
    std::filesystem::path developmentResourceRoot;

    /// Host directories mapped into the logical `wl2:/` resource namespace for
    /// development-time resource loading.
    std::vector<ResourceDirectoryMount> resourceDirectoryMounts;

    /// Print resource lookup hits and misses to stderr.
    bool traceResourceLookups = false;

    /// Statically linked native module registration functions.
    std::vector<StaticModuleRegister> staticModules;

    /// Module names that must be registered before the entry script runs.
    /// Initialization fails with `module_required_missing` when any name is not
    /// found among the initialized modules. Sourced from manifest
    /// `modules.require`.
    std::vector<std::string> requiredModules;

    /// Module names that are used when present but never block startup. Sourced
    /// from manifest `modules.optional`.
    std::vector<std::string> optionalModules;

    /// Dynamic modules to load by explicit library path during initialization,
    /// before the required-module check and before any script runs.
    std::vector<DynamicModuleSpec> dynamicModules;

    /// Arguments made available to scripts through `wl2.runtime.argv`.
    std::vector<std::string> scriptArgs;

    /// Allow scripts to read environment variables through `wl2.runtime.env()`.
    /// Disabled by default. Even when enabled, only names in
    /// environmentAllowList are readable.
    bool allowEnvironment = false;

    /// Variable-name allow-list consulted when allowEnvironment is true.
    std::vector<std::string> environmentAllowList;

    /// Allow scripts to read the host filesystem through the `wl2:fs` module.
    /// Disabled by default. Even when enabled, reads are confined to
    /// filesystemReadRoots. This is independent of allowFilesystem, which only
    /// governs loading script source from the filesystem.
    bool allowFilesystemReads = false;

    /// Read-only roots that `wl2:fs` may access when allowFilesystemReads is
    /// true. Paths outside every root are denied. An empty list denies all
    /// `wl2:fs` reads even when allowFilesystemReads is true.
    std::vector<std::filesystem::path> filesystemReadRoots;

    /// Allow modules to open outbound network connections. Disabled by default.
    /// Even when enabled, only endpoints matching networkAllowList are permitted.
    bool allowNetwork = false;

    /// Endpoints a module may connect to when allowNetwork is true. Entries are
    /// `host:port`, `host` (any port), `host:*`, `*:port`, or `*` (any). An empty
    /// list denies all connections even when allowNetwork is true.
    std::vector<std::string> networkAllowList;

    /// Allow modules to listen for inbound connections. Disabled by default.
    /// Even when enabled, only endpoints matching listenAllowList are permitted.
    bool allowListening = false;

    /// Endpoints a module may listen on when allowListening is true. Same entry
    /// grammar as networkAllowList. An empty list denies all listening.
    std::vector<std::string> listenAllowList;
};

/**
 * @brief Owns runtime services, modules, resources, and JavaScript execution.
 *
 * Runtime is the main embedding object. It owns resource storage, the module
 * loader, the thread tree, and the configured JavaScript engine. A Runtime is
 * not copyable because those services own process and engine state.
 *
 * @code{.cpp}
 * wl2::RuntimeOptions options;
 * options.staticModules.push_back(wl2_curl_register_module);
 *
 * wl2::Runtime runtime{std::move(options)};
 * auto result = runtime.runModule("scripts/main.js");
 * if (!result) {
 *     std::cerr << result.error().code() << ": "
 *               << result.error().message() << "\n";
 *     return 1;
 * }
 * return result.value();
 * @endcode
 */
class Runtime {
public:
    /**
     * @brief Create a runtime with the supplied options.
     * @param options Runtime configuration and static module registrations.
     */
    explicit Runtime(RuntimeOptions options = {});

    /// Shut down runtime-owned services.
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /**
     * @brief Embedded and in-memory resources.
     * @return Mutable resource store owned by the runtime.
     */
    ResourceStore& resources() noexcept { return resources_; }

    /**
     * @brief Embedded and in-memory resources.
     * @return Const resource store owned by the runtime.
     */
    const ResourceStore& resources() const noexcept { return resources_; }

    /**
     * @brief Native module registry.
     * @return Mutable module loader owned by the runtime.
     */
    ModuleLoader& modules() noexcept { return modules_; }

    /**
     * @brief Thread tree and mailboxes.
     * @return Mutable thread tree owned by the runtime.
     */
    ThreadTree& threadTree() noexcept { return threadTree_; }

    /**
     * @brief Thread tree and mailboxes.
     * @return Const thread tree owned by the runtime.
     */
    const ThreadTree& threadTree() const noexcept { return threadTree_; }

    /**
     * @brief Arguments exposed to scripts through `wl2.runtime.argv`.
     * @return Script arguments captured from the host at construction.
     */
    const std::vector<std::string>& scriptArgs() const noexcept { return options_.scriptArgs; }

    /**
     * @brief Whether a named environment variable may be read by scripts.
     * @param name Environment variable name requested through `wl2.runtime.env()`.
     * @return True only when environment access is enabled and the name is allow-listed.
     */
    bool environmentAccessAllowed(std::string_view name) const noexcept;

    /**
     * @brief Authorize and normalize a `wl2:fs` read path against policy.
     *
     * Resolves @p requested (symlinks and `..` included) and confirms it is
     * contained by one of the configured read roots. This is the single policy
     * gate the `wl2:fs` module consults; it never bypasses RuntimeOptions.
     *
     * @param requested Filesystem path requested by a script.
     * @return The resolved absolute path when permitted, or std::nullopt when
     * filesystem reads are disabled or the path escapes every read root.
     */
    std::optional<std::filesystem::path> resolveFilesystemReadPath(
        const std::filesystem::path& requested) const;

    /**
     * @brief Authorize an outbound network connection against policy.
     *
     * Network access is denied by default. It is permitted only when
     * RuntimeOptions::allowNetwork is set and @p host / @p port match an entry in
     * RuntimeOptions::networkAllowList. This is the single policy gate modules
     * such as `wl2:asio` consult before connecting.
     *
     * @param host Destination host name or address.
     * @param port Destination port.
     * @return Success when permitted, or an Error with code
     * `network_connect_denied`.
     */
    Result<void> authorizeNetworkConnect(std::string_view host, uint16_t port) const;

    /**
     * @brief Authorize listening for inbound connections against policy.
     *
     * Listening is denied by default. It is permitted only when
     * RuntimeOptions::allowListening is set and @p host / @p port match an entry
     * in RuntimeOptions::listenAllowList.
     *
     * @param host Local bind host name or address.
     * @param port Local bind port.
     * @return Success when permitted, or an Error with code
     * `network_listen_denied`.
     */
    Result<void> authorizeNetworkListen(std::string_view host, uint16_t port) const;

    /**
     * @brief Shared host support for native asynchronous module work.
     * @return Mutable async host owned by the runtime.
     */
    AsyncHost& async() noexcept { return async_; }

    /**
     * @brief Register a QuickJS native module factory.
     * @param name JavaScript module specifier handled by the factory.
     * @param factory Factory called by the QuickJS module loader.
     */
    void registerQuickJsModule(std::string name, QuickJsModuleFactory factory);

    /**
     * @brief Find a QuickJS native module factory by module specifier.
     * @param name JavaScript module specifier to resolve.
     * @return Factory pointer when registered, otherwise null.
     */
    QuickJsModuleFactory findQuickJsModule(std::string_view name) const;

    /**
     * @brief Initialize modules and runtime services once.
     * @return Successful Result on initialization, or an Error.
     */
    Result<void> initialize();

    /**
     * @brief Load and execute one JavaScript module by resource or filesystem path.
     * @param scriptSpecifier `wl2:`, `file:`, or filesystem script specifier.
     * @return Script exit code on success, or an Error on load/execution failure.
     */
    Result<int> runModule(std::string scriptSpecifier);

    /**
     * @brief Load a text resource from `wl2:`, `file:`, or filesystem specifiers.
     * @param specifier Resource or filesystem specifier.
     * @return Resource text on success, or an Error when unavailable.
     */
    Result<std::string> loadTextResource(std::string specifier) const;

private:
    RuntimeOptions options_;
    ResourceStore resources_;
    ModuleLoader modules_;
    std::vector<QuickJsNativeModule> quickJsModules_;
    AsyncHost async_;
    std::unique_ptr<JsEngine> jsEngine_;
    // Declared after the services script threads use (resources, modules, the
    // engine factory) so ~ThreadTree joins those threads while their
    // dependencies are still alive.
    ThreadTree threadTree_;
    bool initialized_ = false;
};

} // namespace wl2
