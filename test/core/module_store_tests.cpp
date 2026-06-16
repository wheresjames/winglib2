#include "wl2/module_store.h"

#include "wl2/module_resolver.h"
#include "wl2/runtime.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

int fail(const std::string& message) {
    std::cerr << "module store test failed: " << message << '\n';
    return 1;
}

fs::path make_temp_root() {
    auto root = fs::temp_directory_path() / fs::path("wl2-store-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

wl2::ModuleScopePaths scope_paths(const fs::path& root) {
    wl2::ModuleScopePaths paths;
    paths.local = root / "local" / "modules";
    paths.user = root / "user" / "modules";
    paths.system = root / "system" / "modules";
    return paths;
}

int run() {
    const fs::path root = make_temp_root();
    const fs::path source = WL2_DYN_GOOD;       // a valid module library fixture
    const std::string name = "wl2:dyn_good";
    const std::string slug = "wl2_dyn_good";

    // Local install writes the expected layout and index.
    {
        wl2::ModuleStore store(scope_paths(root));
        auto installed = store.install(source, wl2::ModuleScope::Local);
        if (!installed) {
            return fail("local install failed: " + installed.error().code());
        }
        if (installed.value().name != name) {
            return fail("unexpected installed name: " + installed.value().name);
        }
        if (installed.value().build.empty()) {
            return fail("installed module did not record a build stamp");
        }
        const auto moduleDir = scope_paths(root).local / slug;
        if (!fs::is_regular_file(moduleDir / "wl2.module.yml")) {
            return fail("wl2.module.yml was not written");
        }
        if (!fs::is_regular_file(installed.value().libraryPath)) {
            return fail("installed library missing");
        }
        if (!fs::is_regular_file(scope_paths(root).local / "index.yml")) {
            return fail("index.yml was not written");
        }
        if (!fs::is_directory(scope_paths(root).local / ".cache" / slug)) {
            return fail("build cache was not written");
        }
        auto resolved = store.resolve(name);
        if (!resolved || resolved->scope != wl2::ModuleScope::Local) {
            return fail("resolve did not find the local module");
        }
        if (resolved->build != installed.value().build) {
            return fail("scanned module did not preserve the build stamp");
        }
    }

    // User-scope install can target a separate (here temporary) data directory.
    {
        wl2::ModuleStore store(scope_paths(root));
        auto installed = store.install(source, wl2::ModuleScope::User);
        if (!installed || installed.value().scope != wl2::ModuleScope::User) {
            return fail("user install failed");
        }
        if (!fs::is_regular_file(scope_paths(root).user / slug / "wl2.module.yml")) {
            return fail("user wl2.module.yml missing");
        }
    }

    // Resolution order: local shadows user shadows system.
    {
        wl2::ModuleStore store(scope_paths(root));
        store.install(source, wl2::ModuleScope::System);
        // Local, user, and system all have it now.
        auto resolved = store.resolve(name);
        if (!resolved || resolved->scope != wl2::ModuleScope::Local) {
            return fail("resolution order should prefer local");
        }
        // list() marks shadowing for the lower-priority copies.
        int shadowed = 0;
        for (const auto& record : store.list()) {
            if (record.name == name && record.shadowed) {
                ++shadowed;
            }
        }
        if (shadowed != 2) {
            return fail("expected user and system copies to be shadowed");
        }
    }

    // Uninstall without --scope is ambiguous when present in multiple scopes.
    {
        wl2::ModuleStore store(scope_paths(root));
        auto ambiguous = store.uninstall(name, std::nullopt, false, false);
        if (ambiguous || ambiguous.error().code() != "module_scope_ambiguous") {
            return fail("ambiguous uninstall should require a scope");
        }
    }

    // Uninstall refuses a referenced module unless forced.
    {
        wl2::ModuleStore store(scope_paths(root));
        auto referenced = store.uninstall(name, wl2::ModuleScope::System, false, false, {name});
        if (referenced || referenced.error().code() != "module_referenced") {
            return fail("referenced module should not be removed without --force");
        }
        auto forced = store.uninstall(name, wl2::ModuleScope::System, true, false, {name});
        if (!forced) {
            return fail("forced uninstall of referenced module should succeed");
        }
        if (fs::exists(scope_paths(root).system / slug)) {
            return fail("system payload should be removed");
        }
    }

    // Cache is preserved by default and removed with --purge-cache.
    {
        wl2::ModuleStore store(scope_paths(root));
        // Remove the local copy but keep its cache.
        if (auto rc = store.uninstall(name, wl2::ModuleScope::Local, false, false); !rc) {
            return fail("local uninstall failed: " + rc.error().code());
        }
        if (!fs::is_directory(scope_paths(root).local / ".cache" / slug)) {
            return fail("cache should be preserved by default");
        }
        if (fs::exists(scope_paths(root).local / slug)) {
            return fail("local payload should be removed");
        }
        // After removing local, resolution falls back to user.
        auto resolved = store.resolve(name);
        if (!resolved || resolved->scope != wl2::ModuleScope::User) {
            return fail("resolution should fall back to user");
        }
        // Purge the user copy and its cache.
        if (auto rc = store.uninstall(name, wl2::ModuleScope::User, false, true); !rc) {
            return fail("user purge uninstall failed");
        }
        if (fs::exists(scope_paths(root).user / ".cache" / slug)) {
            return fail("--purge-cache should remove the cache");
        }
    }

    // Uninstalling something that is not installed fails clearly.
    {
        wl2::ModuleStore store(scope_paths(root));
        auto missing = store.uninstall("wl2:not_installed", std::nullopt, false, false);
        if (missing || missing.error().code() != "module_not_installed") {
            return fail("uninstall of missing module should fail");
        }
    }

    fs::remove_all(root);
    std::cout << "module_store ok\n";
    return 0;
}

// Installing a current-ABI module records its dependency metadata as wl2.module.v2,
// and scanning reads it back. The scope index also carries the dependency data.
int dependency_metadata_roundtrips() {
    const fs::path root = make_temp_root();
    wl2::ModuleStore store(scope_paths(root));
    auto installed = store.install(WL2_DYN_DEPS, wl2::ModuleScope::Local);
    if (!installed) {
        return fail("install of dependency fixture failed: " + installed.error().code());
    }
    if (installed.value().dependencies.size() != 2) {
        return fail("installed record did not capture dependencies");
    }

    const auto metaPath = scope_paths(root).local / "wl2_dyn_deps" / "wl2.module.yml";
    std::ifstream metaIn(metaPath);
    std::string meta((std::istreambuf_iterator<char>(metaIn)), std::istreambuf_iterator<char>());
    if (meta.find("schema: wl2.module.v2") == std::string::npos) {
        return fail("metadata was not written as wl2.module.v2");
    }
    if (meta.find("build:") == std::string::npos) {
        return fail("module build stamp was not written to metadata");
    }
    if (meta.find("wl2:dyn_good") == std::string::npos
        || meta.find("required:") == std::string::npos
        || meta.find("optional:") == std::string::npos) {
        return fail("dependency block was not written to metadata");
    }

    // scanScope re-parses the dependency block.
    auto resolved = store.resolve("wl2:dyn_deps");
    if (!resolved || resolved->dependencies.size() != 2) {
        return fail("scanned record did not read dependencies back");
    }
    if (resolved->dependencies[0].name != "wl2:dyn_good"
        || resolved->dependencies[0].kind != wl2::ModuleDependencyKind::Required
        || resolved->dependencies[0].versionRange != ">=1.0.0 <2.0.0") {
        return fail("required dependency did not round-trip");
    }

    // The scope index includes the dependency metadata.
    std::ifstream indexIn(scope_paths(root).local / "index.yml");
    std::string index((std::istreambuf_iterator<char>(indexIn)), std::istreambuf_iterator<char>());
    if (index.find("wl2:dyn_good") == std::string::npos) {
        return fail("scope index did not include dependency metadata");
    }

    fs::remove_all(root);
    return 0;
}

// Exit criteria: an installed module that depends on another installed module
// resolves the dependency automatically, in dependency-first load order.
int installed_dependency_graph_resolves() {
    const fs::path root = make_temp_root();
    wl2::ModuleStore store(scope_paths(root));
    // wl2:dyn_deps requires wl2:dyn_good; install both into the local scope.
    if (auto rc = store.install(WL2_DYN_GOOD, wl2::ModuleScope::Local); !rc) {
        return fail("install of dependency target failed: " + rc.error().code());
    }
    if (auto rc = store.install(WL2_DYN_DEPS, wl2::ModuleScope::Local); !rc) {
        return fail("install of dependent module failed: " + rc.error().code());
    }

    // Build providers from the installed records, as the CLI does.
    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:dyn_deps", .kind = wl2::ModuleDependencyKind::Required});
    for (const auto& record : store.list()) {
        wl2::ModuleInfo info;
        info.name = record.name;
        info.version = record.version;
        info.abiVersion = record.abiVersion;
        info.stableId = record.stableId;
        info.libraryPath = record.libraryPath;
        info.dependencies = record.dependencies;
        request.providers.push_back(wl2::ModuleProvider{
            .info = std::move(info),
            .source = wl2::ModuleProvider::Source::Local,
            .path = record.libraryPath,
        });
    }

    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        return fail("installed dependency graph failed to resolve: " + plan.error().code());
    }
    // The app only requires wl2:dyn_deps; wl2:dyn_good must be selected and
    // ordered first.
    if (plan.value().loadOrder.size() != 2
        || plan.value().loadOrder[0].provider.info.name != "wl2:dyn_good"
        || plan.value().loadOrder[1].provider.info.name != "wl2:dyn_deps") {
        return fail("transitive installed dependency was not resolved in order");
    }

    // The selected providers load successfully in that order.
    wl2::Runtime runtime;
    for (const auto& resolved : plan.value().loadOrder) {
        auto loaded = runtime.modules().loadDynamicModule(
            resolved.provider.path, runtime, wl2::ModuleShadowPolicy::Allow);
        if (!loaded) {
            return fail("selected provider failed to load: " + loaded.error().code());
        }
    }
    if (!runtime.modules().find("wl2:dyn_good") || !runtime.modules().find("wl2:dyn_deps")) {
        return fail("both modules should be registered after ordered load");
    }

    fs::remove_all(root);
    return 0;
}

// Install records a library checksum; verifyInstalled passes for an intact
// library and rejects a stale or tampered one.
int checksum_validation() {
    const fs::path root = make_temp_root();
    wl2::ModuleStore store(scope_paths(root));
    auto installed = store.install(WL2_DYN_GOOD, wl2::ModuleScope::Local);
    if (!installed) {
        return fail("install failed: " + installed.error().code());
    }
    if (installed.value().checksum.empty()) {
        return fail("install did not record a checksum");
    }

    // The recorded checksum is persisted in the module metadata file.
    {
        std::ifstream meta(scope_paths(root).local / "wl2_dyn_good" / "wl2.module.yml");
        std::string text((std::istreambuf_iterator<char>(meta)), std::istreambuf_iterator<char>());
        if (text.find("sha256:") == std::string::npos) {
            return fail("wl2.module.yml is missing the sha256 field");
        }
    }

    auto record = store.resolve("wl2:dyn_good");
    if (!record || record->checksum.empty()) {
        return fail("resolve did not read back the checksum");
    }
    if (auto rc = store.verifyInstalled(*record); !rc) {
        return fail("verifyInstalled rejected an intact library: " + rc.error().code());
    }

    // Tampering with the installed library is detected.
    {
        std::ofstream lib(record->libraryPath, std::ios::binary | std::ios::app);
        lib << "tampered";
    }
    auto tampered = store.verifyInstalled(*record);
    if (tampered || tampered.error().code() != "module_checksum_mismatch") {
        return fail("verifyInstalled did not detect a tampered library");
    }

    // A missing library is reported distinctly.
    fs::remove(record->libraryPath);
    auto missing = store.verifyInstalled(*record);
    if (missing || missing.error().code() != "module_library_missing") {
        return fail("verifyInstalled did not detect a missing library");
    }

    // A legacy record without a recorded checksum is treated as valid.
    wl2::InstalledModuleRecord legacy = *record;
    legacy.checksum.clear();
    {
        std::ofstream lib(legacy.libraryPath, std::ios::binary | std::ios::trunc);
        lib << "anything";
    }
    if (auto rc = store.verifyInstalled(legacy); !rc) {
        return fail("legacy record without checksum should be treated as valid");
    }

    fs::remove_all(root);
    return 0;
}

} // namespace

int wl2_module_store_tests_entry() {
    if (int rc = run(); rc != 0) {
        return rc;
    }
    if (int rc = checksum_validation(); rc != 0) {
        return rc;
    }
    if (int rc = dependency_metadata_roundtrips(); rc != 0) {
        return rc;
    }
    if (int rc = installed_dependency_graph_resolves(); rc != 0) {
        return rc;
    }
    std::cout << "module_store dependency graph ok\n";
    return 0;
}
