function(wl2_get_target_build_path out_var)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _system_name)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _system_arch)

    if(CMAKE_BUILD_TYPE)
        string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type)
    else()
        set(_build_type "multi")
    endif()

    if(CMAKE_CXX_COMPILER_TARGET)
        string(TOLOWER "${CMAKE_CXX_COMPILER_TARGET}" _compiler_target)
        set(_system_arch "${_compiler_target}")
    endif()

    string(REPLACE "/" "_" _system_name "${_system_name}")
    string(REPLACE "/" "_" _system_arch "${_system_arch}")
    string(REPLACE "/" "_" _build_type "${_build_type}")

    set(${out_var} "${_system_name}-${_system_arch}/${_build_type}" PARENT_SCOPE)
endfunction()

function(wl2_configure_dependency_root)
    wl2_get_target_build_path(_wl2_target_path)
    set(WL2_TARGET_BUILD_PATH "${_wl2_target_path}" CACHE STRING "Target-specific dependency path segment" FORCE)

    set(_default_deps_root "${CMAKE_SOURCE_DIR}/.deps/${WL2_TARGET_BUILD_PATH}")
    set(WL2_DEPS_ROOT "${_default_deps_root}" CACHE PATH "Target-specific local dependency prefix")

    message(STATUS "Winglib2 dependency target path: ${WL2_TARGET_BUILD_PATH}")
    message(STATUS "Winglib2 dependency root: ${WL2_DEPS_ROOT}")
endfunction()

function(wl2_add_imported_static_library target include_file library_file)
    set(options REQUIRED)
    set(oneValueArgs ROOT INCLUDE_SUBDIR LIB_SUBDIR)
    set(multiValueArgs EXTRA_LIBRARIES)
    cmake_parse_arguments(WL2IMP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT WL2IMP_ROOT)
        message(FATAL_ERROR "wl2_add_imported_static_library requires ROOT")
    endif()
    if(NOT WL2IMP_INCLUDE_SUBDIR)
        set(WL2IMP_INCLUDE_SUBDIR "include")
    endif()
    if(NOT WL2IMP_LIB_SUBDIR)
        set(WL2IMP_LIB_SUBDIR "lib")
    endif()

    set(_include_dir "${WL2IMP_ROOT}/${WL2IMP_INCLUDE_SUBDIR}")
    set(_library "${WL2IMP_ROOT}/${WL2IMP_LIB_SUBDIR}/${library_file}")

    if(EXISTS "${_include_dir}/${include_file}" AND EXISTS "${_library}")
        add_library(${target} STATIC IMPORTED GLOBAL)
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION "${_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}")
        if(WL2IMP_EXTRA_LIBRARIES)
            set_target_properties(${target} PROPERTIES
                INTERFACE_LINK_LIBRARIES "${WL2IMP_EXTRA_LIBRARIES}")
        endif()
        set(${target}_FOUND TRUE PARENT_SCOPE)
    else()
        set(${target}_FOUND FALSE PARENT_SCOPE)
    endif()
endfunction()
