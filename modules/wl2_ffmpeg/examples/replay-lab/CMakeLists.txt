if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_ffmpeg_replay_lab_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
    set(WL2_FFMPEG_STATIC_TARGET winglib2::wl2_ffmpeg_static)
    set(WL2_SLINT_STATIC_TARGET winglib2::wl2_slint_static)
else()
    set(WL2_CORE_TARGET wl2_core)
    set(WL2_FFMPEG_STATIC_TARGET wl2_ffmpeg_static)
    set(WL2_SLINT_STATIC_TARGET wl2_slint_static)
endif()

add_executable(wl2_ffmpeg_replay_lab_example main.cpp)
target_link_libraries(wl2_ffmpeg_replay_lab_example PRIVATE
    ${WL2_CORE_TARGET}
    ${WL2_FFMPEG_STATIC_TARGET}
    ${WL2_SLINT_STATIC_TARGET})

wl2_add_embedded_resources(wl2_ffmpeg_replay_lab_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    PREFIX wl2:/ffmpeg-replay-lab
    STORE_FILES scripts/main.js ui/app.slint fixtures/README.md)
