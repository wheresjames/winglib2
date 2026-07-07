add_executable(wl2_asio_fixture
    wl2_asio_fixture.cpp)

target_compile_features(wl2_asio_fixture PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(wl2_asio_fixture PRIVATE Threads::Threads)

# Loopback echo round-trip: connect / write / read / close through wl2:asio,
# with network access granted only for the fixture endpoint.
add_test(NAME scripts.wl2_asio_tcp_echo
    COMMAND
        wl2_asio_fixture
        $<TARGET_FILE:wl2>
        ${CMAKE_CURRENT_SOURCE_DIR}/js/tcp_echo.test.js)
set_tests_properties(scripts.wl2_asio_tcp_echo PROPERTIES
    LABELS "js;asio;integration;smoke"
    TIMEOUT 30)

# Default-denied policy: no grant is passed, so connect() must be denied.
# --no-permission-prompt forces the non-interactive path so the denial is
# deterministic; without it, running under an interactive terminal makes the
# runtime prompt on stdin and the test hangs waiting for input.
add_test(NAME scripts.wl2_asio_permission_denied
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/permission_denied.test.js)
set_tests_properties(scripts.wl2_asio_permission_denied PROPERTIES
    LABELS "js;asio;integration"
    TIMEOUT 15)

# Server round trip: listen / accept / address / close plus a loopback client,
# all in one process. Listening and loopback connects are granted explicitly.
add_test(NAME scripts.wl2_asio_tcp_server
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow-listen --listen-allow 127.0.0.1
        --network-allow 127.0.0.1
        ${CMAKE_CURRENT_SOURCE_DIR}/js/tcp_server.test.js)
set_tests_properties(scripts.wl2_asio_tcp_server PROPERTIES
    LABELS "js;asio;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_asio_timers
    COMMAND
        $<TARGET_FILE:wl2>
        run
        ${CMAKE_CURRENT_SOURCE_DIR}/js/timers.test.js)
set_tests_properties(scripts.wl2_asio_timers PROPERTIES
    LABELS "js;asio;integration;timers"
    TIMEOUT 15)

# Single-file permission declaration: the leading `/* wl2 ... */` block defines
# a loopback envelope that runtime requestPermissions() can turn into grants.
add_test(NAME scripts.wl2_asio_declared_permissions
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow-declared
        ${CMAKE_CURRENT_SOURCE_DIR}/js/declared_permissions.test.js)
set_tests_properties(scripts.wl2_asio_declared_permissions PROPERTIES
    LABELS "js;asio;integration;permissions"
    TIMEOUT 30)

# The same script must not silently self-authorize in non-interactive mode.
add_test(NAME scripts.wl2_asio_declared_permissions_denied
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/declared_permissions.test.js)
set_tests_properties(scripts.wl2_asio_declared_permissions_denied PROPERTIES
    LABELS "js;asio;permissions"
    WILL_FAIL TRUE
    TIMEOUT 15)

add_test(NAME scripts.wl2_asio_declared_permissions_trusted
    COMMAND
        ${CMAKE_COMMAND}
        -DWL2_EXECUTABLE=$<TARGET_FILE:wl2>
        -DSOURCE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/js/declared_permissions.test.js
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/declared_permissions_trusted
        -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_declared_trust.cmake)
set_tests_properties(scripts.wl2_asio_declared_permissions_trusted PROPERTIES
    LABELS "js;asio;permissions"
    TIMEOUT 30)

# When the dynamic module is built, validate that the shared target exposes the
# same identity/metadata as the static build through the runner's loader.
if(TARGET wl2_asio)
    add_test(NAME scripts.wl2_asio_module_validate
        COMMAND $<TARGET_FILE:wl2> module validate $<TARGET_FILE:wl2_asio>)
    set_tests_properties(scripts.wl2_asio_module_validate PROPERTIES
        LABELS "asio;modules;dynamic"
        PASS_REGULAR_EXPRESSION "module: wl2:asio"
        TIMEOUT 15)
endif()
