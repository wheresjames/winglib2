# Native headless test: compile, instantiate, property round-trip, compile-error
# diagnostics, and show()-denied-by-default. No window is opened, so this runs in
# CI without a display.
add_executable(wl2_slint_tests wl2_slint_tests.cpp)
target_link_libraries(wl2_slint_tests PRIVATE wl2_core wl2_slint_static)
target_compile_definitions(wl2_slint_tests PRIVATE
    WL2_SLINT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME slint.wl2_slint COMMAND wl2_slint_tests)
set_tests_properties(slint.wl2_slint PROPERTIES
    LABELS "slint;js;unit;integration"
    TIMEOUT 30)

# End-to-end through the runner: compile/create/get/set headlessly, no capability.
add_test(NAME scripts.wl2_slint_compile_properties
    COMMAND
        $<TARGET_FILE:wl2>
        run
        ${CMAKE_CURRENT_SOURCE_DIR}/js/compile_properties.test.js)
set_tests_properties(scripts.wl2_slint_compile_properties PROPERTIES
    LABELS "js;slint;integration;smoke"
    TIMEOUT 30)

# compileFile() from a wl2:/ resource (mounted with --map-resource), including
# include-path resolution for the component's import. Headless.
add_test(NAME scripts.wl2_slint_compile_file
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --map-resource ${CMAKE_CURRENT_SOURCE_DIR}/fixtures:wl2:/fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/js/compile_file.test.js)
set_tests_properties(scripts.wl2_slint_compile_file PROPERTIES
    LABELS "js;slint;integration"
    TIMEOUT 30)

# End-to-end through the runner: on()/invoke() callback marshaling, headless.
add_test(NAME scripts.wl2_slint_callbacks
    COMMAND
        $<TARGET_FILE:wl2>
        run
        ${CMAKE_CURRENT_SOURCE_DIR}/js/callbacks.test.js)
set_tests_properties(scripts.wl2_slint_callbacks PROPERTIES
    LABELS "js;slint;integration;smoke"
    TIMEOUT 30)

# Default-denied UI policy: no --allow-ui is passed, so show() must be denied.
add_test(NAME scripts.wl2_slint_permission_denied
    COMMAND
        $<TARGET_FILE:wl2>
        run
        ${CMAKE_CURRENT_SOURCE_DIR}/js/permission_denied.test.js)
set_tests_properties(scripts.wl2_slint_permission_denied PROPERTIES
    LABELS "js;slint;integration"
    TIMEOUT 15)

# Opt-in windowed smoke test: needs a real display/backend, so it is registered
# only when WL2_SLINT_DISPLAY_TESTS is ON and labeled "display" for exclusion
# (like "external-network" for wl2:asio). It opens a window, runs the event loop,
# and quits from a Slint-timer-driven callback.
option(WL2_SLINT_DISPLAY_TESTS "Register wl2_slint windowed tests that need a real display" OFF)
if(WL2_SLINT_DISPLAY_TESTS)
    add_test(NAME scripts.wl2_slint_windowed_smoke
        COMMAND
            $<TARGET_FILE:wl2>
            run --allow-ui
            ${CMAKE_CURRENT_SOURCE_DIR}/js/windowed_smoke.test.js)
    set_tests_properties(scripts.wl2_slint_windowed_smoke PROPERTIES
        LABELS "js;slint;display;smoke"
        TIMEOUT 30)
endif()

# When the dynamic module is built, validate that the shared target exposes the
# same identity/metadata as the static build through the runner's loader.
if(TARGET wl2_slint)
    add_test(NAME scripts.wl2_slint_module_validate
        COMMAND $<TARGET_FILE:wl2> module validate $<TARGET_FILE:wl2_slint>)
    set_tests_properties(scripts.wl2_slint_module_validate PROPERTIES
        LABELS "slint;modules;dynamic"
        PASS_REGULAR_EXPRESSION "module: wl2:slint"
        TIMEOUT 15)
endif()
