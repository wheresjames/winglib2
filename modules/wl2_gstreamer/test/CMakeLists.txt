add_executable(wl2_gstreamer_tests wl2_gstreamer_tests.cpp)
target_link_libraries(wl2_gstreamer_tests PRIVATE wl2_core wl2_gstreamer_static)
# wl2:membus provides an in-process reader for inspecting the rings the bridges
# publish into. Linked only when the module is enabled in this build.
if(TARGET wl2_membus_static)
    target_link_libraries(wl2_gstreamer_tests PRIVATE wl2_membus_static)
    target_compile_definitions(wl2_gstreamer_tests PRIVATE WL2_GSTREAMER_TEST_HAVE_MEMBUS=1)
endif()

# Deterministic and headless: synthetic sources only (videotestsrc/fakesink),
# no devices, no network, no display.
add_test(NAME gstreamer.wl2_gstreamer COMMAND wl2_gstreamer_tests)
set_tests_properties(gstreamer.wl2_gstreamer PROPERTIES
    LABELS "gstreamer;js;integration"
    TIMEOUT 30)
