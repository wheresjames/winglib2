# External dependency discovery/fetch.
#
# Included from the root CMakeLists.txt (same scope), so CMAKE_CURRENT_SOURCE_DIR
# still points at the project root. Requires the helper modules (WL2TargetPath,
# WL2Resources, WL2Modules) and the project options to have been included first.

# Resolve the per-config dependency root, then locate/fetch external deps that
# wl2_core links directly. Module-owned dependencies are discovered by the
# modules that use them, not in the global build space.
wl2_configure_dependency_root()
wl2_configure_dependency_options()
include(cmake/deps/WL2V8.cmake)
include(cmake/deps/WL2QuickJS.cmake)
include(cmake/deps/WL2Libmembus.cmake)
