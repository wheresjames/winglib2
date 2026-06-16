include(cmake/options.cmake)

if(NOT WL2_ENABLE_ASIO)
    return()
endif()

# standalone Asio is used only by this module, so it is discovered here rather
# than in the global build space. wl2_find_asio() sets WL2_HAVE_ASIO /
# WL2_ASIO_TARGET in this scope and creates the GLOBAL wl2_asio_dep target.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Asio.cmake)
wl2_find_asio()

if(NOT WL2_HAVE_ASIO)
    message(STATUS "standalone Asio not found; wl2_asio module disabled")
    return()
endif()

wl2_add_module(wl2_asio
    MODULE_NAME wl2:asio
    REGISTER_FUNCTION wl2_asio_register_module
    STABLE_ID 25de54b1-78cb-4109-9a31-b9aed98807a1
    FEATURE WL2_ENABLE_ASIO
    SOURCES src/wl2_asio.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_ASIO_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_ASIO_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_ASIO_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")

# Ship the dependency helper so out-of-tree consumers can resolve Asio the same
# way (modules/wl2_asio/cmake/{WL2Asio,options}.cmake).
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Asio.cmake"
    DESTINATION modules/wl2_asio)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake"
    DESTINATION modules/wl2_asio)

# Asio is header-only and entirely internal to the compiled static library, so
# consumers never include it. They only need the wl2_asio_dep INTERFACE target
# (referenced as a LINK_ONLY dependency of the exported static library) to exist;
# recreate it carrying Threads, which is the sole real link requirement.
wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET wl2_asio_dep)
    include(CMakeFindDependencyMacro)
    find_dependency(Threads)
    add_library(wl2_asio_dep INTERFACE IMPORTED)
    set_target_properties(wl2_asio_dep PROPERTIES
        INTERFACE_LINK_LIBRARIES Threads::Threads)
endif()
]==])

if(TARGET wl2_asio_static)
    if(WL2_BUILD_EXAMPLES)
        add_subdirectory(examples/tcp-echo)
    endif()
    wl2_add_module_tests(wl2_asio)
endif()
