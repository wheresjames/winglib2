#include "wl2/runtime.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace wl2 {

namespace {

std::string strip_shebang(std::string source) {
    if (source.rfind("#!", 0) != 0) {
        return source;
    }
    source[0] = '/';
    source[1] = '/';
    return source;
}

// Match a "host:port" endpoint against one allow-list entry. Supported entry
// forms: "host:port", "host" (any port), "host:*", "*:port", "*" (any).
bool endpoint_matches(const std::string& entry, std::string_view host, uint16_t port) {
    if (entry == "*") {
        return true;
    }
    const std::string portText = std::to_string(port);
    auto colon = entry.rfind(':');
    if (colon == std::string::npos) {
        // Host only: any port on that host.
        return entry == host;
    }
    std::string entryHost = entry.substr(0, colon);
    std::string entryPort = entry.substr(colon + 1);
    const bool hostOk = entryHost == "*" || entryHost == host;
    const bool portOk = entryPort == "*" || entryPort == portText;
    return hostOk && portOk;
}

bool endpoint_allowed(const std::vector<std::string>& allowList, std::string_view host, uint16_t port) {
    for (const auto& entry : allowList) {
        if (endpoint_matches(entry, host, port)) {
            return true;
        }
    }
    return false;
}

bool prefix_allowed(const std::vector<std::string>& allowList, std::string_view value) {
    for (const auto& entry : allowList) {
        if (!entry.empty() && value.rfind(entry, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool prompt_yes_no(const std::vector<std::string>& requested) {
    std::cerr << "Script requests host permissions:\n";
    for (const auto& item : requested) {
        std::cerr << "  - " << item << '\n';
    }
    std::cerr << "Allow these and subsequent UI, graphics, and shared-memory requests for this run? [y/N] ";
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

} // namespace

Runtime::Runtime(RuntimeOptions options)
    : options_(std::move(options)), jsEngine_(createConfiguredJsEngine()) {
    for (auto& module : options_.staticModules) {
        modules_.registerStaticModule(std::move(module));
    }
    resources_.setTraceLookups(options_.traceResourceLookups);
    for (const auto& mount : options_.resourceDirectoryMounts) {
        auto mounted = resources_.mountDirectory(
            mount.root,
            mount.prefix,
            mount.excludePatterns,
            mount.compressedFiles,
            mount.compressedDirectories);
        if (!mounted) {
            // Runtime construction cannot report Result yet; failed mounts are
            // also validated by the wl2 runner before construction.
        }
    }
}

Runtime::~Runtime() {
    // Stop native workers and settle pending async work before runtime-owned
    // services are torn down, so module worker threads never outlive the runtime.
    async_.shutdown();
}

Result<void> Runtime::authorizeNetworkConnect(std::string_view host, uint16_t port) const {
    if (!options_.allowNetwork || !endpoint_allowed(options_.networkAllowList, host, port)) {
        return Error("network_connect_denied",
            "Network connection to " + std::string(host) + ":" + std::to_string(port)
                + " is not permitted by policy");
    }
    return {};
}

Result<void> Runtime::authorizeNetworkListen(std::string_view host, uint16_t port) const {
    if (!options_.allowListening || !endpoint_allowed(options_.listenAllowList, host, port)) {
        return Error("network_listen_denied",
            "Listening on " + std::string(host) + ":" + std::to_string(port)
                + " is not permitted by policy");
    }
    return {};
}

bool Runtime::interactivePermissionAllowed(const std::vector<std::string>& requested) const {
    if (!options_.interactivePermissions) {
        return false;
    }
    if (interactivePermissionPrompted_) {
        return interactivePermissionApproved_;
    }
    interactivePermissionPrompted_ = true;
    interactivePermissionApproved_ = prompt_yes_no(requested);
    return interactivePermissionApproved_;
}

Result<void> Runtime::authorizeUi() const {
    if (options_.allowUi || interactivePermissionApproved_) {
        return {};
    }
    if (interactivePermissionAllowed({"UI window access"})) {
        return {};
    }
    if (!options_.allowUi) {
        return Error("ui_denied", "Opening a window is not permitted by policy");
    }
    return {};
}

Result<void> Runtime::authorizeGraphics() const {
    if (options_.allowGraphics || interactivePermissionApproved_) {
        return {};
    }
    if (interactivePermissionAllowed({"graphics context access"})) {
        return {};
    }
    if (!options_.allowGraphics) {
        return Error("graphics_denied", "Creating a graphics context is not permitted by policy");
    }
    return {};
}

Result<void> Runtime::authorizeSharedMemory(std::string_view name) const {
    if ((options_.allowSharedMemory && prefix_allowed(options_.sharedMemoryAllowList, name)) ||
        interactivePermissionApproved_) {
        return {};
    }
    if (interactivePermissionAllowed({"shared-memory object " + std::string(name)})) {
        return {};
    }
    if (!options_.allowSharedMemory || !prefix_allowed(options_.sharedMemoryAllowList, name)) {
        return Error("shared_memory_denied",
            "Shared-memory object " + std::string(name) + " is not permitted by policy");
    }
    return {};
}

Result<void> Runtime::initialize() {
    if (initialized_) {
        return {};
    }
    modules_.initializeStaticModules(*this);
    // Load explicitly requested dynamic modules before the required-module check
    // so a requirement can be satisfied by a dynamically loaded module.
    for (const auto& spec : options_.dynamicModules) {
        auto loaded = modules_.loadDynamicModule(
            spec.path,
            *this,
            spec.allowShadow ? ModuleShadowPolicy::Allow : ModuleShadowPolicy::Deny);
        if (!loaded) {
            return loaded.error();
        }
    }
    // Enforce required modules before any script runs. Optional modules are
    // intentionally not checked: they are used when present and ignored when
    // absent.
    for (const auto& name : options_.requiredModules) {
        if (!modules_.find(name)) {
            return Error("module_required_missing",
                "Required module is not available: " + name);
        }
    }
    initialized_ = true;
    return {};
}

void Runtime::registerQuickJsModule(std::string name, QuickJsModuleFactory factory) {
    quickJsModules_.push_back(QuickJsNativeModule{std::move(name), factory});
}

bool Runtime::environmentAccessAllowed(std::string_view name) const noexcept {
    if (!options_.allowEnvironment) {
        return false;
    }
    for (const auto& allowed : options_.environmentAllowList) {
        if (allowed == name) {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> Runtime::resolveFilesystemReadPath(
    const std::filesystem::path& requested) const {
    if (!options_.allowFilesystemReads || options_.filesystemReadRoots.empty()) {
        return std::nullopt;
    }

    namespace fs = std::filesystem;
    std::error_code ec;

    // Resolve the request to an absolute path, following symlinks and `..` for
    // the portion that exists. weakly_canonical keeps the trailing component
    // even when the file does not exist, which is needed for exists()/stat().
    fs::path target = requested;
    if (!target.is_absolute()) {
        target = fs::current_path(ec) / target;
        if (ec) {
            return std::nullopt;
        }
    }
    fs::path canonicalTarget = fs::weakly_canonical(target, ec);
    if (ec) {
        canonicalTarget = target.lexically_normal();
    }

    for (const auto& root : options_.filesystemReadRoots) {
        std::error_code rootEc;
        fs::path canonicalRoot = fs::weakly_canonical(root, rootEc);
        if (rootEc) {
            canonicalRoot = root.lexically_normal();
        }
        // Contained when the relative path neither escapes (`..`) nor is empty
        // (unrelated). A target equal to the root yields ".", which is allowed.
        fs::path relative = canonicalTarget.lexically_relative(canonicalRoot);
        if (!relative.empty() && *relative.begin() != "..") {
            return canonicalTarget;
        }
    }
    return std::nullopt;
}

QuickJsModuleFactory Runtime::findQuickJsModule(std::string_view name) const {
    for (const auto& module : quickJsModules_) {
        if (module.name == name) {
            return module.factory;
        }
    }
    return nullptr;
}

Result<std::string> Runtime::loadTextResource(std::string specifier) const {
    if (specifier.rfind("wl2:", 0) == 0) {
        if (auto res = resources_.get(specifier)) {
            return strip_shebang(std::string(reinterpret_cast<const char*>(res->bytes.data()), res->bytes.size()));
        }
        return Error("resource_not_found", "No embedded resource named " + specifier);
    }

    if (specifier.rfind("file:", 0) == 0) {
        specifier = specifier.substr(5);
    }

    if (!options_.allowFilesystem) {
        return Error("filesystem_disabled", "Filesystem script loading is disabled");
    }

    std::ifstream in(specifier, std::ios::binary);
    if (!in) {
        return Error("file_not_found", "Unable to open " + specifier);
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    return strip_shebang(ss.str());
}

Result<int> Runtime::runModule(std::string scriptSpecifier) {
    auto init = initialize();
    if (!init) {
        return init.error();
    }

    auto source = loadTextResource(scriptSpecifier);
    if (!source) {
        return source.error();
    }

    return jsEngine_->runModule(*this, scriptSpecifier, source.value());
}

} // namespace wl2
