include(cmake/options.cmake)

if(NOT WL2_ENABLE_MEDIA)
    return()
endif()

# wl2:media is the backend-agnostic media schema module. It has no external
# dependencies: it is pure schema, validation, and timestamp-conversion code
# plus a thin QuickJS surface. Backend modules (wl2:gstreamer, wl2:ffmpeg) link
# the static target to reuse the C++ helpers and share one metadata wire format.
wl2_add_module(wl2_media
    MODULE_NAME wl2:media
    REGISTER_FUNCTION wl2_media_register_module
    STABLE_ID dbea8df8-c257-4e4d-9772-0cb3083ca87f
    FEATURE WL2_ENABLE_MEDIA
    SOURCES
        src/media_schema.cpp
        src/wl2_media.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_MEDIA_STATIC_MODULE=1)

if(TARGET wl2_media_static)
    wl2_add_module_tests(wl2_media)
endif()
