include(cmake/options.cmake)

function(_wl2_webrtc_add_skip_test reason)
    if(WL2_BUILD_TESTING)
        add_test(NAME webrtc.wl2_webrtc
            COMMAND ${CMAKE_COMMAND} -E echo "SKIP: ${reason}")
        set_tests_properties(webrtc.wl2_webrtc PROPERTIES
            LABELS "webrtc;js;integration"
            SKIP_REGULAR_EXPRESSION "^SKIP:")
    endif()
endfunction()

if(NOT WL2_ENABLE_WEBRTC)
    _wl2_webrtc_add_skip_test("wl2:webrtc is disabled")
    return()
endif()

# libdatachannel is only used by this module, so it is discovered here rather
# than in the global build space. wl2_find_webrtc() sets WL2_HAVE_WEBRTC /
# WL2_WEBRTC_TARGET in this scope.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2WebRtc.cmake)
wl2_find_webrtc()

if(NOT WL2_HAVE_WEBRTC)
    message(STATUS "libdatachannel not available; wl2_webrtc module disabled "
        "(set WL2_WEBRTC_PROVIDER=fetch to build it from source)")
    _wl2_webrtc_add_skip_test("libdatachannel is not available")
    return()
endif()

set(_wl2_webrtc_install_args)
if(WL2_WEBRTC_PROVIDER_USED STREQUAL "fetch")
    # FetchContent creates regular in-tree libdatachannel/subdependency targets
    # that are not part of Winglib2's install export. Keep the module buildable
    # and testable, but avoid exporting an incomplete package graph.
    list(APPEND _wl2_webrtc_install_args NO_INSTALL)
endif()

wl2_add_module(wl2_webrtc
    ${_wl2_webrtc_install_args}
    MODULE_NAME wl2:webrtc
    REGISTER_FUNCTION wl2_webrtc_register_module
    STABLE_ID 4d8f2a17-6c3e-4b90-9e21-0f5a7c9d1b44
    FEATURE WL2_ENABLE_WEBRTC
    SOURCES
        src/wl2_webrtc.cpp
        src/webrtc/peer_connection.cpp
        src/webrtc/signaling.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_WEBRTC_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_WEBRTC_TARGET}
    PRIVATE_COMPILE_DEFINITIONS
        WL2_WEBRTC_PROVIDER_USED="${WL2_WEBRTC_PROVIDER_USED}"
    STATIC_PRIVATE_COMPILE_DEFINITIONS
        WL2_WEBRTC_STATIC_MODULE=1
        WL2_WEBRTC_LIBVERSION="${WL2_WEBRTC_VERSION}"
        WL2_WEBRTC_TLS_BACKEND="${WL2_WEBRTC_TLS_BACKEND}"
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS
        WL2_VERSION="${PROJECT_VERSION}"
        WL2_WEBRTC_LIBVERSION="${WL2_WEBRTC_VERSION}"
        WL2_WEBRTC_TLS_BACKEND="${WL2_WEBRTC_TLS_BACKEND}")

if(WL2_WEBRTC_PROVIDER_USED STREQUAL "fetch" AND TARGET wl2_webrtc_static)
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_TARGETS wl2_webrtc_static)
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_REGISTRATION_FUNCTIONS wl2_webrtc_register_module)
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_NAMES wl2:webrtc)
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_REQUIRES "-")
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_OPTIONAL "-")
endif()

if(TARGET wl2_webrtc_static)
    wl2_add_module_tests(wl2_webrtc)
endif()
