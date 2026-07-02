# GStreamer is used only by the wl2_gstreamer module, which includes this file
# and calls wl2_find_gstreamer() from its own scope. The guard keeps the helper
# definitions idempotent if the file is included more than once.
include_guard(GLOBAL)

if(DEFINED PROJECT_SOURCE_DIR AND EXISTS "${PROJECT_SOURCE_DIR}/cmake/deps/WL2Dependency.cmake")
    include("${PROJECT_SOURCE_DIR}/cmake/deps/WL2Dependency.cmake")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/../../deps/WL2Dependency.cmake")
endif()

# Discover the GStreamer 1.0 core stack through pkg-config into the given prefix.
# The IMPORTED_TARGET is promoted to GLOBAL so the module targets can link it
# regardless of the directory scope the check ran in.
function(_wl2_gstreamer_pkgcheck prefix)
    pkg_check_modules(${prefix} QUIET IMPORTED_TARGET GLOBAL
        gstreamer-1.0>=1.16
        gstreamer-base-1.0
        gobject-2.0
        glib-2.0)
    set(${prefix}_FOUND "${${prefix}_FOUND}" PARENT_SCOPE)
    set(${prefix}_VERSION "${${prefix}_VERSION}" PARENT_SCOPE)
endfunction()

function(_wl2_resolve_gstreamer out_found out_target out_is_system out_provider_used)
    find_package(PkgConfig QUIET)
    if(NOT PkgConfig_FOUND)
        message(STATUS "pkg-config not found; cannot discover GStreamer for wl2_gstreamer")
        return()
    endif()

    # local / auto: prefer a target-local root, e.g. a cross-sysroot staging
    # install pointed at by WL2_GSTREAMER_ROOT. Point pkg-config at that root.
    if(WL2_GSTREAMER_PROVIDER STREQUAL "local" OR WL2_GSTREAMER_PROVIDER STREQUAL "auto")
        if(WL2_GSTREAMER_ROOT AND EXISTS "${WL2_GSTREAMER_ROOT}")
            set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
            set(ENV{PKG_CONFIG_PATH}
                "${WL2_GSTREAMER_ROOT}/lib/pkgconfig:${WL2_GSTREAMER_ROOT}/lib64/pkgconfig:${WL2_GSTREAMER_ROOT}/share/pkgconfig:${_saved_pkg_config_path}")
            _wl2_gstreamer_pkgcheck(WL2_GSTREAMER_LOCAL)
            set(ENV{PKG_CONFIG_PATH} "${_saved_pkg_config_path}")
            if(WL2_GSTREAMER_LOCAL_FOUND)
                set(${out_found} TRUE PARENT_SCOPE)
                set(${out_target} PkgConfig::WL2_GSTREAMER_LOCAL PARENT_SCOPE)
                set(${out_is_system} FALSE PARENT_SCOPE)
                set(${out_provider_used} local PARENT_SCOPE)
                wl2_dependency_note_result(gstreamer local "${WL2_GSTREAMER_ROOT} (${WL2_GSTREAMER_LOCAL_VERSION})")
                message(STATUS "Using local GStreamer ${WL2_GSTREAMER_LOCAL_VERSION} from ${WL2_GSTREAMER_ROOT}")
                return()
            endif()
        endif()
        if(WL2_GSTREAMER_PROVIDER STREQUAL "local")
            message(FATAL_ERROR
                "WL2_GSTREAMER_PROVIDER=local but GStreamer was not found at ${WL2_GSTREAMER_ROOT}")
        endif()
    endif()

    # fetch/download: GStreamer is not built from source (large, Meson-based), so
    # there is nothing to fetch. Rather than fail the build, fall back to system
    # discovery so a `download`-defaulted tree still configures. A dedicated
    # target-local install is available through WL2_DEPS_GSTREAMER=local.
    if(WL2_GSTREAMER_PROVIDER STREQUAL "fetch")
        message(STATUS
            "GStreamer is not built from source; ignoring download provider and trying system pkg-config "
            "(use WL2_DEPS_GSTREAMER=local with -DWL2_GSTREAMER_ROOT=<prefix> for a target-local install)")
    endif()

    # package/system / auto / fetch-fallback: discover through the ambient
    # pkg-config. For a cross build this is the toolchain-configured sysroot
    # pkg-config. note_package is called only once GStreamer is actually found so
    # an absent optional dependency never warns; while cross-compiling it refuses
    # an implicit system dependency unless WL2_DEPS_GSTREAMER=system was explicit.
    if(WL2_GSTREAMER_PROVIDER STREQUAL "package"
            OR WL2_GSTREAMER_PROVIDER STREQUAL "auto"
            OR WL2_GSTREAMER_PROVIDER STREQUAL "fetch")
        _wl2_gstreamer_pkgcheck(WL2_GSTREAMER_SYS)
        if(WL2_GSTREAMER_SYS_FOUND)
            wl2_dependency_note_package(GSTREAMER "${WL2_GSTREAMER_PROVIDER}")
            set(${out_found} TRUE PARENT_SCOPE)
            set(${out_target} PkgConfig::WL2_GSTREAMER_SYS PARENT_SCOPE)
            set(${out_is_system} TRUE PARENT_SCOPE)
            set(${out_provider_used} package PARENT_SCOPE)
            wl2_dependency_note_result(gstreamer system "gstreamer-1.0 ${WL2_GSTREAMER_SYS_VERSION}")
            message(STATUS "Using package/system GStreamer ${WL2_GSTREAMER_SYS_VERSION}")
            return()
        endif()
        if(WL2_GSTREAMER_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_GSTREAMER_PROVIDER=package but GStreamer development packages were not found")
        endif()
    endif()

    message(STATUS "GStreamer development packages not found for wl2_gstreamer")
    wl2_dependency_note_result(gstreamer DISABLED "not found")
endfunction()

# Discover an optional GStreamer feature module (app/video/audio/pbutils) using
# the same PKG_CONFIG_PATH as the resolved core provider. Sets
# <out_prefix>_FOUND and creates PkgConfig::<out_prefix> (GLOBAL) when present.
function(wl2_find_gstreamer_feature out_prefix pc_module)
    # find_package(PkgConfig) is called from wl2_find_gstreamer()'s function
    # scope, so PKG_CONFIG_VERSION is not visible here; re-establish it or
    # FindPkgConfig's internal re-check if() fails with an empty version operand.
    find_package(PkgConfig QUIET)
    if(NOT PkgConfig_FOUND)
        set(${out_prefix}_FOUND FALSE PARENT_SCOPE)
        return()
    endif()
    if(WL2_GSTREAMER_IS_SYSTEM)
        set(_saved "$ENV{PKG_CONFIG_PATH}")
    else()
        set(_saved "$ENV{PKG_CONFIG_PATH}")
        set(ENV{PKG_CONFIG_PATH}
            "${WL2_GSTREAMER_ROOT}/lib/pkgconfig:${WL2_GSTREAMER_ROOT}/lib64/pkgconfig:${WL2_GSTREAMER_ROOT}/share/pkgconfig:${_saved}")
    endif()
    pkg_check_modules(${out_prefix} QUIET IMPORTED_TARGET GLOBAL ${pc_module})
    set(ENV{PKG_CONFIG_PATH} "${_saved}")
    set(${out_prefix}_FOUND "${${out_prefix}_FOUND}" PARENT_SCOPE)
endfunction()

function(wl2_find_gstreamer)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)
    wl2_declare_dependency(GSTREAMER
        ROOT_DEFAULT "${WL2_DEPS_ROOT}/gstreamer"
        FIND_CALLBACK _wl2_resolve_gstreamer)
    set(WL2_HAVE_GSTREAMER "${WL2_GSTREAMER_FOUND}" PARENT_SCOPE)
    set(WL2_GSTREAMER_TARGET "${WL2_GSTREAMER_TARGET}" PARENT_SCOPE)
    set(WL2_GSTREAMER_IS_SYSTEM "${WL2_GSTREAMER_IS_SYSTEM}" PARENT_SCOPE)
    set(WL2_GSTREAMER_PROVIDER_USED "${WL2_GSTREAMER_PROVIDER_USED}" PARENT_SCOPE)
endfunction()
