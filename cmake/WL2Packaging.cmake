# Install and package export.
#
# Builds the install target list (core + runner + any non-imported deps and
# built-in modules), bundles external libs/headers when they were built locally,
# then writes the winglib2 CMake package, manifest, and uninstall helpers.
#
# Included last from the root CMakeLists.txt (same scope) so module/example
# targets and the WL2_MODULE_* global properties are already populated, and
# CMAKE_CURRENT_SOURCE_DIR/CMAKE_CURRENT_BINARY_DIR point at the project root.

set(_wl2_install_targets wl2_core wl2)
set(WL2_PACKAGE_EXPORTS_QUICKJS FALSE)
set(WL2_PACKAGE_INSTALLS_QUICKJS FALSE)
set(WL2_PACKAGE_INSTALLS_QUICKJS_HEADERS FALSE)
set(WL2_PACKAGE_QUICKJS_LIBRARY_NAME "")
set(WL2_PACKAGE_NEEDS_LIBMEMBUS_PACKAGE FALSE)
set(WL2_PACKAGE_NEEDS_BOOST_STACKTRACE_BACKTRACE FALSE)
set(_wl2_package_bundle_manifest_entries)
set(_wl2_package_generated_module_fragments)
# QuickJS: export the target if we built it, otherwise bundle the imported lib
# and its headers so out-of-tree modules can compile against the package.
if(TARGET quickjs)
    get_target_property(_wl2_quickjs_imported quickjs IMPORTED)
    if(NOT _wl2_quickjs_imported)
        list(APPEND _wl2_install_targets quickjs)
        set(WL2_PACKAGE_EXPORTS_QUICKJS TRUE)
    else()
        get_target_property(_wl2_quickjs_location quickjs IMPORTED_LOCATION)
        if(_wl2_quickjs_location)
            get_filename_component(WL2_PACKAGE_QUICKJS_LIBRARY_NAME "${_wl2_quickjs_location}" NAME)
            install(FILES "${_wl2_quickjs_location}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
            set(WL2_PACKAGE_INSTALLS_QUICKJS TRUE)
            # Ship the QuickJS headers so out-of-tree modules (for example
            # examples/modules/wl2_echo) can compile against the installed package.
            get_target_property(_wl2_quickjs_include quickjs INTERFACE_INCLUDE_DIRECTORIES)
            if(_wl2_quickjs_include AND EXISTS "${_wl2_quickjs_include}/quickjs.h")
                install(FILES
                    "${_wl2_quickjs_include}/quickjs.h"
                    "${_wl2_quickjs_include}/quickjs-libc.h"
                    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/quickjs
                    OPTIONAL)
                set(WL2_PACKAGE_INSTALLS_QUICKJS_HEADERS TRUE)
            endif()
        endif()
    endif()
endif()
if(WL2_PACKAGE_EXPORTS_QUICKJS)
    install(FILES
        "${WL2_DEPS_ROOT}/quickjs/include/quickjs.h"
        "${WL2_DEPS_ROOT}/quickjs/include/quickjs-libc.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/quickjs
        OPTIONAL)
endif()
# libmembus: export the locally built dependency and record what consumers need.
if(TARGET wl2_libmembus_dependency)
    get_target_property(_wl2_libmembus_imported wl2_libmembus_dependency IMPORTED)
    if(NOT _wl2_libmembus_imported)
        list(APPEND _wl2_install_targets wl2_libmembus_dependency)
        get_target_property(_wl2_libmembus_interface_links wl2_libmembus_dependency INTERFACE_LINK_LIBRARIES)
        if("Boost::stacktrace_backtrace" IN_LIST _wl2_libmembus_interface_links)
            set(WL2_PACKAGE_NEEDS_BOOST_STACKTRACE_BACKTRACE TRUE)
        endif()
        get_target_property(_wl2_libmembus_type wl2_libmembus_dependency TYPE)
        if(_wl2_libmembus_type STREQUAL "INTERFACE_LIBRARY")
            set(WL2_PACKAGE_NEEDS_LIBMEMBUS_PACKAGE TRUE)
        endif()
    endif()
endif()
# Built-in modules registered via wl2_add_module() add their install targets here.
get_property(_wl2_module_install_targets GLOBAL PROPERTY WL2_MODULE_INSTALL_TARGETS)
if(_wl2_module_install_targets)
    list(APPEND _wl2_install_targets ${_wl2_module_install_targets})
endif()

get_property(_wl2_package_bundle_libraries GLOBAL PROPERTY WL2_PACKAGE_BUNDLE_LIBRARIES)
foreach(_wl2_bundle_entry IN LISTS _wl2_package_bundle_libraries)
    string(REPLACE "|" ";" _wl2_bundle_fields "${_wl2_bundle_entry}")
    list(GET _wl2_bundle_fields 0 _wl2_bundle_dep)
    list(GET _wl2_bundle_fields 1 _wl2_bundle_target)
    list(GET _wl2_bundle_fields 2 _wl2_bundle_definitions)
    list(GET _wl2_bundle_fields 3 _wl2_bundle_location)
    if(NOT _wl2_bundle_location)
        message(FATAL_ERROR "Package bundle entry has no library location for target: ${_wl2_bundle_target}")
    endif()
    get_filename_component(_wl2_bundle_library_name "${_wl2_bundle_location}" NAME)
    install(FILES "${_wl2_bundle_location}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    list(APPEND _wl2_package_bundle_manifest_entries
        "${CMAKE_INSTALL_LIBDIR}/${_wl2_bundle_library_name}")

    string(APPEND _wl2_package_generated_module_fragments
        "if(NOT TARGET ${_wl2_bundle_target})\n"
        "    add_library(${_wl2_bundle_target} UNKNOWN IMPORTED GLOBAL)\n"
        "    set_target_properties(${_wl2_bundle_target} PROPERTIES\n"
        "        IMPORTED_LOCATION \"\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}/${_wl2_bundle_library_name}\"\n"
        "        INTERFACE_INCLUDE_DIRECTORIES \"\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_INCLUDEDIR}\")\n")
    if(NOT _wl2_bundle_definitions STREQUAL "")
        string(REPLACE "," ";" _wl2_bundle_definition_list "${_wl2_bundle_definitions}")
        string(REPLACE ";" "\\;" _wl2_bundle_definitions_escaped "${_wl2_bundle_definition_list}")
        string(APPEND _wl2_package_generated_module_fragments
            "    set_property(TARGET ${_wl2_bundle_target} APPEND PROPERTY\n"
            "        INTERFACE_COMPILE_DEFINITIONS \"${_wl2_bundle_definitions_escaped}\")\n")
    endif()
    string(APPEND _wl2_package_generated_module_fragments "endif()\n")
endforeach()

get_property(_wl2_package_install_include_dirs GLOBAL PROPERTY WL2_PACKAGE_INSTALL_INCLUDE_DIRS)
foreach(_wl2_include_entry IN LISTS _wl2_package_install_include_dirs)
    string(REPLACE "|" ";" _wl2_include_fields "${_wl2_include_entry}")
    list(GET _wl2_include_fields 0 _wl2_include_source)
    list(GET _wl2_include_fields 1 _wl2_include_dest)
    install(DIRECTORY "${_wl2_include_source}/" DESTINATION "${_wl2_include_dest}" FILES_MATCHING PATTERN "*.h")
    list(APPEND _wl2_package_bundle_manifest_entries "${_wl2_include_dest}/*.h")
endforeach()

get_property(_wl2_package_cmake_files GLOBAL PROPERTY WL2_PACKAGE_CMAKE_FILES)
set(_wl2_package_cmake_include_lines)
foreach(_wl2_cmake_entry IN LISTS _wl2_package_cmake_files)
    string(REPLACE "|" ";" _wl2_cmake_fields "${_wl2_cmake_entry}")
    list(GET _wl2_cmake_fields 0 _wl2_cmake_source)
    list(GET _wl2_cmake_fields 1 _wl2_cmake_dest)
    install(FILES "${_wl2_cmake_source}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/${_wl2_cmake_dest}")
    get_filename_component(_wl2_cmake_name "${_wl2_cmake_source}" NAME)
    list(APPEND _wl2_installed_module_cmake_entries
        "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/${_wl2_cmake_dest}/${_wl2_cmake_name}")
    string(APPEND _wl2_package_cmake_include_lines
        "include(\"\${CMAKE_CURRENT_LIST_DIR}/${_wl2_cmake_dest}/${_wl2_cmake_name}\")\n")
endforeach()

get_property(_wl2_package_config_fragments GLOBAL PROPERTY WL2_PACKAGE_CONFIG_FRAGMENTS)
set(WL2_PACKAGE_MODULE_CONFIG_FRAGMENTS "${_wl2_package_generated_module_fragments}")
foreach(_wl2_fragment IN LISTS _wl2_package_config_fragments)
    string(APPEND WL2_PACKAGE_MODULE_CONFIG_FRAGMENTS "${_wl2_fragment}")
endforeach()

string(CONCAT WL2_PACKAGE_CORE_CONFIG
    "if(${WL2_PACKAGE_NEEDS_LIBMEMBUS_PACKAGE} AND NOT TARGET libmembus::libmembus)\n"
    "    find_dependency(libmembus ${WL2_LIBMEMBUS_TARGET_VERSION} CONFIG)\n"
    "endif()\n"
    "if(${WL2_PACKAGE_NEEDS_BOOST_STACKTRACE_BACKTRACE} AND NOT TARGET Boost::stacktrace_backtrace)\n"
    "    find_dependency(Boost CONFIG COMPONENTS stacktrace_backtrace)\n"
    "endif()\n")
string(APPEND WL2_PACKAGE_MODULE_CONFIG_FRAGMENTS "${_wl2_package_cmake_include_lines}")

# Install the targets (exported set) plus public and module headers.
install(TARGETS ${_wl2_install_targets}
    EXPORT winglib2Targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
install(FILES
    "${CMAKE_BINARY_DIR}/generated/winglib2/build_info.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/winglib2")
get_property(_wl2_module_header_dirs GLOBAL PROPERTY WL2_MODULE_HEADER_DIRS)
foreach(_wl2_module_header_dir IN LISTS _wl2_module_header_dirs)
    install(DIRECTORY "${_wl2_module_header_dir}/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
endforeach()
install(FILES
    cmake/WL2Modules.cmake
    cmake/WL2Options.cmake
    cmake/WL2Resources.cmake
    cmake/WL2TargetPath.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/winglib2)
install(FILES
    cmake/deps/WL2Dependency.cmake
    cmake/deps/WL2Libmembus.cmake
    cmake/deps/WL2QuickJS.cmake
    cmake/deps/WL2V8.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/deps)

# Generate and install the find_package(winglib2) config and version files.
configure_package_config_file(
    cmake/winglib2Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/winglib2)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/winglib2)
install(EXPORT winglib2Targets
    NAMESPACE winglib2::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/winglib2)

# Build an exact list of installed files so the package ships its own manifest
# and a matching uninstall script (used by the `uninstall` target below).
set(_wl2_installed_manifest_entries)
foreach(_target IN LISTS _wl2_install_targets)
    get_target_property(_wl2_target_type ${_target} TYPE)
    if(_wl2_target_type STREQUAL "EXECUTABLE")
        list(APPEND _wl2_installed_manifest_entries "${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:${_target}>")
    else()
        list(APPEND _wl2_installed_manifest_entries "${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:${_target}>")
    endif()
endforeach()
if(WL2_PACKAGE_INSTALLS_QUICKJS)
    list(APPEND _wl2_installed_manifest_entries "${CMAKE_INSTALL_LIBDIR}/${WL2_PACKAGE_QUICKJS_LIBRARY_NAME}")
endif()
if(WL2_PACKAGE_INSTALLS_QUICKJS_HEADERS)
    list(APPEND _wl2_installed_manifest_entries
        "${CMAKE_INSTALL_INCLUDEDIR}/quickjs/quickjs.h"
        "${CMAKE_INSTALL_INCLUDEDIR}/quickjs/quickjs-libc.h")
endif()
list(APPEND _wl2_installed_manifest_entries ${_wl2_package_bundle_manifest_entries})
if(WL2_PACKAGE_EXPORTS_QUICKJS)
    list(APPEND _wl2_installed_manifest_entries
        "${CMAKE_INSTALL_INCLUDEDIR}/quickjs/quickjs.h"
        "${CMAKE_INSTALL_INCLUDEDIR}/quickjs/quickjs-libc.h")
endif()

file(GLOB_RECURSE _wl2_public_headers
    CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
foreach(_header IN LISTS _wl2_public_headers)
    list(APPEND _wl2_installed_manifest_entries "${CMAKE_INSTALL_INCLUDEDIR}/${_header}")
endforeach()
list(APPEND _wl2_installed_manifest_entries "${CMAKE_INSTALL_INCLUDEDIR}/winglib2/build_info.h")

foreach(_wl2_module_header_dir IN LISTS _wl2_module_header_dirs)
    file(GLOB_RECURSE _wl2_module_headers
        CONFIGURE_DEPENDS
        RELATIVE "${_wl2_module_header_dir}"
        "${_wl2_module_header_dir}/*.h")
    foreach(_header IN LISTS _wl2_module_headers)
        list(APPEND _wl2_installed_manifest_entries "${CMAKE_INSTALL_INCLUDEDIR}/${_header}")
    endforeach()
endforeach()

list(APPEND _wl2_installed_manifest_entries
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/WL2Modules.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/WL2Options.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/WL2Resources.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/WL2TargetPath.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/deps/WL2Dependency.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/deps/WL2Libmembus.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/deps/WL2QuickJS.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/deps/WL2V8.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/winglib2Config.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/winglib2ConfigVersion.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/winglib2Targets*.cmake"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/winglib2InstalledManifest.txt"
    "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2/winglib2Uninstall.cmake")
list(APPEND _wl2_installed_manifest_entries ${_wl2_installed_module_cmake_entries})
list(REMOVE_DUPLICATES _wl2_installed_manifest_entries)
list(SORT _wl2_installed_manifest_entries)
string(REPLACE ";" "\n" _wl2_installed_manifest_text "${_wl2_installed_manifest_entries}")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/winglib2InstalledManifest.txt"
    CONTENT "${_wl2_installed_manifest_text}\n")
set(_wl2_package_dir_relative "${CMAKE_INSTALL_LIBDIR}/cmake/winglib2")
string(REPLACE "/" ";" _wl2_package_dir_parts "${_wl2_package_dir_relative}")
set(WL2_INSTALLED_UNINSTALL_PREFIX_RELATIVE)
foreach(_part IN LISTS _wl2_package_dir_parts)
    if(NOT _part STREQUAL "")
        string(APPEND WL2_INSTALLED_UNINSTALL_PREFIX_RELATIVE "/..")
    endif()
endforeach()
configure_file(
    cmake/winglib2InstalledUninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2Uninstall.cmake
    @ONLY)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2InstalledManifest.txt
    ${CMAKE_CURRENT_BINARY_DIR}/winglib2Uninstall.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/winglib2)

# `uninstall` target: removes the files recorded in install_manifest.txt.
configure_file(
    cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Uninstalling Winglib2 files listed in install_manifest.txt")
