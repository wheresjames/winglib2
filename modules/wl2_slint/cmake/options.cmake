# Build option and dependency-provider knobs for the wl2_slint module. This file
# is included both from the module CMakeLists and (after installation) from the
# packaged WL2Slint.cmake, so it must be safe to include more than once and must
# not depend on in-tree-only state.
if(COMMAND wl2_module_option)
    # Default OFF: Slint is a heavy dependency (a Rust library shipped as a
    # prebuilt C++ binary package), so default builds and CI are unaffected until
    # it is explicitly enabled.
    wl2_module_option(ENABLE DEFAULT OFF DOC "Build wl2_slint UI module when Slint is available")
endif()

# Provider selection. The prefer-prebuilt model keeps default builds free of a
# Rust toolchain: local/package/fetch all consume Slint's published C++ binary
# package (an installed CMake package), while "source" (opt-in) builds from
# source via FetchContent and is the only provider that needs cargo/Rust.
set(WL2_SLINT_PROVIDER "fetch" CACHE STRING "slint provider: auto, local, package, fetch, source, or off")
set_property(CACHE WL2_SLINT_PROVIDER PROPERTY STRINGS auto local package fetch source off)
if(WL2_USE_FETCHED_DEPS)
    set(WL2_SLINT_PROVIDER "fetch" CACHE STRING "slint provider: auto, local, package, fetch, source, or off" FORCE)
endif()

set(WL2_SLINT_VERSION "1.8.0" CACHE STRING "Slint release version for the fetch/source providers")

# Prebuilt C++ binary package archive for the fetch provider. Slint publishes one
# archive per platform (slint-cpp-<ver>-<platform>); the default targets Linux
# x86_64. Override WL2_SLINT_URL / WL2_SLINT_URL_HASH for other platforms.
set(WL2_SLINT_URL
    "https://github.com/slint-ui/slint/releases/download/v${WL2_SLINT_VERSION}/slint-cpp-${WL2_SLINT_VERSION}-Linux-x86_64.tar.gz"
    CACHE STRING "Slint prebuilt C++ binary package URL for the fetch provider")
# Hash matches the default WL2_SLINT_URL (Linux x86_64). Override together with
# WL2_SLINT_URL when targeting another platform's package.
set(WL2_SLINT_URL_HASH "SHA256=3999bb654437720972f085946549a0ff865b5971784e3ad575b054b73b746f75"
    CACHE STRING "Slint prebuilt C++ binary package hash (e.g. SHA256=...) for the fetch provider")

# Git source for the opt-in source provider (requires cargo/Rust).
set(WL2_SLINT_GIT_REPOSITORY "https://github.com/slint-ui/slint.git"
    CACHE STRING "Slint source repository for the source provider")

if(WL2_DEPS_ROOT)
    set(WL2_SLINT_ROOT "${WL2_DEPS_ROOT}/slint" CACHE PATH "Path to a target-local Slint install")
    set(WL2_SLINT_NFD_ROOT "${WL2_DEPS_ROOT}/nativefiledialog-extended"
        CACHE PATH "Path to a target-local nativefiledialog-extended source/build")
endif()

option(WL2_SLINT_NATIVE_DIALOGS "Enable native file/folder dialogs in wl2_slint when supported" ON)
set(WL2_SLINT_NFD_PROVIDER "auto" CACHE STRING
    "nativefiledialog-extended provider for wl2_slint native dialogs: auto, local, package, fetch, or off")
set_property(CACHE WL2_SLINT_NFD_PROVIDER PROPERTY STRINGS auto local package fetch off)
if(WL2_USE_FETCHED_DEPS)
    set(WL2_SLINT_NFD_PROVIDER "fetch" CACHE STRING
        "nativefiledialog-extended provider for wl2_slint native dialogs: auto, local, package, fetch, or off" FORCE)
endif()
set(WL2_SLINT_NFD_GIT_REPOSITORY "https://github.com/btzy/nativefiledialog-extended.git"
    CACHE STRING "nativefiledialog-extended repository for the fetch provider")
set(WL2_SLINT_NFD_GIT_TAG "v1.2.1"
    CACHE STRING "nativefiledialog-extended git tag for the fetch provider")
