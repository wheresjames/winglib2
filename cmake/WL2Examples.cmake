# Example programs (opt-in via WL2_BUILD_EXAMPLES).
#
# Included from the root CMakeLists.txt (same scope), so add_subdirectory paths
# resolve against the project root.

# Examples are opt-in. Module-specific examples are added by the modules that
# own them; this file keeps only project-level examples and generic discovery.
if(WL2_BUILD_EXAMPLES)
    add_subdirectory(examples/cpp/embedded)
    add_subdirectory(examples/js/hello)
    add_subdirectory(examples/js/test-scripts)
    add_subdirectory(examples/cpp/resources)
    add_subdirectory(examples/js/resources)
    add_subdirectory(examples/js/thread-tree)
    if(TARGET wl2_3d_static AND TARGET wl2_slint_static AND TARGET wl2_membus_static)
        add_subdirectory(examples/js/3d-dashboard)
    endif()
    file(GLOB _wl2_example_module_dirs
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES TRUE
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/modules/*")
    if(_wl2_example_module_dirs)
        list(SORT _wl2_example_module_dirs)
    endif()
    foreach(_wl2_example_module_dir IN LISTS _wl2_example_module_dirs)
        if(IS_DIRECTORY "${_wl2_example_module_dir}"
                AND EXISTS "${_wl2_example_module_dir}/CMakeLists.txt")
            get_filename_component(_wl2_example_module_name "${_wl2_example_module_dir}" NAME)
            add_subdirectory(
                "${_wl2_example_module_dir}"
                "${CMAKE_CURRENT_BINARY_DIR}/examples/modules/${_wl2_example_module_name}")
        endif()
    endforeach()
endif()
