add_executable(wl2_core_tests
    app_store_tests.cpp
    async_host_tests.cpp
    async_integration_tests.cpp
    buffer_tests.cpp
    dynamic_module_tests.cpp
    membus_tests.cpp
    module_deps_tests.cpp
    module_requirements_tests.cpp
    module_resolver_tests.cpp
    module_store_tests.cpp
    resource_tests.cpp
    runtime_tests.cpp
    script_thread_tests.cpp
    thread_tree_tests.cpp)

target_link_libraries(wl2_core_tests PRIVATE wl2_core)

# Dynamic module fixtures: small MODULE libraries exercising the loader's
# success and failure paths. They depend only on the wl2 module ABI header.
set(_wl2_dynamic_fixtures
    dyn_good dyn_no_getinfo dyn_no_register dyn_bad_abi dyn_bad_version dyn_deps dyn_abi2)
foreach(_fixture IN LISTS _wl2_dynamic_fixtures)
    add_library(${_fixture} MODULE dynamic_fixtures/${_fixture}.cpp)
    target_include_directories(${_fixture} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_features(${_fixture} PRIVATE cxx_std_20)
endforeach()
# Fixtures that must satisfy the host wl2 version requirement at load time.
target_compile_definitions(dyn_good PRIVATE WL2_VERSION="${PROJECT_VERSION}" WL2_BUILD="${APPBUILD}")
target_compile_definitions(dyn_deps PRIVATE WL2_VERSION="${PROJECT_VERSION}" WL2_BUILD="${APPBUILD}")
target_compile_definitions(dyn_abi2 PRIVATE WL2_VERSION="${PROJECT_VERSION}")

add_dependencies(wl2_core_tests ${_wl2_dynamic_fixtures})
target_compile_definitions(wl2_core_tests PRIVATE
    WL2_DYN_GOOD="$<TARGET_FILE:dyn_good>"
    WL2_DYN_NO_GETINFO="$<TARGET_FILE:dyn_no_getinfo>"
    WL2_DYN_NO_REGISTER="$<TARGET_FILE:dyn_no_register>"
    WL2_DYN_BAD_ABI="$<TARGET_FILE:dyn_bad_abi>"
    WL2_DYN_BAD_VERSION="$<TARGET_FILE:dyn_bad_version>"
    WL2_DYN_DEPS="$<TARGET_FILE:dyn_deps>"
    WL2_DYN_ABI2="$<TARGET_FILE:dyn_abi2>"
    WL2_DYN_MISSING="${CMAKE_CURRENT_BINARY_DIR}/dynamic_fixtures/does-not-exist.so")

# Crash report fixture: installs the crash reporter and deliberately crashes so
# the report contents can be inspected by a CMake-driven test.
add_executable(wl2_crash_fixture crash_fixture.cpp)
target_link_libraries(wl2_crash_fixture PRIVATE wl2_core)
if(NOT WIN32)
    # Export symbols so backtrace_symbols_fd resolves frame names.
    target_link_options(wl2_crash_fixture PRIVATE -rdynamic)
endif()

add_test(NAME crash.report
    COMMAND
        ${CMAKE_COMMAND}
        -DWL2_CRASH_FIXTURE=$<TARGET_FILE:wl2_crash_fixture>
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/crash_report
        -P ${PROJECT_SOURCE_DIR}/test/scripts/expect_crash_report.cmake)
set_tests_properties(crash.report PROPERTIES
    LABELS "cli;crash")

add_test(NAME core.buffer COMMAND wl2_core_tests buffer)
add_test(NAME core.membus COMMAND wl2_core_tests membus)
add_test(NAME core.membus.shared_buffer COMMAND wl2_core_tests membus shared_buffer)
add_test(NAME core.membus.shared_queue COMMAND wl2_core_tests membus shared_queue)
add_test(NAME core.membus.video_buffer COMMAND wl2_core_tests membus video_buffer)
add_test(NAME core.membus.audio_buffer COMMAND wl2_core_tests membus audio_buffer)
add_test(NAME core.membus.command_channel COMMAND wl2_core_tests membus command_channel)
add_test(NAME core.membus.key_value_store COMMAND wl2_core_tests membus key_value_store)
add_test(NAME core.membus.selector COMMAND wl2_core_tests membus selector)
add_test(NAME core.resources COMMAND wl2_core_tests resources)
add_test(NAME core.thread_tree COMMAND wl2_core_tests thread_tree)
add_test(NAME core.runtime COMMAND wl2_core_tests runtime)
add_test(NAME core.async_host COMMAND wl2_core_tests async_host)
add_test(NAME core.async_integration COMMAND wl2_core_tests async_integration)
add_test(NAME core.module_requirements COMMAND wl2_core_tests module_requirements)
add_test(NAME core.dynamic_module COMMAND wl2_core_tests dynamic_module)
add_test(NAME core.module_store COMMAND wl2_core_tests module_store)
add_test(NAME core.module_deps COMMAND wl2_core_tests module_deps)
add_test(NAME core.module_resolver COMMAND wl2_core_tests module_resolver)
add_test(NAME core.app_store COMMAND wl2_core_tests app_store)
add_test(NAME core.script_thread COMMAND wl2_core_tests script_thread)
add_test(NAME core.script_thread.javascript_api COMMAND wl2_core_tests script_thread javascript_api)
add_test(NAME core.script_thread.force_close COMMAND wl2_core_tests script_thread force_close)
if(WL2_ENABLE_STRESS_TESTS)
    add_test(NAME core.script_thread.stress COMMAND wl2_core_tests script_thread stress)
endif()

set_tests_properties(core.buffer PROPERTIES
    LABELS "unit")
set_tests_properties(core.membus PROPERTIES
    LABELS "unit;membus")
set_tests_properties(
    core.membus.shared_buffer
    core.membus.shared_queue
    core.membus.video_buffer
    core.membus.audio_buffer
    core.membus.command_channel
    core.membus.key_value_store
    core.membus.selector
    PROPERTIES
        LABELS "unit;membus")
set_tests_properties(core.resources PROPERTIES
    LABELS "unit;resources")
set_tests_properties(core.thread_tree PROPERTIES
    LABELS "unit;thread")
set_tests_properties(core.runtime PROPERTIES
    LABELS "unit;runtime")
set_tests_properties(core.async_host PROPERTIES
    LABELS "unit;async")
set_tests_properties(core.async_integration PROPERTIES
    LABELS "unit;async"
    TIMEOUT 30)
set_tests_properties(core.module_requirements PROPERTIES
    LABELS "unit;runtime;modules")
set_tests_properties(core.dynamic_module PROPERTIES
    LABELS "unit;modules;dynamic")
set_tests_properties(core.module_store PROPERTIES
    LABELS "unit;modules")
set_tests_properties(core.module_deps PROPERTIES
    LABELS "unit;modules;git")
set_tests_properties(core.module_resolver PROPERTIES
    LABELS "unit;modules")
set_tests_properties(core.app_store PROPERTIES
    LABELS "unit;apps")
set_tests_properties(core.script_thread PROPERTIES
    LABELS "unit;thread"
    TIMEOUT 60)
set_tests_properties(core.script_thread.javascript_api PROPERTIES
    LABELS "unit;thread"
    TIMEOUT 30)
set_tests_properties(core.script_thread.force_close PROPERTIES
    LABELS "unit;thread"
    TIMEOUT 30)
if(WL2_ENABLE_STRESS_TESTS)
    set_tests_properties(core.script_thread.stress PROPERTIES
        LABELS "stress;thread"
        TIMEOUT 120)
endif()
