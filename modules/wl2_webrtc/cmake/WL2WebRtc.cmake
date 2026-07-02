# Resolve libdatachannel for the wl2:webrtc module. Sets, in the caller's scope:
#   WL2_HAVE_WEBRTC   - TRUE when a usable libdatachannel was found/built
#   WL2_WEBRTC_TARGET - the target to link (an imported or in-tree static target)
#   WL2_WEBRTC_PROVIDER_USED - which provider satisfied the request
#
# Provider order for "auto": local/package (prebuilt) first, then fetch (source
# build from a pinned tag). "off" disables the module. libdatachannel is not
# packaged for most distributions, so "fetch" is the realistic path and builds it
# from source with libjuice (ICE), usrsctp (SCTP), and libsrtp (media) against
# the selected TLS backend.

include_guard(GLOBAL)

function(_wl2_webrtc_try_prebuilt out_found out_target)
    set(${out_found} FALSE PARENT_SCOPE)
    set(_hints "")
    if(WL2_WEBRTC_ROOT)
        set(_hints "${WL2_WEBRTC_ROOT}")
    endif()
    find_package(LibDataChannel CONFIG QUIET HINTS ${_hints})
    if(LibDataChannel_FOUND)
        if(TARGET LibDataChannel::LibDataChannelStatic)
            set(${out_target} LibDataChannel::LibDataChannelStatic PARENT_SCOPE)
        else()
            set(${out_target} LibDataChannel::LibDataChannel PARENT_SCOPE)
        endif()
        set(${out_found} TRUE PARENT_SCOPE)
    endif()
endfunction()

function(_wl2_webrtc_fetch out_target)
    include(FetchContent)
    # Configure libdatachannel's build: static, no examples/tests, OpenSSL TLS
    # (not GnuTLS/mbedTLS), and its bundled libjuice for ICE (not libnice).
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(PLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NO_MEDIA OFF CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(USE_NICE OFF CACHE BOOL "" FORCE)
    if(WL2_WEBRTC_TLS_BACKEND STREQUAL "gnutls")
        set(USE_GNUTLS ON CACHE BOOL "" FORCE)
    elseif(WL2_WEBRTC_TLS_BACKEND STREQUAL "mbedtls")
        set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
    else()
        set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
        set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
    endif()
    set(BUILD_SHARED_LIBS OFF)

    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY "${WL2_WEBRTC_GIT}"
        GIT_TAG "v${WL2_WEBRTC_VERSION}"
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(libdatachannel)

    # libdatachannel's bundled usrsctp and libsrtp emit their static archives into
    # config-suffixed directories (e.g. build/DEBUG/lib), which do not match the
    # single lib dir the rest of the build (and the propagated link line) expect.
    # Pin every fetched static target — including per-config output dirs — to the
    # common lib directory so consumers in other build subdirectories can link
    # them by path.
    foreach(_wl2_dc_target datachannel-static usrsctp srtp2 juice-static)
        if(TARGET ${_wl2_dc_target})
            set_target_properties(${_wl2_dc_target} PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
                ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/lib"
                ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
                ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/lib"
                ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/lib"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
        endif()
    endforeach()

    if(TARGET LibDataChannel::LibDataChannelStatic)
        set(${out_target} LibDataChannel::LibDataChannelStatic PARENT_SCOPE)
    elseif(TARGET datachannel-static)
        set(${out_target} datachannel-static PARENT_SCOPE)
    else()
        set(${out_target} "" PARENT_SCOPE)
    endif()
endfunction()

function(wl2_find_webrtc)
    set(WL2_HAVE_WEBRTC FALSE PARENT_SCOPE)
    set(WL2_WEBRTC_TARGET "" PARENT_SCOPE)

    if(WL2_WEBRTC_PROVIDER STREQUAL "off")
        message(STATUS "wl2:webrtc provider is off; module disabled")
        if(COMMAND wl2_dependency_note_result)
            wl2_dependency_note_result(webrtc DISABLED "provider off")
        endif()
        return()
    endif()

    if(WL2_WEBRTC_PROVIDER STREQUAL "local" OR WL2_WEBRTC_PROVIDER STREQUAL "package"
       OR WL2_WEBRTC_PROVIDER STREQUAL "auto")
        _wl2_webrtc_try_prebuilt(_found _target)
        if(_found)
            set(WL2_HAVE_WEBRTC TRUE PARENT_SCOPE)
            set(WL2_WEBRTC_TARGET "${_target}" PARENT_SCOPE)
            set(WL2_WEBRTC_PROVIDER_USED "package" PARENT_SCOPE)
            if(COMMAND wl2_dependency_note_result)
                wl2_dependency_note_result(webrtc package "${_target}")
            endif()
            message(STATUS "Using prebuilt libdatachannel (${_target})")
            return()
        endif()
        if(WL2_WEBRTC_PROVIDER STREQUAL "local" OR WL2_WEBRTC_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_WEBRTC_PROVIDER=${WL2_WEBRTC_PROVIDER} but libdatachannel was not found")
        endif()
    endif()

    if(WL2_WEBRTC_PROVIDER STREQUAL "fetch" OR WL2_WEBRTC_PROVIDER STREQUAL "auto")
        _wl2_webrtc_fetch(_target)
        if(_target)
            set(WL2_HAVE_WEBRTC TRUE PARENT_SCOPE)
            set(WL2_WEBRTC_TARGET "${_target}" PARENT_SCOPE)
            set(WL2_WEBRTC_PROVIDER_USED "fetch" PARENT_SCOPE)
            if(COMMAND wl2_dependency_note_result)
                wl2_dependency_note_result(webrtc download "${WL2_WEBRTC_VERSION} ${WL2_WEBRTC_URL}")
            endif()
            message(STATUS "Using fetched libdatachannel ${WL2_WEBRTC_VERSION} (${_target})")
            return()
        endif()
        message(FATAL_ERROR "libdatachannel fetch did not produce a usable target")
    endif()
endfunction()
