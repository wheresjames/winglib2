if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_resources_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
else()
    set(WL2_CORE_TARGET wl2_core)
endif()

add_executable(wl2_resources_example main.cpp)
target_link_libraries(wl2_resources_example PRIVATE ${WL2_CORE_TARGET})

wl2_add_embedded_resources(wl2_resources_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/files
    PREFIX wl2:/resources
    AUTO_DIRECTORIES assets
    EXCLUDE_PATTERNS "*.tmp"
    STORE_FILES config.json empty.txt
    COMPRESS_FILES repeated.txt)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.resources
        COMMAND wl2_resources_example)
    set_tests_properties(examples.resources PROPERTIES
        LABELS "examples;resources")
endif()
