wl2_generate_static_module_registry(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wl2_builtin_static_modules.cpp"
    OUT_SOURCE _wl2_builtin_static_modules_source
    OUT_TARGETS _wl2_builtin_static_module_targets)

add_executable(wl2 main.cpp "${_wl2_builtin_static_modules_source}")
target_link_libraries(wl2 PRIVATE wl2_core ${_wl2_builtin_static_module_targets})

# Export the runner's dynamic symbols so dynamically loaded native modules can
# resolve host-provided symbols (the JavaScript engine, wl2 runtime) at dlopen.
set_target_properties(wl2 PROPERTIES ENABLE_EXPORTS ON)

if(WL2_BUILD_TESTING)
    # Validates CMake-declared static module dependencies against C++ ModuleInfo.
    add_executable(wl2_static_module_metadata_check
        static_module_metadata_check.cpp
        "${_wl2_builtin_static_modules_source}")
    target_link_libraries(wl2_static_module_metadata_check
        PRIVATE wl2_core ${_wl2_builtin_static_module_targets})
    add_test(NAME core.static_module_metadata
        COMMAND wl2_static_module_metadata_check)
    set_tests_properties(core.static_module_metadata PROPERTIES
        LABELS "cmake;modules")
endif()
