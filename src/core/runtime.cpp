#include "wl2/runtime.h"

#include <cstdlib>
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

std::string endpoint_text(std::string_view host, uint16_t port) {
    return std::string(host) + ":" + std::to_string(port);
}

bool prefix_allowed(const std::vector<std::string>& allowList, std::string_view value) {
    for (const auto& entry : allowList) {
        if (!entry.empty() && value.rfind(entry, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool path_contained_by(const std::filesystem::path& target, const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    fs::path relative = target.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

std::filesystem::path canonical_permission_path(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path target = path;
    if (!target.is_absolute()) {
        target = fs::current_path(ec) / target;
        if (ec) {
            return path.lexically_normal();
        }
    }
    fs::path canonical = fs::weakly_canonical(target, ec);
    return ec ? target.lexically_normal() : canonical;
}

bool prompt_yes_no(const std::vector<std::string>& requested) {
    std::cerr << "Script requests host permissions:\n";
    for (const auto& item : requested) {
        std::cerr << "  - " << item << '\n';
    }
    std::cerr << "Allow these and subsequent matching host permission requests for this run? [y/N] ";
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
    if ((options_.allowNetwork && endpoint_allowed(options_.networkAllowList, host, port)) ||
        endpoint_allowed(dynamicNetworkAllowList_, host, port) ||
        endpoint_allowed(interactiveNetworkAllowList_, host, port)) {
        return {};
    }
    const std::string endpoint = endpoint_text(host, port);
    if (options_.interactivePermissions && prompt_yes_no({"network connection to " + endpoint})) {
        interactiveNetworkAllowList_.push_back(endpoint);
        return {};
    }
    return Error("network_connect_denied",
        "Network connection to " + endpoint + " is not permitted by policy");
}

Result<void> Runtime::authorizeNetworkListen(std::string_view host, uint16_t port) const {
    if ((options_.allowListening && endpoint_allowed(options_.listenAllowList, host, port)) ||
        endpoint_allowed(dynamicListenAllowList_, host, port) ||
        endpoint_allowed(interactiveListenAllowList_, host, port)) {
        return {};
    }
    const std::string endpoint = endpoint_text(host, port);
    if (options_.interactivePermissions && prompt_yes_no({"network listener on " + endpoint})) {
        interactiveListenAllowList_.push_back(endpoint);
        return {};
    }
    return Error("network_listen_denied",
        "Listening on " + endpoint + " is not permitted by policy");
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
    if (options_.allowUi || dynamicUiAllowed_ || interactivePermissionApproved_) {
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
    if (options_.allowGraphics || dynamicGraphicsAllowed_ || interactivePermissionApproved_) {
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
        prefix_allowed(dynamicSharedMemoryAllowList_, name) ||
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

    auto root_contains_target = [&](const fs::path& root) {
        std::error_code rootEc;
        fs::path canonicalRoot = fs::weakly_canonical(root, rootEc);
        if (rootEc) {
            canonicalRoot = root.lexically_normal();
        }
        return path_contained_by(canonicalTarget, canonicalRoot);
    };

    if (options_.allowFilesystemReads) {
        for (const auto& root : options_.filesystemReadRoots) {
            if (root_contains_target(root)) {
                return canonicalTarget;
            }
        }
    }

    for (const auto& root : interactiveFilesystemReadRoots_) {
        if (root_contains_target(root)) {
            return canonicalTarget;
        }
    }

    for (const auto& root : dynamicFilesystemReadRoots_) {
        if (root_contains_target(root)) {
            return canonicalTarget;
        }
    }

    if (options_.interactivePermissions) {
        fs::path requestedRoot = canonicalTarget;
        std::error_code statusEc;
        if (!fs::is_directory(canonicalTarget, statusEc)) {
            requestedRoot = canonicalTarget.parent_path();
        }
        if (!requestedRoot.empty()
            && prompt_yes_no({"filesystem read " + canonicalTarget.string()
                + " (grant read access under " + requestedRoot.string() + ")"})) {
            interactiveFilesystemReadRoots_.push_back(requestedRoot);
            return canonicalTarget;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> Runtime::resolveFilesystemWritePath(
    const std::filesystem::path& requested) const {
    namespace fs = std::filesystem;
    std::error_code ec;

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

    auto root_contains_target = [&](const fs::path& root) {
        std::error_code rootEc;
        fs::path canonicalRoot = fs::weakly_canonical(root, rootEc);
        if (rootEc) {
            canonicalRoot = root.lexically_normal();
        }
        return path_contained_by(canonicalTarget, canonicalRoot) || canonicalTarget == canonicalRoot;
    };

    if (options_.allowFilesystemWrites) {
        for (const auto& root : options_.filesystemWriteRoots) {
            if (root_contains_target(root)) {
                return canonicalTarget;
            }
        }
    }

    for (const auto& root : interactiveFilesystemWriteRoots_) {
        if (root_contains_target(root)) {
            return canonicalTarget;
        }
    }

    for (const auto& root : dynamicFilesystemWriteRoots_) {
        if (root_contains_target(root)) {
            return canonicalTarget;
        }
    }

    if (options_.interactivePermissions) {
        fs::path requestedRoot = canonicalTarget;
        std::error_code statusEc;
        if (!fs::is_directory(canonicalTarget, statusEc)) {
            requestedRoot = canonicalTarget.parent_path();
        }
        if (!requestedRoot.empty()
            && prompt_yes_no({"filesystem write " + canonicalTarget.string()
                + " (grant write access under " + requestedRoot.string() + ")"})) {
            interactiveFilesystemWriteRoots_.push_back(requestedRoot);
            return canonicalTarget;
        }
    }

    return std::nullopt;
}

bool Runtime::hasPermissions(const PermissionSet& requested) const {
    if (requested.ui && !options_.allowUi && !dynamicUiAllowed_) {
        return false;
    }
    if (requested.graphics && !options_.allowGraphics && !dynamicGraphicsAllowed_) {
        return false;
    }
    for (const auto& item : requested.network) {
        auto colon = item.rfind(':');
        if (colon == std::string::npos) {
            if (!((options_.allowNetwork && endpoint_allowed(options_.networkAllowList, item, 0)) ||
                  endpoint_allowed(dynamicNetworkAllowList_, item, 0))) {
                return false;
            }
            continue;
        }
        const auto host = item.substr(0, colon);
        const auto portText = item.substr(colon + 1);
        const uint16_t port = portText == "*" ? 0 : static_cast<uint16_t>(std::max(0, std::atoi(portText.c_str())));
        if (!((options_.allowNetwork && endpoint_allowed(options_.networkAllowList, host, port)) ||
              endpoint_allowed(dynamicNetworkAllowList_, host, port))) {
            return false;
        }
    }
    for (const auto& item : requested.listen) {
        auto colon = item.rfind(':');
        if (colon == std::string::npos) {
            if (!((options_.allowListening && endpoint_allowed(options_.listenAllowList, item, 0)) ||
                  endpoint_allowed(dynamicListenAllowList_, item, 0))) {
                return false;
            }
            continue;
        }
        const auto host = item.substr(0, colon);
        const auto portText = item.substr(colon + 1);
        const uint16_t port = portText == "*" ? 0 : static_cast<uint16_t>(std::max(0, std::atoi(portText.c_str())));
        if (!((options_.allowListening && endpoint_allowed(options_.listenAllowList, host, port)) ||
              endpoint_allowed(dynamicListenAllowList_, host, port))) {
            return false;
        }
    }
    for (const auto& item : requested.sharedMemory) {
        if (!((options_.allowSharedMemory && prefix_allowed(options_.sharedMemoryAllowList, item)) ||
              prefix_allowed(dynamicSharedMemoryAllowList_, item))) {
            return false;
        }
    }
    for (const auto& item : requested.filesystemRead) {
        bool ok = false;
        const auto canonicalItem = canonical_permission_path(item);
        for (const auto& root : options_.filesystemReadRoots) {
            const auto canonicalRoot = canonical_permission_path(root);
            if (options_.allowFilesystemReads &&
                (path_contained_by(canonicalItem, canonicalRoot) || canonicalItem == canonicalRoot)) {
                ok = true;
                break;
            }
        }
        for (const auto& root : dynamicFilesystemReadRoots_) {
            const auto canonicalRoot = canonical_permission_path(root);
            if (path_contained_by(canonicalItem, canonicalRoot) || canonicalItem == canonicalRoot) {
                ok = true;
                break;
            }
        }
        if (!ok) return false;
    }
    for (const auto& item : requested.filesystemWrite) {
        bool ok = false;
        const auto canonicalItem = canonical_permission_path(item);
        for (const auto& root : options_.filesystemWriteRoots) {
            const auto canonicalRoot = canonical_permission_path(root);
            if (options_.allowFilesystemWrites &&
                (path_contained_by(canonicalItem, canonicalRoot) || canonicalItem == canonicalRoot)) {
                ok = true;
                break;
            }
        }
        for (const auto& root : dynamicFilesystemWriteRoots_) {
            const auto canonicalRoot = canonical_permission_path(root);
            if (path_contained_by(canonicalItem, canonicalRoot) || canonicalItem == canonicalRoot) {
                ok = true;
                break;
            }
        }
        if (!ok) return false;
    }
    return true;
}

Result<PermissionSet> Runtime::requestPermissions(const PermissionSet& requested) const {
    PermissionSet granted;

    auto endpoint_inside = [](const std::vector<std::string>& envelope, const std::string& item) {
        auto colon = item.rfind(':');
        if (colon == std::string::npos) {
            for (const auto& entry : envelope) {
                if (entry == item || entry == "*" || entry == item + ":*") return true;
            }
            return false;
        }
        const auto host = item.substr(0, colon);
        const auto portText = item.substr(colon + 1);
        const uint16_t port = portText == "*" ? 0 : static_cast<uint16_t>(std::max(0, std::atoi(portText.c_str())));
        for (const auto& entry : envelope) {
            if (portText == "*") {
                if (entry == "*" || entry == host || entry == host + ":*" || entry == "*:*") return true;
            } else if (endpoint_matches(entry, host, port)) {
                return true;
            }
        }
        return false;
    };
    auto path_inside = [](const std::vector<std::filesystem::path>& envelope, const std::filesystem::path& item) {
        const auto normalized = canonical_permission_path(item);
        for (const auto& root : envelope) {
            const auto normalizedRoot = canonical_permission_path(root);
            if (path_contained_by(normalized, normalizedRoot) || normalized == normalizedRoot) {
                return true;
            }
        }
        return false;
    };

    auto deny = [](const std::string& item) {
        return Error("permission_denied", "Requested permission is outside the approved policy: " + item);
    };

    if (requested.ui) {
        if (options_.allowUi || dynamicUiAllowed_ ||
            (options_.declaredPermissionsApproved && options_.declaredPermissions.ui)) {
            dynamicUiAllowed_ = true;
            granted.ui = true;
        } else {
            return deny("ui");
        }
    }
    if (requested.graphics) {
        if (options_.allowGraphics || dynamicGraphicsAllowed_ ||
            (options_.declaredPermissionsApproved && options_.declaredPermissions.graphics)) {
            dynamicGraphicsAllowed_ = true;
            granted.graphics = true;
        } else {
            return deny("graphics");
        }
    }
    for (const auto& item : requested.network) {
        if ((options_.allowNetwork && endpoint_inside(options_.networkAllowList, item)) ||
            endpoint_inside(dynamicNetworkAllowList_, item) ||
            (options_.declaredPermissionsApproved && endpoint_inside(options_.declaredPermissions.network, item))) {
            dynamicNetworkAllowList_.push_back(item);
            granted.network.push_back(item);
        } else {
            return deny("network " + item);
        }
    }
    for (const auto& item : requested.listen) {
        if ((options_.allowListening && endpoint_inside(options_.listenAllowList, item)) ||
            endpoint_inside(dynamicListenAllowList_, item) ||
            (options_.declaredPermissionsApproved && endpoint_inside(options_.declaredPermissions.listen, item))) {
            dynamicListenAllowList_.push_back(item);
            granted.listen.push_back(item);
        } else {
            return deny("listen " + item);
        }
    }
    for (const auto& item : requested.sharedMemory) {
        if ((options_.allowSharedMemory && prefix_allowed(options_.sharedMemoryAllowList, item)) ||
            prefix_allowed(dynamicSharedMemoryAllowList_, item) ||
            (options_.declaredPermissionsApproved && prefix_allowed(options_.declaredPermissions.sharedMemory, item))) {
            dynamicSharedMemoryAllowList_.push_back(item);
            granted.sharedMemory.push_back(item);
        } else {
            return deny("sharedMemory " + item);
        }
    }
    for (const auto& item : requested.filesystemRead) {
        if ((options_.allowFilesystemReads && path_inside(options_.filesystemReadRoots, item)) ||
            path_inside(dynamicFilesystemReadRoots_, item) ||
            (options_.declaredPermissionsApproved && path_inside(options_.declaredPermissions.filesystemRead, item))) {
            const auto canonicalItem = canonical_permission_path(item);
            dynamicFilesystemReadRoots_.push_back(canonicalItem);
            granted.filesystemRead.push_back(canonicalItem);
        } else {
            return deny("filesystemRead " + item.string());
        }
    }
    for (const auto& item : requested.filesystemWrite) {
        if ((options_.allowFilesystemWrites && path_inside(options_.filesystemWriteRoots, item)) ||
            path_inside(dynamicFilesystemWriteRoots_, item) ||
            (options_.declaredPermissionsApproved && path_inside(options_.declaredPermissions.filesystemWrite, item))) {
            const auto canonicalItem = canonical_permission_path(item);
            dynamicFilesystemWriteRoots_.push_back(canonicalItem);
            granted.filesystemWrite.push_back(canonicalItem);
        } else {
            return deny("filesystemWrite " + item.string());
        }
    }

    return granted;
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
