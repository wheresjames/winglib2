add_executable(wl2_curl_fixture
    wl2_curl_fixture.cpp)

target_compile_features(wl2_curl_fixture PRIVATE cxx_std_20)

add_test(NAME scripts.wl2_curl_smoke
    COMMAND
        wl2_curl_fixture
        $<TARGET_FILE:wl2>
        ${CMAKE_CURRENT_SOURCE_DIR}/js/wl2_curl_smoke.js)
set_tests_properties(scripts.wl2_curl_smoke PROPERTIES
    LABELS "js;curl;integration;smoke"
    TIMEOUT 15)
