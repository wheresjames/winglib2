add_executable(wl2_ffmpeg_tests wl2_ffmpeg_tests.cpp)
target_link_libraries(wl2_ffmpeg_tests PRIVATE wl2_core wl2_ffmpeg_static)

add_test(NAME ffmpeg.wl2_ffmpeg COMMAND wl2_ffmpeg_tests)
set_tests_properties(ffmpeg.wl2_ffmpeg PROPERTIES
    LABELS "ffmpeg;js;integration;smoke"
    TIMEOUT 20)
