#include "wl2/module.h"

#include "wl2/module_resolver.h"
#include "wl2/runtime.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif

namespace wl2 {

namespace {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

SemVer parse_semver(std::string_view text) {
    SemVer version;
    std::array<int*, 3> fields = {&version.major, &version.minor, &version.patch};
    size_t field = 0;
    int value = 0;
    bool any = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + (ch - '0');
            any = true;
        } else if (ch == '.') {
            if (field < fields.size()) {
                *fields[field] = value;
            }
            ++field;
            value = 0;
            any = false;
            if (field >= fields.size()) {
                break;
            }
        } else {
            // Stop at the first non-numeric, non-dot character (e.g. "-rc1").
            break;
        }
    }
    if (any && field < fields.size()) {
        *fields[field] = value;
    }
    return version;
}

// The host satisfies a module's requirement when it shares the major version and
// is at least as new overall.
bool host_satisfies(const SemVer& required, const SemVer& host) {
    if (host.major != required.major) {
        return false;
    }
    if (host.minor != required.minor) {
        return host.minor > required.minor;
    }
    return host.patch >= required.patch;
}

ModuleInfo from_c_info(const wl2_module_info& in, const std::filesystem::path& path) {
    ModuleInfo info;
    info.abiVersion = in.abi_version;
    info.name = in.name ? in.name : "";
    info.version = in.version ? in.version : "";
    info.build = in.abi_version >= 4 && in.build ? in.build : "";
    info.stableId = in.stable_id ? in.stable_id : "";
    info.summary = in.summary ? in.summary : "";
    info.api = in.api ? in.api : "";
    info.unloadSafe = in.unload_safe != 0;
    info.requiredWL2Version = in.required_wl2_version ? in.required_wl2_version : "";
    info.libraryPath = path;
    // Dependency metadata was added in ABI v3. Older modules are treated as
    // declaring no dependencies and their (absent) array fields are not read.
    if (in.abi_version >= 3 && in.dependencies && in.dependency_count > 0) {
        info.dependencies.reserve(in.dependency_count);
        for (uint32_t i = 0; i < in.dependency_count; ++i) {
            const wl2_module_dependency_info& dep = in.dependencies[i];
            ModuleDependencyRequirement requirement;
            requirement.name = dep.name ? dep.name : "";
            requirement.versionRange = dep.version_range ? dep.version_range : "";
            requirement.stableId = dep.stable_id ? dep.stable_id : "";
            requirement.kind = dep.required != 0 ? ModuleDependencyKind::Required
                                                 : ModuleDependencyKind::Optional;
            info.dependencies.push_back(std::move(requirement));
        }
    }
    return info;
}

Result<void> validate_metadata(const ModuleInfo& info) {
    if (info.name.empty()) {
        return Error("module_invalid_info", "Module did not report a name");
    }
    if (info.abiVersion < ModuleMinAbiVersion || info.abiVersion > ModuleAbiVersion) {
        return Error("module_abi_mismatch",
            "Module ABI version " + std::to_string(info.abiVersion)
                + " is not supported by host (accepts "
                + std::to_string(ModuleMinAbiVersion) + ".." + std::to_string(ModuleAbiVersion)
                + ") for " + info.name);
    }
    for (const auto& dep : info.dependencies) {
        if (dep.name.empty()) {
            return Error("module_dependency_invalid",
                "Module " + info.name + " declares a dependency with no name");
        }
        if (auto ok = moduleVersionSatisfies("0.0.0", dep.versionRange); !ok) {
            return Error("module_dependency_invalid",
                "Module " + info.name + " declares an invalid version range for "
                    + dep.name + ": " + dep.versionRange);
        }
    }
    if (!info.requiredWL2Version.empty()) {
        const SemVer required = parse_semver(info.requiredWL2Version);
        const SemVer host = parse_semver(WL2_VERSION);
        if (!host_satisfies(required, host)) {
            return Error("module_wl2_version_mismatch",
                "Module " + info.name + " requires wl2 " + info.requiredWL2Version
                    + " but host is " + WL2_VERSION);
        }
    }
    return {};
}

#if !defined(_WIN32)
std::string last_dl_error() {
    const char* message = dlerror();
    return message ? message : "unknown dynamic loader error";
}

void host_register_quickjs_module(void* host, const char* name, wl2_quickjs_module_factory_fn factory) {
    auto* runtime = static_cast<Runtime*>(host);
    if (runtime && name && factory) {
        runtime->registerQuickJsModule(name, factory);
    }
}
#endif

} // namespace

