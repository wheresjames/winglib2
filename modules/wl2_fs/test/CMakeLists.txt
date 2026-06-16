add_executable(wl2_fs_tests wl2_fs_tests.cpp)
target_link_libraries(wl2_fs_tests PRIVATE wl2_core wl2_fs_static)

add_test(NAME fs.wl2_fs COMMAND wl2_fs_tests)
set_tests_properties(fs.wl2_fs PROPERTIES
    LABELS "fs;js;integration"
    TIMEOUT 15)
