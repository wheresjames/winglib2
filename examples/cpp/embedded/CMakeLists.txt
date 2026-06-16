if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_embedded_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
else()
    set(WL2_CORE_TARGET wl2_core)
endif()

add_executable(wl2_embedded_example main.cpp)
target_link_libraries(wl2_embedded_example PRIVATE ${WL2_CORE_TARGET})
wl2_add_embedded_resources(wl2_embedded_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/scripts
    PREFIX wl2:/app
    FILES main.js)
