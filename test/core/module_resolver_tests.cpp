#include "wl2/module_resolver.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

int fail(const std::string& message) {
    std::cerr << "module resolver test failed: " << message << '\n';
    return 1;
}

wl2::ModuleProvider provider(
    std::string name,
    std::string version,
    wl2::ModuleProvider::Source source = wl2::ModuleProvider::Source::Builtin) {
    wl2::ModuleProvider out;
    out.source = source;
    out.info.name = std::move(name);
    out.info.version = std::move(version);
    out.info.abiVersion = wl2::ModuleAbiVersion;
    return out;
}

wl2::ModuleDependencyRequirement required(std::string name, std::string range = {}) {
    wl2::ModuleDependencyRequirement dep;
    dep.name = std::move(name);
    dep.versionRange = std::move(range);
    dep.kind = wl2::ModuleDependencyKind::Required;
    return dep;
}

wl2::ModuleDependencyRequirement optional(std::string name) {
    wl2::ModuleDependencyRequirement dep;
    dep.name = std::move(name);
    dep.kind = wl2::ModuleDependencyKind::Optional;
    return dep;
}

int version_checks() {
    auto exact = wl2::moduleVersionSatisfies("1.2.3", "=1.2.3");
    if (!exact || !exact.value()) {
        return fail("exact version did not match");
    }
    auto range = wl2::moduleVersionSatisfies("1.2.3", ">=1.0.0 <2.0.0");
    if (!range || !range.value()) {
        return fail("range version did not match");
    }
    auto outside = wl2::moduleVersionSatisfies("2.0.0", ">=1.0.0 <2.0.0");
    if (!outside || outside.value()) {
        return fail("range version should not match upper bound");
    }
    auto invalid = wl2::moduleVersionSatisfies("1.2.3", "^1.2.3");
    if (invalid || invalid.error().code() != "module_dependency_invalid_version_range") {
        return fail("invalid range was accepted");
    }
    return 0;
}

int version_mismatch_fails() {
    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:threads", .kind = wl2::ModuleDependencyKind::Required});
    request.providers = {provider("wl2:threads", "0.2.0")};
    wl2::ModuleDependencyRequirement dep;
    dep.name = "wl2:threads";
    dep.versionRange = ">=0.1.0 <0.2.0";
    request.roots.clear();
    auto app = provider("wl2:app", "0.1.0");
    app.info.dependencies.push_back(dep);
    request.roots.push_back({.name = "wl2:app", .kind = wl2::ModuleDependencyKind::Required});
    request.providers.push_back(app);

    auto plan = wl2::resolveModuleGraph(request);
    if (plan || plan.error().code() != "module_dependency_version_mismatch") {
        return fail("version mismatch did not fail with stable diagnostic");
    }
    return 0;
}

int stable_id_mismatch_fails() {
    auto threads = provider("wl2:threads", "0.1.0");
    threads.info.stableId = "actual";
    auto app = provider("wl2:app", "0.1.0");
    auto dep = required("wl2:threads");
    dep.stableId = "expected";
    app.info.dependencies.push_back(dep);

    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:app", .kind = wl2::ModuleDependencyKind::Required});
    request.providers = {app, threads};
    auto plan = wl2::resolveModuleGraph(request);
    if (plan || plan.error().code() != "module_dependency_identity_mismatch") {
        return fail("stableId mismatch did not fail with stable diagnostic");
    }
    return 0;
}

int shadowing_is_diagnostic() {
    auto explicitProvider = provider("wl2:fs", "0.2.0", wl2::ModuleProvider::Source::Explicit);
    auto builtinProvider = provider("wl2:fs", "0.1.0", wl2::ModuleProvider::Source::Builtin);

    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:fs", .kind = wl2::ModuleDependencyKind::Required});
    request.providers = {builtinProvider, explicitProvider};
    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        return fail("shadowing graph failed");
    }
    if (plan.value().loadOrder.empty()
        || plan.value().loadOrder[0].provider.source != wl2::ModuleProvider::Source::Explicit) {
        return fail("higher-priority provider was not selected");
    }
    if (plan.value().diagnostics.empty()
        || plan.value().diagnostics[0].code != "module_provider_shadowed") {
        return fail("shadowed provider did not produce diagnostic");
    }
    return 0;
}

int source_metadata() {
    const fs::path root = fs::temp_directory_path() / fs::path("wl2-module-source-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path file = root / "wl2.module.source.yml";
    {
        std::ofstream out(file);
        out
            << "schema: wl2.module-source.v1\n"
            << "provides: wl2:asio\n"
            << "version: 0.2.0\n"
            << "stableId: stable-asio\n"
            << "path: modules/wl2_asio\n"
            << "dependencies:\n"
            << "  required:\n"
            << "    - name: wl2:threads\n"
            << "      version: \">=0.1.0 <0.2.0\"\n"
            << "  optional:\n"
            << "    - name: wl2:metrics\n"
            << "sourceDependencies:\n"
            << "  modules:\n"
            << "    - name: wl2_threads\n"
            << "      provides: wl2:threads\n"
            << "      git: https://example.com/wl2_threads.git\n"
            << "      tag: v0.1.0\n";
    }
    auto metadata = wl2::loadModuleSourceMetadata(file);
    fs::remove_all(root);
    if (!metadata) {
        return fail("source metadata failed to parse: " + metadata.error().code());
    }
    if (metadata.value().provides != "wl2:asio"
        || metadata.value().dependencies.size() != 2
        || metadata.value().dependencies[0].kind != wl2::ModuleDependencyKind::Required
        || metadata.value().dependencies[1].kind != wl2::ModuleDependencyKind::Optional
        || metadata.value().sourceDependencies.size() != 1) {
        return fail("source metadata fields were not parsed correctly");
    }
    return 0;
}

int resolves_dependency_order() {
    auto asio = provider("wl2:asio", "0.2.0", wl2::ModuleProvider::Source::Project);
    asio.info.dependencies.push_back(required("wl2:threads", ">=0.1.0 <0.2.0"));
    auto threads = provider("wl2:threads", "0.1.0", wl2::ModuleProvider::Source::Local);

    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:asio", .kind = wl2::ModuleDependencyKind::Required});
    request.providers = {asio, threads};
    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        return fail("resolver failed: " + plan.error().code());
    }
    if (plan.value().loadOrder.size() != 2
        || plan.value().loadOrder[0].provider.info.name != "wl2:threads"
        || plan.value().loadOrder[1].provider.info.name != "wl2:asio") {
        return fail("resolver did not produce dependency-first order");
    }
    return 0;
}

