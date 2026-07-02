add_executable(wl2_sdp_tests wl2_sdp_tests.cpp)
target_link_libraries(wl2_sdp_tests PRIVATE wl2_core wl2_sdp_static)
target_compile_definitions(wl2_sdp_tests PRIVATE
    WL2_SDP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME sdp.wl2_sdp COMMAND wl2_sdp_tests)
set_tests_properties(sdp.wl2_sdp PROPERTIES
    LABELS "sdp;js;unit"
    TIMEOUT 15)

# Optional libFuzzer target for the parser (Clang only, opt-in). The default
# build never compiles it; enable with -DWL2_SDP_FUZZ=ON under a Clang toolchain.
if(WL2_SDP_FUZZ)
    add_executable(wl2_sdp_fuzz fuzz/sdp_fuzz.cpp)
    target_link_libraries(wl2_sdp_fuzz PRIVATE wl2_sdp_static)
    target_compile_options(wl2_sdp_fuzz PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined)
    target_link_options(wl2_sdp_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
