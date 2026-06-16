#include "wl2/wl2.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "dynamic module test failed: " << message << '\n';
    return 1;
}

// Each load needs a runtime to receive the module's factory registrations.
int loads_and_registers_successfully() {
    wl2::Runtime runtime;
    auto loaded = runtime.modules().loadDynamicModule(WL2_DYN_GOOD, runtime);
    if (!loaded) {
        return fail("good module failed to load: " + loaded.error().code());
    }
    if (loaded.value().name != "wl2:dyn_good") {
        return fail("unexpected module name: " + loaded.value().name);
    }
    if (loaded.value().libraryPath.empty()) {
        return fail("loaded module did not record its library path");
    }
    if (!runtime.modules().find("wl2:dyn_good")) {
        return fail("loaded module is not in the registry");
    }
    if (!runtime.findQuickJsModule("wl2:dyn_good")) {
        return fail("loaded module did not register its factory");
    }
    return 0;
}

int missing_library_fails() {
    wl2::Runtime runtime;
    auto loaded = runtime.modules().loadDynamicModule(WL2_DYN_MISSING, runtime);
    if (loaded) {
        return fail("missing library should fail");
    }
    if (loaded.error().code() != "module_library_not_found") {
        return fail("unexpected missing-library code: " + loaded.error().code());
    }
    // The diagnostic should name the path.
    if (loaded.error().message().find(std::filesystem::path(WL2_DYN_MISSING).filename().string()) == std::string::npos) {
        return fail("missing-library diagnostic did not include the path");
    }
    return 0;
}

int expect_load_error(const char* path, const char* expectedCode) {
    wl2::Runtime runtime;
    auto loaded = runtime.modules().loadDynamicModule(path, runtime);
    if (loaded) {
        return fail(std::string("expected failure for ") + path);
    }
    if (loaded.error().code() != expectedCode) {
        return fail(std::string("expected ") + expectedCode + " but got " + loaded.error().code());
    }
    return 0;
}

int duplicate_name_fails_and_shadow_allows() {
    wl2::Runtime runtime;
    auto first = runtime.modules().loadDynamicModule(WL2_DYN_GOOD, runtime);
    if (!first) {
        return fail("first load should succeed: " + first.error().code());
    }
    // A second load of the same name is rejected by default.
    auto duplicate = runtime.modules().loadDynamicModule(WL2_DYN_GOOD, runtime);
    if (duplicate) {
        return fail("duplicate module name should fail");
    }
    if (duplicate.error().code() != "module_duplicate_name") {
        return fail("unexpected duplicate code: " + duplicate.error().code());
    }
    // With an explicit shadow policy it is allowed.
    auto shadow = runtime.modules().loadDynamicModule(
        WL2_DYN_GOOD, runtime, wl2::ModuleShadowPolicy::Allow);
    if (!shadow) {
        return fail("shadow load should succeed when allowed: " + shadow.error().code());
    }
    return 0;
}

int inspect_reports_metadata_without_running() {
    auto info = wl2::ModuleLoader::inspectDynamicModule(WL2_DYN_GOOD);
    if (!info) {
        return fail("inspect should succeed: " + info.error().code());
    }
    if (info.value().name != "wl2:dyn_good" || info.value().abiVersion != wl2::ModuleAbiVersion) {
        return fail("inspect returned unexpected metadata");
    }
    // inspect must fail the same way load does for a bad ABI, without a runtime.
    auto bad = wl2::ModuleLoader::inspectDynamicModule(WL2_DYN_BAD_ABI);
    if (bad || bad.error().code() != "module_abi_mismatch") {
        return fail("inspect should reject a bad ABI");
    }
    return 0;
}

int inspect_reads_dependency_metadata() {
    auto info = wl2::ModuleLoader::inspectDynamicModule(WL2_DYN_DEPS);
    if (!info) {
        return fail("inspect of dependency fixture failed: " + info.error().code());
    }
    if (info.value().abiVersion != wl2::ModuleAbiVersion) {
        return fail("dependency fixture should report the current ABI");
    }
    if (info.value().build.empty()) {
        return fail("dependency fixture should report build metadata");
    }
    if (info.value().dependencies.size() != 2) {
        return fail("dependency fixture did not report 2 dependencies");
    }
    const auto& required = info.value().dependencies[0];
    if (required.name != "wl2:dyn_good"
        || required.kind != wl2::ModuleDependencyKind::Required
        || required.versionRange != ">=1.0.0 <2.0.0") {
        return fail("required dependency metadata was not read correctly");
    }
    const auto& optional = info.value().dependencies[1];
    if (optional.name != "wl2:dyn_metrics"
        || optional.kind != wl2::ModuleDependencyKind::Optional) {
        return fail("optional dependency metadata was not read correctly");
    }
    return 0;
}

int abi_v2_module_still_loads() {
    // The host accepts the previous ABI version and treats it as having no
    // declared dependencies.
    wl2::Runtime runtime;
    auto loaded = runtime.modules().loadDynamicModule(WL2_DYN_ABI2, runtime);
    if (!loaded) {
        return fail("ABI v2 module should still load: " + loaded.error().code());
    }
    if (loaded.value().abiVersion != 2) {
        return fail("ABI v2 module reported an unexpected ABI version");
    }
    if (!loaded.value().dependencies.empty()) {
        return fail("ABI v2 module should declare no dependencies");
    }
    if (!runtime.modules().find("wl2:dyn_abi2")) {
        return fail("ABI v2 module is not in the registry");
    }
    return 0;
}

} // namespace

int wl2_dynamic_module_tests_entry() {
    if (int rc = loads_and_registers_successfully(); rc != 0) {
        return rc;
    }
    if (int rc = missing_library_fails(); rc != 0) {
        return rc;
    }
    if (int rc = expect_load_error(WL2_DYN_NO_GETINFO, "module_missing_get_info"); rc != 0) {
        return rc;
    }
    if (int rc = expect_load_error(WL2_DYN_NO_REGISTER, "module_missing_register"); rc != 0) {
        return rc;
    }
    if (int rc = expect_load_error(WL2_DYN_BAD_ABI, "module_abi_mismatch"); rc != 0) {
        return rc;
    }
    if (int rc = expect_load_error(WL2_DYN_BAD_VERSION, "module_wl2_version_mismatch"); rc != 0) {
        return rc;
    }
    if (int rc = duplicate_name_fails_and_shadow_allows(); rc != 0) {
        return rc;
    }
    if (int rc = inspect_reports_metadata_without_running(); rc != 0) {
        return rc;
    }
    if (int rc = inspect_reads_dependency_metadata(); rc != 0) {
        return rc;
    }
    if (int rc = abi_v2_module_still_loads(); rc != 0) {
        return rc;
    }
    std::cout << "dynamic_module ok\n";
    return 0;
}
