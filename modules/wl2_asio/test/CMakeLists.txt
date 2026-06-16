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
add_test(NAME scripts.wl2_asio_permission_denied
    COMMAND
        $<TARGET_FILE:wl2>
        run
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
