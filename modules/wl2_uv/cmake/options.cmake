if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE DOC "Build wl2_uv system and networking utilities module")
endif()

set(WL2_UV_PROVIDER "auto" CACHE STRING
    "libuv provider: auto, local, package, fetch, or off")
set_property(CACHE WL2_UV_PROVIDER PROPERTY STRINGS auto local package fetch off)
if(COMMAND wl2_dependency_configure_provider)
    wl2_dependency_configure_provider(UV WL2_UV_PROVIDER)
endif()

if(WL2_DEPS_ROOT)
    set(WL2_UV_ROOT "${WL2_DEPS_ROOT}/libuv" CACHE PATH "Path to a local libuv installation")
endif()

set(WL2_UV_VERSION "1.52.1" CACHE STRING "libuv release version for the fetch provider")
set(WL2_UV_URL
    "https://github.com/libuv/libuv/archive/refs/tags/v1.52.1.tar.gz"
    CACHE STRING "libuv source archive URL for the fetch provider")
set(WL2_UV_URL_HASH
    "SHA256=478baf2599bfbc882c355288c9cb6f92e0e7dda435fa04031fa5b607cf3f414c"
    CACHE STRING "libuv source archive hash for the fetch provider")
