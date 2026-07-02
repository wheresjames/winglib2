# Included from the module CMakeLists and (after install) from the packaged
# module config, where wl2_module_option is not defined; guard the call so
# external consumers can include it safely.
if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE
        DEFAULT ${WL2_ENABLE_EXTENDED_MODULES}
        DOC "Build the wl2:webrtc module (libdatachannel WebRTC/DataChannel)")
endif()

# Provider selection for libdatachannel. It is not distributed as a system
# package on most targets, so "auto" falls back to a pinned source archive when
# no target-local or packaged build is present. Select "off" to compile the
# module out for dependency-free builds.
set(WL2_WEBRTC_PROVIDER "auto" CACHE STRING "libdatachannel provider: auto, local, package, fetch, or off")
set_property(CACHE WL2_WEBRTC_PROVIDER PROPERTY STRINGS auto local package fetch off)
if(COMMAND wl2_dependency_configure_provider)
    wl2_dependency_configure_provider(WEBRTC WL2_WEBRTC_PROVIDER)
endif()

# Pinned libdatachannel release for the fetch provider. Git is used because the
# upstream generated source archives do not include the required dependency
# submodules; the URL/hash are kept for diagnostics and future vendored archives.
set(WL2_WEBRTC_VERSION "0.24.5" CACHE STRING "libdatachannel release tag for the fetch provider")
set(WL2_WEBRTC_GIT "https://github.com/paullouisageneau/libdatachannel.git" CACHE STRING "libdatachannel git repository")
set(WL2_WEBRTC_URL "https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v${WL2_WEBRTC_VERSION}.tar.gz" CACHE STRING "libdatachannel source archive URL")
set(WL2_WEBRTC_URL_HASH "SHA256=454537c3cd526bed935d847bb2dff4046f266eef84d43b2a5f2f2f293c0026f4" CACHE STRING "libdatachannel source archive hash for the fetch provider")
set(WL2_WEBRTC_TLS_BACKEND "openssl" CACHE STRING "libdatachannel TLS backend: openssl, gnutls, or mbedtls")

if(WL2_DEPS_ROOT)
    set(WL2_WEBRTC_ROOT "${WL2_DEPS_ROOT}/webrtc" CACHE PATH "Path to a target-local libdatachannel install")
endif()