void ModuleLoader::registerStaticModule(StaticModuleRegister fn) {
    staticModules_.push_back(std::move(fn));
}

std::vector<ModuleInfo> ModuleLoader::initializeStaticModules(Runtime& runtime) {
    modules_.clear();
    for (auto& fn : staticModules_) {
        auto info = fn(runtime);
        if (info.abiVersion >= ModuleMinAbiVersion && info.abiVersion <= ModuleAbiVersion) {
            modules_.push_back(std::move(info));
        }
    }
    return modules_;
}

const ModuleInfo* ModuleLoader::find(std::string_view name) const noexcept {
    for (const auto& module : modules_) {
        if (module.name == name) {
            return &module;
        }
    }
    return nullptr;
}

Result<ModuleInfo> ModuleLoader::inspectDynamicModule(const std::filesystem::path& path) {
#if defined(_WIN32)
    (void)path;
    return Error("module_platform_unsupported",
        "Dynamic module loading is not supported on this platform");
#else
    if (!std::filesystem::exists(path)) {
        return Error("module_library_not_found", "Module library not found: " + path.string());
    }
    void* handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        return Error("module_load_failed", "Failed to load module " + path.string() + ": " + last_dl_error());
    }
    auto getInfo = reinterpret_cast<wl2_module_get_info_fn>(dlsym(handle, "wl2_module_get_info"));
    if (!getInfo) {
        dlclose(handle);
        return Error("module_missing_get_info", "Module does not export wl2_module_get_info: " + path.string());
    }
    wl2_module_info raw{};
    if (getInfo(&raw) != 0) {
        dlclose(handle);
        return Error("module_info_failed", "wl2_module_get_info failed for " + path.string());
    }
    ModuleInfo info = from_c_info(raw, path);
    // Copy out of the library before closing it; the strings point into it.
    dlclose(handle);
    if (auto ok = validate_metadata(info); !ok) {
        return ok.error();
    }
    return info;
#endif
}

Result<ModuleInfo> ModuleLoader::loadDynamicModule(
    const std::filesystem::path& path,
    Runtime& runtime,
    ModuleShadowPolicy policy) {
#if defined(_WIN32)
    (void)path;
    (void)runtime;
    (void)policy;
    return Error("module_platform_unsupported",
        "Dynamic module loading is not supported on this platform");
#else
    if (!std::filesystem::exists(path)) {
        return Error("module_library_not_found", "Module library not found: " + path.string());
    }
    void* handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        return Error("module_load_failed", "Failed to load module " + path.string() + ": " + last_dl_error());
    }

    auto getInfo = reinterpret_cast<wl2_module_get_info_fn>(dlsym(handle, "wl2_module_get_info"));
    if (!getInfo) {
        dlclose(handle);
        return Error("module_missing_get_info", "Module does not export wl2_module_get_info: " + path.string());
    }
    wl2_module_info raw{};
    if (getInfo(&raw) != 0) {
        dlclose(handle);
        return Error("module_info_failed", "wl2_module_get_info failed for " + path.string());
    }
    ModuleInfo info = from_c_info(raw, path);
    if (auto ok = validate_metadata(info); !ok) {
        dlclose(handle);
        return ok.error();
    }

    if (find(info.name) && policy != ModuleShadowPolicy::Allow) {
        dlclose(handle);
        return Error("module_duplicate_name",
            "Module name already registered: " + info.name
                + " (loading " + path.string() + ")");
    }

    auto registerModule = reinterpret_cast<wl2_module_register_fn>(dlsym(handle, "wl2_module_register"));
    if (!registerModule) {
        dlclose(handle);
        return Error("module_missing_register", "Module does not export wl2_module_register: " + path.string());
    }

    wl2_module_host host{};
    host.abi_version = ModuleAbiVersion;
    host.host = &runtime;
    host.wl2_version = WL2_VERSION;
    host.register_quickjs_module = &host_register_quickjs_module;
    if (registerModule(&host) != 0) {
        dlclose(handle);
        return Error("module_register_failed", "wl2_module_register failed for " + path.string());
    }

    // Retain the handle for the process lifetime; modules are never unloaded.
    dynamicHandles_.push_back(handle);

    // When shadowing is allowed, replace any prior registration of this name.
    modules_.erase(
        std::remove_if(modules_.begin(), modules_.end(),
            [&](const ModuleInfo& existing) { return existing.name == info.name; }),
        modules_.end());
    modules_.push_back(info);
    return info;
#endif
}

} // namespace wl2
