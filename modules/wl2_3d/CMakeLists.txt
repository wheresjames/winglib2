include(cmake/options.cmake)

if(NOT WL2_ENABLE_3D)
    return()
endif()

set(WL2_3D_HAVE_MAGNUM 0)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Magnum.cmake)
wl2_find_magnum()
if(WL2_3D_HAVE_MAGNUM)
    set(_wl2_3d_have_magnum_define 1)
else()
    set(_wl2_3d_have_magnum_define 0)
endif()
if(NOT WL2_HAVE_LIBMEMBUS)
    message(STATUS "libmembus not found; wl2_3d module disabled")
    return()
endif()

wl2_add_module(wl2_3d
    MODULE_NAME wl2:3d
    REGISTER_FUNCTION wl2_3d_register_module
    STABLE_ID 8cc3d260-02f4-4466-a8d1-3029fbbbf41c
    FEATURE WL2_ENABLE_3D
    REQUIRES_MODULES wl2:membus
    SOURCES src/wl2_3d.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_3D_MAGNUM_TARGETS}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_3D_STATIC_MODULE=1 WL2_3D_HAVE_MAGNUM=${_wl2_3d_have_magnum_define} WL2_3D_GPU_SAMPLES=${WL2_3D_GPU_SAMPLES}
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}" WL2_3D_HAVE_MAGNUM=${_wl2_3d_have_magnum_define} WL2_3D_GPU_SAMPLES=${WL2_3D_GPU_SAMPLES})

if(WL2_3D_HAVE_MAGNUM AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(_wl2_3d_target wl2_3d_static wl2_3d_shared)
        if(TARGET ${_wl2_3d_target})
            target_compile_options(${_wl2_3d_target} PRIVATE -Wno-cpp)
        endif()
    endforeach()
endif()

if(TARGET wl2_3d_static)
    wl2_add_module_tests(wl2_3d)
endif()
