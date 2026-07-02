add_executable(wl2_media_tests wl2_media_tests.cpp)
target_link_libraries(wl2_media_tests PRIVATE wl2_core wl2_media_static)

add_test(NAME media.wl2_media COMMAND wl2_media_tests)
set_tests_properties(media.wl2_media PROPERTIES
    LABELS "media;js;unit"
    TIMEOUT 15)
