// Validates that each built-in static module's CMake-declared dependencies
// (REQUIRES_MODULES + OPTIONAL_MODULES, recorded by wl2_add_module) match the
// dependencies declared in its C++ ModuleInfo. This guards against the two
// metadata sources drifting apart as modules gain real dependencies.
#include "wl2/wl2.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

void wl2_register_builtin_static_modules(wl2::RuntimeOptions& options);
void wl2_append_builtin_static_module_cmake_dependencies(
    std::vector<std::pair<std::string, std::vector<std::string>>>& out);

int main() {
    wl2::RuntimeOptions options;
    wl2_register_builtin_static_modules(options);
    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        std::cerr << "runtime initialize failed: " << init.error().code() << '\n';
        return 1;
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> cmakeDeps;
    wl2_append_builtin_static_module_cmake_dependencies(cmakeDeps);

    int failures = 0;
    for (const auto& [name, declared] : cmakeDeps) {
        const wl2::ModuleInfo* info = runtime.modules().find(name);
        if (!info) {
            std::cerr << "static module not registered at runtime: " << name << '\n';
            ++failures;
            continue;
        }
        std::vector<std::string> fromCMake = declared;
        std::vector<std::string> fromCpp;
        for (const auto& dep : info->dependencies) {
            fromCpp.push_back(dep.name);
        }
        std::sort(fromCMake.begin(), fromCMake.end());
        std::sort(fromCpp.begin(), fromCpp.end());
        fromCMake.erase(std::unique(fromCMake.begin(), fromCMake.end()), fromCMake.end());
        fromCpp.erase(std::unique(fromCpp.begin(), fromCpp.end()), fromCpp.end());
        if (fromCMake != fromCpp) {
            std::cerr << "dependency metadata mismatch for " << name << ":\n  CMake: ";
            for (const auto& s : fromCMake) std::cerr << s << ' ';
            std::cerr << "\n  C++:   ";
            for (const auto& s : fromCpp) std::cerr << s << ' ';
            std::cerr << '\n';
            ++failures;
        }
    }

    if (failures) {
        return 1;
    }
    std::cout << "static module metadata consistent\n";
    return 0;
}