int diamond_graph() {
    auto app = provider("wl2:app", "0.1.0", wl2::ModuleProvider::Source::Project);
    app.info.dependencies.push_back(required("wl2:left"));
    app.info.dependencies.push_back(required("wl2:right"));
    auto left = provider("wl2:left", "0.1.0", wl2::ModuleProvider::Source::Local);
    left.info.dependencies.push_back(required("wl2:base"));
    auto right = provider("wl2:right", "0.1.0", wl2::ModuleProvider::Source::Local);
    right.info.dependencies.push_back(required("wl2:base"));
    auto base = provider("wl2:base", "0.1.0", wl2::ModuleProvider::Source::Builtin);

    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:app", .kind = wl2::ModuleDependencyKind::Required});
    request.providers = {app, left, right, base};
    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        return fail("diamond graph failed: " + plan.error().code());
    }
    const auto& order = plan.value().loadOrder;
    if (order.size() != 4) {
        return fail("diamond graph did not select each provider exactly once");
    }
    // base must precede left and right, which must precede app.
    auto indexOf = [&](const std::string& name) -> int {
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i].provider.info.name == name) return static_cast<int>(i);
        }
        return -1;
    };
    const int baseIdx = indexOf("wl2:base");
    const int leftIdx = indexOf("wl2:left");
    const int rightIdx = indexOf("wl2:right");
    const int appIdx = indexOf("wl2:app");
    if (baseIdx < 0 || leftIdx < 0 || rightIdx < 0 || appIdx < 0
        || baseIdx > leftIdx || baseIdx > rightIdx
        || leftIdx > appIdx || rightIdx > appIdx) {
        return fail("diamond graph was not topologically ordered");
    }
    return 0;
}

int optional_cycle_is_diagnostic() {
    // An entirely optional cycle must not abort resolution; it reports a cycle
    // diagnostic instead.
    auto a = provider("wl2:a", "0.1.0");
    a.info.dependencies.push_back(optional("wl2:b"));
    auto b = provider("wl2:b", "0.1.0");
    b.info.dependencies.push_back(optional("wl2:a"));

    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:a", .kind = wl2::ModuleDependencyKind::Optional});
    request.providers = {a, b};
    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        return fail("optional cycle aborted resolution");
    }
    bool sawCycle = false;
    for (const auto& diagnostic : plan.value().diagnostics) {
        if (diagnostic.code == "module_dependency_cycle") {
            sawCycle = true;
        }
    }
    if (!sawCycle) {
        return fail("optional cycle did not produce a cycle diagnostic");
    }
    return 0;
}

int missing_required_fails() {
    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:absent", .kind = wl2::ModuleDependencyKind::Required});
    auto plan = wl2::resolveModuleGraph(request);
    if (plan || plan.error().code() != "module_dependency_missing") {
        return fail("missing required module did not fail");
    }
    return 0;
}

int optional_missing_is_diagnostic() {
    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:optional", .kind = wl2::ModuleDependencyKind::Optional});
    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        return fail("missing optional module failed resolution");
    }
    if (plan.value().diagnostics.empty() || plan.value().diagnostics[0].code != "module_optional_missing") {
        return fail("missing optional module did not produce diagnostic");
    }
    return 0;
}

int cycle_fails() {
    auto a = provider("wl2:a", "0.1.0");
    a.info.dependencies.push_back(required("wl2:b"));
    auto b = provider("wl2:b", "0.1.0");
    b.info.dependencies.push_back(required("wl2:a"));

    wl2::ModuleResolutionRequest request;
    request.roots.push_back({.name = "wl2:a", .kind = wl2::ModuleDependencyKind::Required});
    request.providers = {a, b};
    auto plan = wl2::resolveModuleGraph(request);
    if (plan || plan.error().code() != "module_dependency_cycle") {
        return fail("cycle did not fail");
    }
    return 0;
}

} // namespace

int wl2_module_resolver_tests_entry() {
    if (int rc = version_checks(); rc != 0) return rc;
    if (int rc = version_mismatch_fails(); rc != 0) return rc;
    if (int rc = stable_id_mismatch_fails(); rc != 0) return rc;
    if (int rc = shadowing_is_diagnostic(); rc != 0) return rc;
    if (int rc = source_metadata(); rc != 0) return rc;
    if (int rc = resolves_dependency_order(); rc != 0) return rc;
    if (int rc = diamond_graph(); rc != 0) return rc;
    if (int rc = optional_cycle_is_diagnostic(); rc != 0) return rc;
    if (int rc = missing_required_fails(); rc != 0) return rc;
    if (int rc = optional_missing_is_diagnostic(); rc != 0) return rc;
    if (int rc = cycle_fails(); rc != 0) return rc;
    std::cout << "module_resolver ok\n";
    return 0;
}
