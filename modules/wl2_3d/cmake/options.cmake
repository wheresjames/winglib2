wl2_module_option(ENABLE DEFAULT ON DOC "Build wl2_3d 3D UI module foundation")

set(WL2_3D_PROVIDER "auto" CACHE STRING "3D renderer provider: auto, local, package, fetch, or off")
set_property(CACHE WL2_3D_PROVIDER PROPERTY STRINGS auto local package fetch off)
if(COMMAND wl2_dependency_configure_provider)
    wl2_dependency_configure_provider(MAGNUM WL2_3D_PROVIDER)
endif()

set(WL2_3D_ASSIMP OFF CACHE BOOL "Enable optional Assimp importer support for wl2_3d")
set(WL2_3D_VULKAN OFF CACHE BOOL "Enable experimental Vulkan support for wl2_3d")
set(WL2_3D_DEFAULT_WIDTH 1280 CACHE STRING "Default wl2_3d frame width")
set(WL2_3D_DEFAULT_HEIGHT 720 CACHE STRING "Default wl2_3d frame height")
set(WL2_3D_DEFAULT_BUFFERS 3 CACHE STRING "Default wl2_3d frame ring buffer count")
set(WL2_3D_GPU_SAMPLES 4 CACHE STRING "MSAA sample count for the wl2_3d GPU renderer; use 1 to disable MSAA")
set(WL2_MAGNUM_VERSION "2020.06" CACHE STRING "Magnum/Corrade release version for the download provider")
set(WL2_CORRADE_URL
    "https://github.com/mosra/corrade/archive/refs/tags/v${WL2_MAGNUM_VERSION}.tar.gz"
    CACHE STRING "Corrade source archive URL for the download provider")
set(WL2_MAGNUM_URL
    "https://github.com/mosra/magnum/archive/refs/tags/v${WL2_MAGNUM_VERSION}.tar.gz"
    CACHE STRING "Magnum source archive URL for the download provider")
set(WL2_CORRADE_URL_HASH "" CACHE STRING "Corrade source archive hash for the download provider")
set(WL2_MAGNUM_URL_HASH "" CACHE STRING "Magnum source archive hash for the download provider")
