# Test suites (opt-in via WL2_BUILD_TESTING): C++ unit suites under test/core,
# CMake-driven fixtures, and CLI/JavaScript smoke tests driving the wl2 runner.
#
# Included from the root CMakeLists.txt (same scope), so CMAKE_CURRENT_SOURCE_DIR
# and CMAKE_CURRENT_BINARY_DIR point at the project root and its build tree and
# every ${CMAKE_CURRENT_SOURCE_DIR}/test/... path resolves as before.

# Tests: C++ unit suites (test/core) plus CMake-driven and CLI/JS smoke tests.
if(WL2_BUILD_TESTING)
    add_subdirectory(test/core)
    # CMake-level tests: resource policy conflicts and static module graph order.
    add_test(NAME cmake.resources.conflicting_policy
        COMMAND
            ${CMAKE_COMMAND}
            -DRESOURCE_CONFLICT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/test/cmake/resource_conflict
            -DRESOURCE_CONFLICT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/resource_conflict
            -DWL2_RESOURCES_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Resources.cmake
            -DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/cmake/resource_conflict/run.cmake)
    set_tests_properties(cmake.resources.conflicting_policy PROPERTIES
        LABELS "cmake;resources")
    add_test(NAME cmake.static_module_graph
        COMMAND
            ${CMAKE_COMMAND}
            -DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}
            -DWL2_MODULES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Modules.cmake
            -DFIXTURE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/test/cmake/static_module_graph
            -DFIXTURE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/static_module_graph
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/cmake/static_module_graph/run.cmake)
    set_tests_properties(cmake.static_module_graph PROPERTIES
        LABELS "cmake;modules")
    # CLI / JavaScript smoke tests driving the built wl2 runner.
    add_test(NAME scripts.wl2_buffer_smoke
        COMMAND
            $<TARGET_FILE:wl2>
            run
            ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_buffer_smoke.js)
    set_tests_properties(scripts.wl2_buffer_smoke PROPERTIES
        LABELS "js")
    add_test(NAME scripts.wl2_runtime_smoke
        COMMAND
            $<TARGET_FILE:wl2>
            run
            ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_runtime_smoke.js
            --
            runtime-arg-1
            runtime-arg-2)
    set_tests_properties(scripts.wl2_runtime_smoke PROPERTIES
        LABELS "js;runtime;smoke")
    add_test(NAME scripts.wl2_cli_version
        COMMAND
            $<TARGET_FILE:wl2>
            version)
    set_tests_properties(scripts.wl2_cli_version PROPERTIES
        LABELS "js;cli")
    add_test(NAME scripts.wl2_cli_help
        COMMAND
            $<TARGET_FILE:wl2>
            --help)
    set_tests_properties(scripts.wl2_cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "usage:"
        LABELS "js;cli")
    add_test(NAME scripts.wl2_cli_subcommand_help
        COMMAND
            $<TARGET_FILE:wl2>
            resources
            --help)
    set_tests_properties(scripts.wl2_cli_subcommand_help PROPERTIES
        PASS_REGULAR_EXPRESSION "wl2 resources"
        LABELS "js;cli")
    add_test(NAME scripts.wl2_cli_invalid_app_action
        COMMAND
            ${CMAKE_COMMAND}
            -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
            -DEXPECT_MODE=invalid_app_action
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_wl2_failure.cmake)
    set_tests_properties(scripts.wl2_cli_invalid_app_action PROPERTIES
        LABELS "js;cli")
    add_test(NAME scripts.wl2_shebang_smoke
        COMMAND
            $<TARGET_FILE:wl2>
            run
            ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_shebang_smoke.js)
    set_tests_properties(scripts.wl2_shebang_smoke PROPERTIES
        LABELS "js;cli")
    set(_wl2_resource_map_arg "${CMAKE_CURRENT_SOURCE_DIR}/examples/js/resources/files:wl2:/mapped")
    add_test(NAME scripts.wl2_resource_map_smoke
        COMMAND
            $<TARGET_FILE:wl2>
            run
            --map-resource
            ${_wl2_resource_map_arg}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_resource_map_smoke.js)
    set_tests_properties(scripts.wl2_resource_map_smoke PROPERTIES
        LABELS "js;cli;resources")
    add_test(NAME scripts.wl2_resource_trace
        COMMAND
            $<TARGET_FILE:wl2>
            run
            --trace-resources
            --map-resource
            ${_wl2_resource_map_arg}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_resource_map_smoke.js)
    set_tests_properties(scripts.wl2_resource_trace PROPERTIES
        LABELS "js;cli;resources"
        PASS_REGULAR_EXPRESSION "wl2 resource open: wl2:/mapped/config.json")
    add_test(NAME scripts.wl2_config_resource_maps
        COMMAND
            $<TARGET_FILE:wl2>
            config
            --map-resource
            ${_wl2_resource_map_arg})
    set_tests_properties(scripts.wl2_config_resource_maps PROPERTIES
        LABELS "js;cli;resources"
        PASS_REGULAR_EXPRESSION "wl2:/mapped")
        add_test(NAME scripts.wl2_resources_cli
            COMMAND
                ${CMAKE_COMMAND}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DRESOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/examples/js/resources/files
                -DEXTRACT_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/resource_extract
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_resources_cli.cmake)
    set_tests_properties(scripts.wl2_resources_cli PROPERTIES
        LABELS "js;cli;resources")
    if(TARGET wl2_resources_js)
        add_test(NAME scripts.wl2_executable_resources
            COMMAND
                ${CMAKE_COMMAND}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DRESOURCE_EXE=$<TARGET_FILE:wl2_resources_js>
                -DEXTRACT_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/executable_resource_extract
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_executable_resources.cmake)
        set_tests_properties(scripts.wl2_executable_resources PROPERTIES
            LABELS "js;cli;resources")
    endif()
    add_test(NAME scripts.wl2_manifest_run
        COMMAND
            $<TARGET_FILE:wl2>
            run
            --manifest
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/js/resources/resources.yml)
    set_tests_properties(scripts.wl2_manifest_run PROPERTIES
        LABELS "js;cli;resources;manifest")
    add_test(NAME scripts.wl2_manifest_cli
        COMMAND
            ${CMAKE_COMMAND}
            -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
            -DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/examples/js/resources/resources.yml
            -DEXTRACT_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/manifest_extract
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_manifest_cli.cmake)
    set_tests_properties(scripts.wl2_manifest_cli PROPERTIES
        LABELS "js;cli;resources;manifest")
    add_test(NAME scripts.wl2_manifest_unknown_key
        COMMAND
            ${CMAKE_COMMAND}
            -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/bad_manifest
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_manifest_failure.cmake)
    set_tests_properties(scripts.wl2_manifest_unknown_key PROPERTIES
        LABELS "js;cli;manifest")
    add_test(NAME scripts.wl2_stack_trace_smoke
        COMMAND
            ${CMAKE_COMMAND}
            -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
            -DEXPECT_MODE=stack_on
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_stack_trace_smoke.js
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_wl2_failure.cmake)
    set_tests_properties(scripts.wl2_stack_trace_smoke PROPERTIES
        LABELS "js;cli")
    add_test(NAME scripts.wl2_stack_trace_off
        COMMAND
            ${CMAKE_COMMAND}
            -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
            -DEXPECT_MODE=stack_off
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_stack_trace_smoke.js
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_wl2_failure.cmake)
    set_tests_properties(scripts.wl2_stack_trace_off PROPERTIES
        LABELS "js;cli")
    add_test(NAME scripts.wl2_promise_rejection_stack
        COMMAND
            ${CMAKE_COMMAND}
            -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
            -DEXPECT_MODE=promise_stack
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_promise_rejection_smoke.js
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_wl2_failure.cmake)
    set_tests_properties(scripts.wl2_promise_rejection_stack PROPERTIES
        LABELS "js;cli")
    # Dynamic-module tests: build/validate/install/load and dependency graphs.
    if(TARGET wl2_echo)
        # wl2_echo's dynamic MODULE target exercises the dynamic loader through
        # the wl2 runner: validate metadata, then load it and import from JS.
        add_test(NAME scripts.wl2_module_validate
            COMMAND $<TARGET_FILE:wl2> module validate $<TARGET_FILE:wl2_echo>)
        set_tests_properties(scripts.wl2_module_validate PROPERTIES
            LABELS "cli;modules;dynamic"
            PASS_REGULAR_EXPRESSION "module: wl2:echo")
        add_test(NAME scripts.wl2_run_load_module
            COMMAND
                $<TARGET_FILE:wl2>
                run
                --load-module $<TARGET_FILE:wl2_echo>
                ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/wl2_dynamic_module_smoke.js)
        set_tests_properties(scripts.wl2_run_load_module PROPERTIES
            LABELS "cli;modules;dynamic;js"
            PASS_REGULAR_EXPRESSION "dynamic module smoke ok")
        add_test(NAME scripts.wl2_module_validate_missing
            COMMAND $<TARGET_FILE:wl2> module validate ${CMAKE_CURRENT_BINARY_DIR}/no-such-module.so)
        set_tests_properties(scripts.wl2_module_validate_missing PROPERTIES
            LABELS "cli;modules;dynamic"
            WILL_FAIL TRUE)
        add_test(NAME scripts.wl2_deps_modules
            COMMAND
                ${CMAKE_COMMAND}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DWL2_ECHO_SO=$<TARGET_FILE:wl2_echo>
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/deps_modules
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_deps_modules.cmake)
        set_tests_properties(scripts.wl2_deps_modules PROPERTIES
            LABELS "cli;modules;dynamic;git"
            TIMEOUT 60)
        if(TARGET dyn_deps AND TARGET dyn_good)
            # Transitive installed dynamic dependency graph: install a module and
            # its dependency, then resolve/run requiring only the dependent.
            add_test(NAME scripts.wl2_module_graph
                COMMAND
                    ${CMAKE_COMMAND}
                    -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                    -DWL2_DYN_GOOD=$<TARGET_FILE:dyn_good>
                    -DWL2_DYN_DEPS=$<TARGET_FILE:dyn_deps>
                    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/module_graph
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_module_graph.cmake)
            set_tests_properties(scripts.wl2_module_graph PROPERTIES
                LABELS "cli;modules;dynamic"
                TIMEOUT 60)
        endif()
        add_test(NAME scripts.wl2_config_watch_sourcemaps
            COMMAND
                ${CMAKE_COMMAND}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DWL2_ECHO_SO=$<TARGET_FILE:wl2_echo>
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/config_watch_sourcemaps
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_config_watch_sourcemaps.cmake)
        set_tests_properties(scripts.wl2_config_watch_sourcemaps PROPERTIES
            LABELS "cli;watch;manifest;modules"
            TIMEOUT 30)
        add_test(NAME scripts.wl2_js_test_scaffolds
            COMMAND
                ${CMAKE_COMMAND}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DMAIN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/js_test_scaffolds
                -DGENERATOR=${CMAKE_GENERATOR}
                -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
                -DBUILD_TYPE=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_js_test_scaffolds.cmake)
        set_tests_properties(scripts.wl2_js_test_scaffolds PROPERTIES
            LABELS "cli;test;scaffold;modules"
            TIMEOUT 180)
        add_test(NAME scripts.wl2_app_install
            COMMAND
                ${CMAKE_COMMAND}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DMAIN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test/app_install
                -DGENERATOR=${CMAKE_GENERATOR}
                -DBUILD_TYPE=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/expect_app_install.cmake)
        set_tests_properties(scripts.wl2_app_install PROPERTIES
            LABELS "cli;apps;install"
            TIMEOUT 180)
    endif()
    if(WL2_BUILD_OUTOFTREE_TESTS)
        # Installs Winglib2 into a throwaway prefix and builds/tests
        # examples/modules/wl2_echo against it with find_package(winglib2).
        add_test(NAME outoftree.wl2_echo
            COMMAND
                ${CMAKE_COMMAND}
                -DMAIN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DEXAMPLE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples/modules/wl2_echo
                -DWORK_DIR=${CMAKE_BINARY_DIR}/test/outoftree/wl2_echo
                -DGENERATOR=${CMAKE_GENERATOR}
                -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
                -DBUILD_TYPE=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/outoftree/run_wl2_echo.cmake)
        set_tests_properties(outoftree.wl2_echo PROPERTIES
            LABELS "outoftree;modules;echo;cmake"
            TIMEOUT 300)
        # Generates out-of-tree source-dependency module repos and drives
        # wl2 deps lock/fetch/build/install for a module and its transitive dep.
        add_test(NAME outoftree.deps_build
            COMMAND
                ${CMAKE_COMMAND}
                -DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}
                -DMAIN_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
                -DWORK_DIR=${CMAKE_BINARY_DIR}/test/outoftree/deps_build
                -DGENERATOR=${CMAKE_GENERATOR}
                -DBUILD_TYPE=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/outoftree/run_deps_build.cmake)
        set_tests_properties(outoftree.deps_build PROPERTIES
            LABELS "outoftree;modules;deps;cmake"
            TIMEOUT 360)
        add_test(NAME outoftree.extended_module
            COMMAND
                ${CMAKE_COMMAND}
                -DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}
                -DMAIN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DWORK_DIR=${CMAKE_BINARY_DIR}/test/outoftree/extended_module
                -DGENERATOR=${CMAKE_GENERATOR}
                -DBUILD_TYPE=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/outoftree/run_extended_module.cmake)
        set_tests_properties(outoftree.extended_module PROPERTIES
            LABELS "outoftree;modules;cmake"
            TIMEOUT 360)
    endif()

    if(TARGET wl2_3d_static AND TARGET wl2_slint_static AND TARGET wl2_membus_static)
        add_test(NAME scripts.wl2_3d_morph3d_compile
            COMMAND
                $<TARGET_FILE:wl2>
                run
                --allow graphics,shared-memory:/wl2_morph3d_
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/js/scripts/morph3d.js
                --
                --compile-only)
        set_tests_properties(scripts.wl2_3d_morph3d_compile PROPERTIES
            LABELS "3d;js;slint;membus;examples;smoke"
            TIMEOUT 30)

        option(WL2_SLINT_DISPLAY_TESTS "Register wl2_slint windowed tests that need a real display" OFF)
        if(WL2_SLINT_DISPLAY_TESTS)
            add_test(NAME scripts.wl2_3d_morph3d_selftest
                COMMAND
                    $<TARGET_FILE:wl2>
                    run
                    --allow ui,graphics,shared-memory:/wl2_morph3d_
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/js/scripts/morph3d.js
                    --
                    --selftest)
            set_tests_properties(scripts.wl2_3d_morph3d_selftest PROPERTIES
                LABELS "3d;js;slint;membus;examples;display;smoke"
                ENVIRONMENT "SLINT_BACKEND=winit-software"
                TIMEOUT 45)
        endif()
    endif()
endif()
