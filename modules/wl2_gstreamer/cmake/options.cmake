# Included from the module CMakeLists and (after install) from the packaged
# module config, where wl2_module_option is not defined; guard the call so
# external consumers can include it safely.
if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE
        DEFAULT ${WL2_ENABLE_EXTENDED_MODULES}
        DOC "Build the wl2:gstreamer extended media module")
endif()

# Provider selection for the GStreamer dependency. GStreamer is discovered
# through pkg-config, which covers a system install (package), a target-local /
# cross-sysroot install (local, via WL2_GSTREAMER_ROOT), and a toolchain-provided
# sysroot. Source fetch is intentionally unsupported: GStreamer is a large,
# Meson-based stack that is not built from source by this project.
set(WL2_GSTREAMER_PROVIDER "auto" CACHE STRING "GStreamer provider: auto, local, package, fetch, or off")
set_property(CACHE WL2_GSTREAMER_PROVIDER PROPERTY STRINGS auto local package fetch off)
if(COMMAND wl2_dependency_configure_provider)
    wl2_dependency_configure_provider(GSTREAMER WL2_GSTREAMER_PROVIDER)
endif()

if(WL2_DEPS_ROOT)
    set(WL2_GSTREAMER_ROOT "${WL2_DEPS_ROOT}/gstreamer" CACHE PATH "Path to a target-local GStreamer install")
endif()

# Optional GStreamer feature libraries. Each pulls an extra pkg-config module and
# defines WL2_GSTREAMER_HAVE_<FEATURE> when present. gstreamer-app-1.0 backs the
# appsink/appsrc membus bridges; the others back format helpers and device
# discovery. None are required for the core pipeline runtime.
set(WL2_GSTREAMER_ENABLE_APP ON CACHE BOOL "Enable gstreamer-app-1.0 (appsink/appsrc) support")
set(WL2_GSTREAMER_ENABLE_VIDEO ON CACHE BOOL "Enable gstreamer-video-1.0 helpers")
set(WL2_GSTREAMER_ENABLE_AUDIO ON CACHE BOOL "Enable gstreamer-audio-1.0 helpers")
set(WL2_GSTREAMER_ENABLE_PBUTILS ON CACHE BOOL "Enable gstreamer-pbutils-1.0 (media discovery and device monitor)")
