include(cmake/options.cmake)

if(NOT WL2_ENABLE_SLINT)
    return()
endif()

# Slint is used only by this module, so it is discovered here rather than in the
# global build space. wl2_find_slint() sets WL2_HAVE_SLINT / WL2_SLINT_TARGET in
# this scope (the Slint::Slint imported target from the prebuilt package).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Slint.cmake)
wl2_find_slint()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Nfd.cmake)
wl2_find_nfd()

if(NOT WL2_HAVE_SLINT)
    message(STATUS "Slint not found; wl2_slint module disabled")
    return()
endif()

set(_wl2_slint_private_libs ${WL2_SLINT_TARGET})
set(_wl2_slint_dynamic_libs ${WL2_SLINT_TARGET})
set(_wl2_slint_private_defs WL2_SLINT_HAVE_NATIVE_DIALOGS=0)
if(WL2_HAVE_NFD)
    list(APPEND _wl2_slint_private_libs ${WL2_NFD_TARGET})
    list(APPEND _wl2_slint_dynamic_libs ${WL2_NFD_TARGET})
    set(_wl2_slint_private_defs WL2_SLINT_HAVE_NATIVE_DIALOGS=1)
    if(TARGET nfd AND NOT WL2_NFD_PROVIDER_USED STREQUAL "package")
        set_target_properties(nfd PROPERTIES PUBLIC_HEADER "")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_INSTALL_TARGETS nfd)
    endif()
endif()

wl2_add_module(wl2_slint
    MODULE_NAME wl2:slint
    REGISTER_FUNCTION wl2_slint_register_module
    STABLE_ID ac80212f-1920-4b74-adb0-36e49971d278
    FEATURE WL2_ENABLE_SLINT
    SOURCES src/wl2_slint.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${_wl2_slint_private_libs}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${_wl2_slint_dynamic_libs}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_SLINT_STATIC_MODULE=1 ${_wl2_slint_private_defs}
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}" ${_wl2_slint_private_defs})

# Ship the dependency helper so out-of-tree consumers can resolve Slint the same
# way (modules/wl2_slint/cmake/{WL2Slint,options}.cmake).
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Slint.cmake"
    DESTINATION modules/wl2_slint)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Nfd.cmake"
    DESTINATION modules/wl2_slint)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake"
    DESTINATION modules/wl2_slint)

# Slint links a real shared library, so out-of-tree consumers of the exported
# wl2_slint_static target must be able to recreate the Slint::Slint target it
# links against (`$<LINK_ONLY:Slint::Slint>`). How depends on the provider:
#
#   * package (system install): re-discover with find_dependency(Slint), the same
#     way wl2_curl re-finds a system libcurl.
#   * fetch/local/source (a staged prebuilt package): the package is NOT on a
#     consumer's prefix path, so find_dependency would fail for every consumer of
#     winglib2 — even ones that don't use the UI module. Instead bundle the Slint
#     shared library into the winglib2 install (like wl2_curl bundles libcurl) and
#     recreate Slint::Slint pointing at it, so loading winglib2 never needs Slint
#     elsewhere. The Slint::Slint chain is INTERFACE -> Slint::slint_cpp ->
#     slint_cpp-shared (the actual .so), so the .so target is what gets bundled.
if(WL2_SLINT_PROVIDER_USED STREQUAL "package")
    wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET Slint::Slint)
    include(CMakeFindDependencyMacro)
    find_dependency(Slint)
endif()
]==])
else()
    wl2_module_bundle_dependency(slint_cpp-shared TARGET slint_cpp-shared)
    wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET Slint::Slint)
    add_library(Slint::Slint INTERFACE IMPORTED)
    set_target_properties(Slint::Slint PROPERTIES INTERFACE_LINK_LIBRARIES slint_cpp-shared)
endif()
]==])
endif()

if(TARGET wl2_slint_static)
    if(WL2_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/counter/CMakeLists.txt")
        add_subdirectory(examples/counter)
    endif()
    if(WL2_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/showcase/CMakeLists.txt")
        add_subdirectory(examples/showcase)
    endif()
    wl2_add_module_tests(wl2_slint)
endif()
