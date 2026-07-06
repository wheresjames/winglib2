if(NOT WL2_WL2_MODULE_MODE STREQUAL "dynamic"
        AND NOT WL2_WL2_MODULE_MODE STREQUAL "static"
        AND NOT WL2_WL2_MODULE_MODE STREQUAL "dynamic-with-static-fallback")
    message(FATAL_ERROR "WL2_WL2_MODULE_MODE must be dynamic, static, or dynamic-with-static-fallback")
endif()
if(WL2_WL2_MODULE_MODE STREQUAL "static" AND NOT WL2_BUILD_STATIC_MODULES)
    message(FATAL_ERROR "WL2_WL2_MODULE_MODE=static requires WL2_BUILD_STATIC_MODULES=ON")
endif()
if(WL2_WL2_MODULE_MODE STREQUAL "dynamic" AND NOT WL2_BUILD_SHARED_MODULES)
    message(FATAL_ERROR "WL2_WL2_MODULE_MODE=dynamic requires WL2_BUILD_SHARED_MODULES=ON")
endif()
if(WL2_WL2_MODULE_MODE STREQUAL "dynamic-with-static-fallback" AND NOT WL2_BUILD_STATIC_MODULES)
    message(FATAL_ERROR "WL2_WL2_MODULE_MODE=dynamic-with-static-fallback requires WL2_BUILD_STATIC_MODULES=ON")
endif()
if(WL2_WL2_MODULE_MODE STREQUAL "dynamic-with-static-fallback" AND NOT WL2_BUILD_SHARED_MODULES)
    message(FATAL_ERROR "WL2_WL2_MODULE_MODE=dynamic-with-static-fallback requires WL2_BUILD_SHARED_MODULES=ON")
endif()

wl2_generate_static_module_registry(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wl2_builtin_static_modules_full.cpp"
    OUT_SOURCE _wl2_builtin_static_modules_full_source
    OUT_TARGETS _wl2_builtin_static_module_targets)

set(_wl2_builtin_static_modules_empty_source
    "${CMAKE_CURRENT_BINARY_DIR}/wl2_builtin_static_modules_empty.cpp")
file(WRITE "${_wl2_builtin_static_modules_empty_source}"
    "#include \"wl2/runtime.h\"\n"
    "#include <string>\n"
    "#include <utility>\n"
    "#include <vector>\n\n"
    "void wl2_register_builtin_static_modules(wl2::RuntimeOptions&) {}\n"
    "void wl2_append_builtin_static_module_names(std::vector<std::string>&) {}\n"
    "void wl2_append_builtin_static_module_cmake_dependencies(\n"
    "    std::vector<std::pair<std::string, std::vector<std::string>>>&) {}\n")

# `static` and `dynamic-with-static-fallback` both link the full builtin static
# registry into the runner. `dynamic` links an empty registry so a missing
# dynamic module cannot fall back to a static copy by default.
if(WL2_WL2_MODULE_MODE STREQUAL "static"
        OR WL2_WL2_MODULE_MODE STREQUAL "dynamic-with-static-fallback")
    set(_wl2_builtin_static_modules_source "${_wl2_builtin_static_modules_full_source}")
    set(_wl2_runner_static_module_targets ${_wl2_builtin_static_module_targets})
    set(_wl2_builtin_fallback_default 1)
else()
    set(_wl2_builtin_static_modules_source "${_wl2_builtin_static_modules_empty_source}")
    set(_wl2_runner_static_module_targets)
    set(_wl2_builtin_fallback_default 0)
endif()
if(WL2_WL2_MODULE_MODE STREQUAL "static")
    set(_wl2_runner_static_mode 1)
else()
    set(_wl2_runner_static_mode 0)
endif()

wl2_generate_dynamic_module_store(OUT_TARGET _wl2_dynamic_module_store_target)

add_executable(wl2 main.cpp "${_wl2_builtin_static_modules_source}")
target_link_libraries(wl2 PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,wl2_core>"
    ${_wl2_runner_static_module_targets})
target_compile_definitions(wl2 PRIVATE
    WL2_BUILD_TREE_MODULE_DIR="${CMAKE_BINARY_DIR}/lib/wl2/modules"
    WL2_INSTALLED_MODULE_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/wl2/modules"
    WL2_BUILTIN_FALLBACK_DEFAULT=${_wl2_builtin_fallback_default}
    WL2_RUNNER_STATIC_MODE=${_wl2_runner_static_mode})
# Executable-relative installed-module lookup needs the libdir name; only a
# relative libdir (lib, lib64, ...) can anchor to the executable's prefix.
if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    target_compile_definitions(wl2 PRIVATE
        WL2_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}")
endif()
if(TARGET ${_wl2_dynamic_module_store_target})
    add_dependencies(wl2 ${_wl2_dynamic_module_store_target})
endif()

# The runner uses nlohmann/json directly for CLI JSON parsing and reporting,
# the same required core dependency behind the wl2:json module and TrustStore.
target_link_libraries(wl2 PRIVATE nlohmann_json::nlohmann_json)

# Export the runner's dynamic symbols so dynamically loaded native modules can
# resolve host-provided symbols (the JavaScript engine, wl2 runtime) at dlopen.
set_target_properties(wl2 PROPERTIES ENABLE_EXPORTS ON)

if(WL2_BUILD_TESTING AND WL2_BUILD_STATIC_MODULES)
    # Validates CMake-declared static module dependencies against C++ ModuleInfo.
    add_executable(wl2_static_module_metadata_check
        static_module_metadata_check.cpp
        "${_wl2_builtin_static_modules_full_source}")
    target_link_libraries(wl2_static_module_metadata_check
        PRIVATE wl2_core ${_wl2_builtin_static_module_targets})
    add_test(NAME core.static_module_metadata
        COMMAND wl2_static_module_metadata_check)
    set_tests_properties(core.static_module_metadata PROPERTIES
        LABELS "cmake;modules")
endif()
