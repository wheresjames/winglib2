include(cmake/options.cmake)

if(NOT WL2_ENABLE_SDP)
    return()
endif()

# wl2:sdp is a dependency-free SDP (RFC 4566 / 8866) parser and builder. It owns
# a fidelity-preserving line model plus munging helpers and a thin QuickJS
# surface. Like wl2:media it pulls in no third-party libraries: the parser,
# builder, typed views, and JSON-free JS glue are all hand-rolled. Other modules
# (a future gstreamer-webrtc munger, tooling) link the static target to reuse the
# C++ helpers.
wl2_add_module(wl2_sdp
    MODULE_NAME wl2:sdp
    REGISTER_FUNCTION wl2_sdp_register_module
    STABLE_ID 808fd3a3-3f48-46fc-8767-438d4e5549aa
    FEATURE WL2_ENABLE_SDP
    SOURCES
        src/sdp.cpp
        src/wl2_sdp.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_SDP_STATIC_MODULE=1)

if(TARGET wl2_sdp_static)
    wl2_add_module_tests(wl2_sdp)
endif()
