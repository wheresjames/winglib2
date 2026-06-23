include(cmake/options.cmake)

if(NOT WL2_ENABLE_FFMPEG)
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2FFmpeg.cmake)
wl2_find_ffmpeg()

if(NOT WL2_HAVE_FFMPEG)
    message(STATUS "FFmpeg not found; wl2_ffmpeg module disabled")
    return()
endif()

wl2_add_module(wl2_ffmpeg
    MODULE_NAME wl2:ffmpeg
    REGISTER_FUNCTION wl2_ffmpeg_register_module
    STABLE_ID 5f03b3d4-2e2f-4c20-9ac5-fb2dd7ad0f6f
    FEATURE WL2_ENABLE_FFMPEG
    SOURCES src/wl2_ffmpeg.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_FFMPEG_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_FFMPEG_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_FFMPEG_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")

# Optional libavfilter-backed filter graph support. Never required for default
# CI; enable with -DWL2_FFMPEG_ENABLE_FILTERS=ON.
if(WL2_FFMPEG_ENABLE_FILTERS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WL2_FFMPEG_FILTERS QUIET IMPORTED_TARGET libavfilter)
    endif()
    if(WL2_FFMPEG_FILTERS_FOUND)
        foreach(_wl2_ffmpeg_target wl2_ffmpeg_static wl2_ffmpeg)
            if(TARGET ${_wl2_ffmpeg_target})
                target_compile_definitions(${_wl2_ffmpeg_target} PRIVATE WL2_FFMPEG_HAVE_FILTERS=1)
                target_link_libraries(${_wl2_ffmpeg_target} PRIVATE PkgConfig::WL2_FFMPEG_FILTERS)
            endif()
        endforeach()
        message(STATUS "wl2_ffmpeg filter graph support enabled (libavfilter)")
    else()
        message(STATUS "libavfilter not found; wl2_ffmpeg filter graph support disabled")
    endif()
endif()

# Optional pinned external-media download target. No network access by default.
if(WL2_FFMPEG_DOWNLOAD_TEST_MEDIA)
    add_custom_target(wl2_ffmpeg_fetch_test_media
        COMMAND ${CMAKE_COMMAND}
            -DWL2_FFMPEG_TEST_MEDIA_ROOT=${WL2_FFMPEG_TEST_MEDIA_ROOT}
            -DWL2_FFMPEG_TEST_MEDIA_PROFILE=${WL2_FFMPEG_TEST_MEDIA_PROFILE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/fetch-media.cmake
        COMMENT "Fetching pinned external wl2_ffmpeg test media (SHA-256 verified)")
endif()

wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2FFmpeg.cmake"
    DESTINATION modules/wl2_ffmpeg)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake"
    DESTINATION modules/wl2_ffmpeg)
wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET PkgConfig::WL2_FFMPEG)
    include(CMakeFindDependencyMacro)
    find_dependency(PkgConfig)
    pkg_check_modules(WL2_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample)
endif()
]==])

if(TARGET wl2_ffmpeg_static)
    if(WL2_BUILD_EXAMPLES
            AND WL2_ENABLE_SLINT
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/replay-lab/CMakeLists.txt")
        add_subdirectory(examples/replay-lab)
    endif()
    wl2_add_module_tests(wl2_ffmpeg)
    if(TARGET wl2_ffmpeg_tests)
        target_compile_definitions(wl2_ffmpeg_tests PRIVATE
            WL2_FFMPEG_TEST_MEDIA_ROOT="${WL2_FFMPEG_TEST_MEDIA_ROOT}")
    endif()
endif()
