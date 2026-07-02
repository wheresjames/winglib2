include(cmake/options.cmake)

if(NOT WL2_ENABLE_GSTREAMER)
    return()
endif()

# GStreamer is discovered here (module-owned), not in the global build space.
# wl2_find_gstreamer() sets WL2_HAVE_GSTREAMER / WL2_GSTREAMER_TARGET in this
# scope and supports system, target-local, and cross-sysroot providers.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2GStreamer.cmake)
wl2_find_gstreamer()

if(NOT WL2_HAVE_GSTREAMER)
    message(STATUS "GStreamer not found; wl2_gstreamer module disabled")
    return()
endif()

wl2_add_module(wl2_gstreamer
    MODULE_NAME wl2:gstreamer
    REGISTER_FUNCTION wl2_gstreamer_register_module
    STABLE_ID bee7b81d-9fb6-48bf-81c9-6e519b85583c
    FEATURE WL2_ENABLE_GSTREAMER
    SOURCES
        src/wl2_gstreamer.cpp
        src/gstreamer/common.cpp
        src/gstreamer/pipeline.cpp
        src/gstreamer/bridges.cpp
        src/gstreamer/helpers.cpp
        src/gstreamer/advanced.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_GSTREAMER_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_GSTREAMER_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_GSTREAMER_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")

# Optional GStreamer feature libraries. Each is discovered with the same
# provider path as the core stack and adds a compile definition plus a link when
# present. None are required for the core pipeline runtime.
set(_wl2_gstreamer_features)
if(WL2_GSTREAMER_ENABLE_APP)
    list(APPEND _wl2_gstreamer_features "APP:gstreamer-app-1.0")
endif()
if(WL2_GSTREAMER_ENABLE_VIDEO)
    list(APPEND _wl2_gstreamer_features "VIDEO:gstreamer-video-1.0")
endif()
if(WL2_GSTREAMER_ENABLE_AUDIO)
    list(APPEND _wl2_gstreamer_features "AUDIO:gstreamer-audio-1.0")
endif()
if(WL2_GSTREAMER_ENABLE_PBUTILS)
    list(APPEND _wl2_gstreamer_features "PBUTILS:gstreamer-pbutils-1.0")
endif()

foreach(_wl2_gstreamer_feature IN LISTS _wl2_gstreamer_features)
    string(REPLACE ":" ";" _wl2_gstreamer_feature_parts "${_wl2_gstreamer_feature}")
    list(GET _wl2_gstreamer_feature_parts 0 _wl2_gstreamer_feature_name)
    list(GET _wl2_gstreamer_feature_parts 1 _wl2_gstreamer_feature_module)
    wl2_find_gstreamer_feature(WL2_GSTREAMER_${_wl2_gstreamer_feature_name} "${_wl2_gstreamer_feature_module}")
    if(WL2_GSTREAMER_${_wl2_gstreamer_feature_name}_FOUND)
        foreach(_wl2_gstreamer_target wl2_gstreamer_static wl2_gstreamer)
            if(TARGET ${_wl2_gstreamer_target})
                target_compile_definitions(${_wl2_gstreamer_target}
                    PRIVATE WL2_GSTREAMER_HAVE_${_wl2_gstreamer_feature_name}=1)
                target_link_libraries(${_wl2_gstreamer_target}
                    PRIVATE PkgConfig::WL2_GSTREAMER_${_wl2_gstreamer_feature_name})
            endif()
        endforeach()
        message(STATUS "wl2_gstreamer feature enabled: ${_wl2_gstreamer_feature_module}")
    else()
        message(STATUS "wl2_gstreamer feature not found: ${_wl2_gstreamer_feature_module}")
    endif()
endforeach()

wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2GStreamer.cmake"
    DESTINATION modules/wl2_gstreamer)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake"
    DESTINATION modules/wl2_gstreamer)
wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET PkgConfig::WL2_GSTREAMER_SYS)
    include(CMakeFindDependencyMacro)
    find_dependency(PkgConfig)
    pkg_check_modules(WL2_GSTREAMER_SYS REQUIRED IMPORTED_TARGET GLOBAL
        gstreamer-1.0
        gstreamer-base-1.0
        gobject-2.0
        glib-2.0)
endif()
]==])

if(TARGET wl2_gstreamer_static)
    wl2_add_module_tests(wl2_gstreamer)
endif()
