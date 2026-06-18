# Build option and dependency-provider knobs for the wl2_asio module. This file
# is included both from the module CMakeLists and (after installation) from the
# packaged WL2Asio.cmake, so it must be safe to include more than once and must
# not depend on in-tree-only state.
if(COMMAND wl2_module_option)
    # Default OFF until the standalone-Asio dependency fetch and the JavaScript
    # API stabilize, so default builds and CI are unaffected.
    wl2_module_option(ENABLE DEFAULT OFF DOC "Build wl2_asio networking module when standalone Asio is available")
endif()

set(WL2_ASIO_PROVIDER "auto" CACHE STRING "asio provider: auto, local, package, fetch, or off")
set_property(CACHE WL2_ASIO_PROVIDER PROPERTY STRINGS auto local package fetch off)
if(WL2_USE_FETCHED_DEPS)
    set(WL2_ASIO_PROVIDER "fetch" CACHE STRING "asio provider: auto, local, package, fetch, or off" FORCE)
endif()
set(WL2_ASIO_VERSION "1.30.2" CACHE STRING "standalone Asio release version for the fetch provider")
set(WL2_ASIO_URL
    "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz"
    CACHE STRING "standalone Asio source archive URL for the fetch provider")
set(WL2_ASIO_URL_HASH
    "SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8"
    CACHE STRING "standalone Asio source archive hash for the fetch provider")

if(WL2_DEPS_ROOT)
    set(WL2_ASIO_ROOT "${WL2_DEPS_ROOT}/asio" CACHE PATH "Path to a target-local standalone Asio checkout")
endif()
