# wl2:http tests. The server round-trip runs the server in a wl2 subprocess and
# drives it from a native out-of-process HTTP client (the wl2:curl client blocks
# the JS thread, so an in-process self-request cannot be used). The
# permission-denied test is a standalone script (no request), run by the runner.

add_executable(wl2_restinio_fixture wl2_restinio_fixture.cpp)
target_compile_features(wl2_restinio_fixture PRIVATE cxx_std_20)

# Server round trip: listen / route / :params / query / body / 404, driven by a
# raw-socket client against the server running in a wl2 subprocess. Listening is
# granted only for loopback.
add_test(NAME scripts.wl2_http_server
    COMMAND
        wl2_restinio_fixture
        $<TARGET_FILE:wl2>
        ${CMAKE_CURRENT_SOURCE_DIR}/js/http_server_listen.js
        ${CMAKE_CURRENT_SOURCE_DIR}/data)
set_tests_properties(scripts.wl2_http_server PROPERTIES
    LABELS "js;http;integration;smoke"
    TIMEOUT 30)

# Default-denied policy: no listen grant, so listen() must be denied.
# --no-permission-prompt forces the non-interactive path so the denial is
# deterministic instead of prompting on stdin.
add_test(NAME scripts.wl2_http_permission_denied
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/http_permission_denied.test.js)
set_tests_properties(scripts.wl2_http_permission_denied PROPERTIES
    LABELS "js;http;integration"
    TIMEOUT 15)

# HTTPS round trip (only when TLS is built, i.e. OpenSSL is available). Runs the
# TLS server in a wl2 subprocess and drives it with an OpenSSL client.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND AND WL2_HTTP_TLS)
    add_executable(wl2_restinio_tls_fixture wl2_restinio_tls_fixture.cpp)
    target_compile_features(wl2_restinio_tls_fixture PRIVATE cxx_std_20)
    target_link_libraries(wl2_restinio_tls_fixture PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME scripts.wl2_https_server
        COMMAND
            wl2_restinio_tls_fixture
            $<TARGET_FILE:wl2>
            ${CMAKE_CURRENT_SOURCE_DIR}/js/https_server_listen.js
            ${CMAKE_CURRENT_SOURCE_DIR}/data/cert.pem
            ${CMAKE_CURRENT_SOURCE_DIR}/data/key.pem)
    set_tests_properties(scripts.wl2_https_server PROPERTIES
        LABELS "js;http;integration;tls;smoke"
        TIMEOUT 30)
endif()

# When the dynamic module is built, validate the shared target's identity.
if(TARGET wl2_restinio)
    add_test(NAME scripts.wl2_http_module_validate
        COMMAND $<TARGET_FILE:wl2> module validate $<TARGET_FILE:wl2_restinio>)
    set_tests_properties(scripts.wl2_http_module_validate PROPERTIES
        LABELS "http;modules;dynamic"
        PASS_REGULAR_EXPRESSION "module: wl2:http"
        TIMEOUT 15)
endif()
