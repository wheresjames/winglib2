define_property(GLOBAL PROPERTY WL2_MODULE_INSTALL_TARGETS
    BRIEF_DOCS "Winglib2 module targets to install and export"
    FULL_DOCS "Targets created by wl2_add_module() that should be installed and exported with the Winglib2 package.")
define_property(GLOBAL PROPERTY WL2_MODULE_HEADER_DIRS
    BRIEF_DOCS "Winglib2 module public header directories to install"
    FULL_DOCS "Public include roots registered by wl2_add_module() for package installation.")
define_property(GLOBAL PROPERTY WL2_STATIC_MODULE_TARGETS
    BRIEF_DOCS "Winglib2 builtin static module targets"
    FULL_DOCS "Static module targets registered by wl2_add_module() for builtin runner linkage.")
define_property(GLOBAL PROPERTY WL2_STATIC_MODULE_REGISTRATION_FUNCTIONS
    BRIEF_DOCS "Winglib2 builtin static module registration functions"
    FULL_DOCS "Static module registration functions registered by wl2_add_module() for generated runner registration.")
define_property(GLOBAL PROPERTY WL2_STATIC_MODULE_NAMES
    BRIEF_DOCS "Winglib2 builtin static module names"
    FULL_DOCS "Canonical module names registered by wl2_add_module() for generated runner diagnostics.")
define_property(GLOBAL PROPERTY WL2_STATIC_MODULE_REQUIRES
    BRIEF_DOCS "Winglib2 builtin static module required dependencies"
    FULL_DOCS "Per-module required dependency names (pipe-joined) aligned with WL2_STATIC_MODULE_NAMES, used for topological ordering of builtin static modules.")
define_property(GLOBAL PROPERTY WL2_STATIC_MODULE_OPTIONAL
    BRIEF_DOCS "Winglib2 builtin static module optional dependencies"
    FULL_DOCS "Per-module optional dependency names (pipe-joined) aligned with WL2_STATIC_MODULE_NAMES, used for topological ordering of builtin static modules.")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_MODULE_NAMES
    BRIEF_DOCS "Winglib2 module registry canonical names"
    FULL_DOCS "Canonical module names recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_STATIC_TARGETS
    BRIEF_DOCS "Winglib2 module registry static targets"
    FULL_DOCS "Static targets recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_DYNAMIC_TARGETS
    BRIEF_DOCS "Winglib2 module registry dynamic targets"
    FULL_DOCS "Dynamic targets recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_REGISTRATION_FUNCTIONS
    BRIEF_DOCS "Winglib2 module registry registration functions"
    FULL_DOCS "Static registration functions recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_VERSIONS
    BRIEF_DOCS "Winglib2 module registry versions"
    FULL_DOCS "Module versions recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_STABLE_IDS
    BRIEF_DOCS "Winglib2 module registry stable identifiers"
    FULL_DOCS "Module stable identifiers recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_FEATURES
    BRIEF_DOCS "Winglib2 module registry feature switches"
    FULL_DOCS "Feature switches recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_TEST_DIRS
    BRIEF_DOCS "Winglib2 module registry test directories"
    FULL_DOCS "Module-local test directories recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_DOCS_DIRS
    BRIEF_DOCS "Winglib2 module registry docs directories"
    FULL_DOCS "Module-local docs directories recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_REQUIRES
    BRIEF_DOCS "Winglib2 module registry required module dependencies"
    FULL_DOCS "Required dependency names (pipe-joined) recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_MODULE_REGISTRY_OPTIONAL
    BRIEF_DOCS "Winglib2 module registry optional module dependencies"
    FULL_DOCS "Optional dependency names (pipe-joined) recorded by wl2_add_module().")
define_property(GLOBAL PROPERTY WL2_PACKAGE_BUNDLE_LIBRARIES
    BRIEF_DOCS "Winglib2 module-owned dependency libraries to bundle"
    FULL_DOCS "Encoded module-owned dependency target metadata used by package installation.")
define_property(GLOBAL PROPERTY WL2_PACKAGE_INSTALL_INCLUDE_DIRS
    BRIEF_DOCS "Winglib2 module-owned dependency include directories to install"
    FULL_DOCS "Encoded include directory metadata used by package installation.")
define_property(GLOBAL PROPERTY WL2_PACKAGE_CMAKE_FILES
    BRIEF_DOCS "Winglib2 module-owned CMake helper files to install and include"
    FULL_DOCS "Encoded CMake helper file metadata used by package installation.")
define_property(GLOBAL PROPERTY WL2_PACKAGE_CONFIG_FRAGMENTS
    BRIEF_DOCS "Winglib2 module-owned package config fragments"
    FULL_DOCS "CMake snippets contributed by modules to winglib2Config.cmake.")

function(_wl2_read_module_marker module_dir out_provides out_category)
    set(_marker "${module_dir}/wl2.module.source.yml")
    if(NOT EXISTS "${_marker}")
        message(FATAL_ERROR "Missing module marker: ${_marker}")
    endif()

    set(_provides "")
    set(_category "")
    file(STRINGS "${_marker}" _marker_lines)
    foreach(_line IN LISTS _marker_lines)
        if(_line MATCHES "^[ \t]*provides:[ \t]*([^# \t]+)")
            set(_provides "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^[ \t]*category:[ \t]*([^# \t]+)")
            set(_category "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    if(_provides STREQUAL "")
        message(FATAL_ERROR "Module marker ${_marker} is missing provides")
    endif()
    if(_category STREQUAL "")
        message(FATAL_ERROR "Module marker ${_marker} is missing category")
    endif()

    set(${out_provides} "${_provides}" PARENT_SCOPE)
    set(${out_category} "${_category}" PARENT_SCOPE)
endfunction()

function(_wl2_module_feature_name module_provides out_var)
    string(REGEX REPLACE "^wl2:" "" _feature "${module_provides}")
    string(REGEX REPLACE "^wl2_" "" _feature "${_feature}")
    string(REGEX REPLACE "[^A-Za-z0-9]+" "_" _feature "${_feature}")
    string(TOUPPER "${_feature}" _feature)
    set(${out_var} "${_feature}" PARENT_SCOPE)
endfunction()

function(_wl2_module_disabled module_dir module_provides out_var)
    get_filename_component(_module_basename "${module_dir}" NAME)
    set(_disabled FALSE)
    foreach(_entry IN LISTS WL2_DISABLE_MODULES)
        if(_entry STREQUAL "${_module_basename}" OR _entry STREQUAL "${module_provides}")
            set(_disabled TRUE)
        endif()
    endforeach()
    set(${out_var} "${_disabled}" PARENT_SCOPE)
endfunction()

function(wl2_module_option mode)
    if(NOT mode STREQUAL "ENABLE")
        message(FATAL_ERROR "wl2_module_option supports only ENABLE")
    endif()

    set(options)
    set(oneValueArgs DEFAULT DOC)
    set(multiValueArgs)
    cmake_parse_arguments(WL2OPT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    _wl2_read_module_marker("${CMAKE_CURRENT_SOURCE_DIR}" _module_provides _module_category)
    _wl2_module_feature_name("${_module_provides}" _module_feature)
    set(_option_name "WL2_ENABLE_${_module_feature}")

    if(WL2_ENABLE_ALL_MODULES)
        set(_default ON)
    elseif(DEFINED WL2OPT_DEFAULT AND NOT "${WL2OPT_DEFAULT}" STREQUAL "")
        set(_default "${WL2OPT_DEFAULT}")
    elseif(_module_category STREQUAL "extended")
        set(_default "${WL2_ENABLE_EXTENDED_MODULES}")
    else()
        set(_default ON)
    endif()

    if(WL2OPT_DOC)
        set(_doc "${WL2OPT_DOC}")
    else()
        set(_doc "Build ${_module_provides} module")
    endif()

    option(${_option_name} "${_doc}" "${_default}")
    if(WL2_ENABLE_ALL_MODULES)
        set(${_option_name} ON CACHE BOOL "${_doc}" FORCE)
    endif()
    set(${_option_name} "${${_option_name}}" PARENT_SCOPE)
endfunction()

function(wl2_add_modules)
    set(options)
    set(oneValueArgs DIRECTORY)
    set(multiValueArgs EXTRA_DIRS)
    cmake_parse_arguments(WL2DISC "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT WL2DISC_DIRECTORY)
        message(FATAL_ERROR "wl2_add_modules requires DIRECTORY")
    endif()

    set(_module_roots "${WL2DISC_DIRECTORY}")
    list(APPEND _module_roots ${WL2DISC_EXTRA_DIRS} ${WL2_EXTRA_MODULE_DIRS})

    set(_module_dirs)
    foreach(_root IN LISTS _module_roots)
        if(NOT IS_ABSOLUTE "${_root}")
            set(_root_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_root}")
        else()
            set(_root_abs "${_root}")
        endif()
        if(NOT IS_DIRECTORY "${_root_abs}")
            message(FATAL_ERROR "Module discovery directory does not exist: ${_root_abs}")
        endif()
        file(GLOB _candidates CONFIGURE_DEPENDS LIST_DIRECTORIES TRUE "${_root_abs}/*")
        foreach(_candidate IN LISTS _candidates)
            if(IS_DIRECTORY "${_candidate}"
                    AND EXISTS "${_candidate}/CMakeLists.txt"
                    AND EXISTS "${_candidate}/wl2.module.source.yml")
                list(APPEND _module_dirs "${_candidate}")
            endif()
        endforeach()
    endforeach()

    if(_module_dirs)
        list(REMOVE_DUPLICATES _module_dirs)
        list(SORT _module_dirs)
    endif()

    set(_enabled_count 0)
    foreach(_module_dir IN LISTS _module_dirs)
        _wl2_read_module_marker("${_module_dir}" _module_provides _module_category)
        _wl2_module_disabled("${_module_dir}" "${_module_provides}" _disabled)
        if(_disabled)
            message(STATUS "Skipping disabled module ${_module_provides}")
            continue()
        endif()
        get_filename_component(_module_name "${_module_dir}" NAME)
        add_subdirectory("${_module_dir}" "${CMAKE_CURRENT_BINARY_DIR}/modules/${_module_name}")
        math(EXPR _enabled_count "${_enabled_count} + 1")
    endforeach()
    message(STATUS "Discovered ${_enabled_count} Winglib2 module source directories")
endfunction()

function(wl2_module_package_config_fragment)
    set(options)
    set(oneValueArgs CONTENT)
    set(multiValueArgs)
    cmake_parse_arguments(WL2PKGFRAG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT DEFINED WL2PKGFRAG_CONTENT)
        message(FATAL_ERROR "wl2_module_package_config_fragment requires CONTENT")
    endif()
    set_property(GLOBAL APPEND PROPERTY WL2_PACKAGE_CONFIG_FRAGMENTS "${WL2PKGFRAG_CONTENT}")
endfunction()

function(wl2_module_package_cmake_file source_file)
    set(options)
    set(oneValueArgs DESTINATION)
    set(multiValueArgs)
    cmake_parse_arguments(WL2PKGFILE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT EXISTS "${source_file}")
        message(FATAL_ERROR "wl2_module_package_cmake_file missing source file: ${source_file}")
    endif()
    if(NOT WL2PKGFILE_DESTINATION)
        set(WL2PKGFILE_DESTINATION "modules")
    endif()
    set_property(GLOBAL APPEND PROPERTY WL2_PACKAGE_CMAKE_FILES "${source_file}|${WL2PKGFILE_DESTINATION}")
endfunction()

function(wl2_module_bundle_dependency dep)
    set(options)
    set(oneValueArgs TARGET INCLUDE_DIR INCLUDE_DESTINATION)
    set(multiValueArgs COMPILE_DEFINITIONS CMAKE_FILES)
    cmake_parse_arguments(WL2BUNDLE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT WL2BUNDLE_TARGET)
        message(FATAL_ERROR "wl2_module_bundle_dependency(${dep}) requires TARGET")
    endif()
    if(NOT TARGET ${WL2BUNDLE_TARGET})
        message(FATAL_ERROR "wl2_module_bundle_dependency(${dep}) target does not exist: ${WL2BUNDLE_TARGET}")
    endif()
    get_target_property(_wl2_bundle_location ${WL2BUNDLE_TARGET} IMPORTED_LOCATION)
    if(NOT _wl2_bundle_location)
        if(CMAKE_BUILD_TYPE)
            string(TOUPPER "${CMAKE_BUILD_TYPE}" _wl2_bundle_config)
            get_target_property(_wl2_bundle_location ${WL2BUNDLE_TARGET} IMPORTED_LOCATION_${_wl2_bundle_config})
        endif()
    endif()
    if(NOT _wl2_bundle_location)
        foreach(_wl2_bundle_config RELEASE RELWITHDEBINFO MINSIZEREL DEBUG NOCONFIG)
            get_target_property(_wl2_bundle_location ${WL2BUNDLE_TARGET} IMPORTED_LOCATION_${_wl2_bundle_config})
            if(_wl2_bundle_location)
                break()
            endif()
        endforeach()
    endif()
    if(NOT _wl2_bundle_location)
        message(FATAL_ERROR "wl2_module_bundle_dependency(${dep}) target has no IMPORTED_LOCATION: ${WL2BUNDLE_TARGET}")
    endif()
    if(NOT WL2BUNDLE_INCLUDE_DESTINATION)
        set(WL2BUNDLE_INCLUDE_DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    endif()

    string(REPLACE ";" "," _compile_definitions "${WL2BUNDLE_COMPILE_DEFINITIONS}")
    set_property(GLOBAL APPEND PROPERTY WL2_PACKAGE_BUNDLE_LIBRARIES
        "${dep}|${WL2BUNDLE_TARGET}|${_compile_definitions}|${_wl2_bundle_location}")

    if(WL2BUNDLE_INCLUDE_DIR)
        set_property(GLOBAL APPEND PROPERTY WL2_PACKAGE_INSTALL_INCLUDE_DIRS
            "${WL2BUNDLE_INCLUDE_DIR}|${WL2BUNDLE_INCLUDE_DESTINATION}")
    endif()
    foreach(_cmake_file IN LISTS WL2BUNDLE_CMAKE_FILES)
        wl2_module_package_cmake_file("${_cmake_file}")
    endforeach()
endfunction()

function(wl2_add_module_tests module_name)
    set(options)
    set(oneValueArgs DIRECTORY)
    set(multiValueArgs)
    cmake_parse_arguments(WL2TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT WL2_BUILD_TESTING)
        return()
    endif()
    if(NOT WL2TEST_DIRECTORY)
        set(WL2TEST_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test")
    endif()
    if(NOT IS_DIRECTORY "${WL2TEST_DIRECTORY}")
        message(FATAL_ERROR "wl2_add_module_tests(${module_name}) missing directory: ${WL2TEST_DIRECTORY}")
    endif()
    if(NOT EXISTS "${WL2TEST_DIRECTORY}/CMakeLists.txt")
        message(FATAL_ERROR "wl2_add_module_tests(${module_name}) missing CMakeLists.txt in ${WL2TEST_DIRECTORY}")
    endif()

    add_subdirectory("${WL2TEST_DIRECTORY}" "${CMAKE_CURRENT_BINARY_DIR}/test")
endfunction()

function(wl2_add_module module_name)
    set(options NO_DYNAMIC_LINK_LIBRARIES NO_INSTALL)
    set(oneValueArgs
        STATIC_TARGET DYNAMIC_TARGET INCLUDE_DIR
        MODULE_NAME REGISTER_FUNCTION VERSION STABLE_ID FEATURE TEST_DIR DOCS_DIR)
    set(multiValueArgs
        SOURCES
        REQUIRES_MODULES OPTIONAL_MODULES
        PUBLIC_LINK_LIBRARIES PRIVATE_LINK_LIBRARIES
        DYNAMIC_LINK_LIBRARIES DYNAMIC_INCLUDE_DIRS
        PUBLIC_COMPILE_DEFINITIONS PRIVATE_COMPILE_DEFINITIONS
        STATIC_PUBLIC_COMPILE_DEFINITIONS STATIC_PRIVATE_COMPILE_DEFINITIONS
        DYNAMIC_PUBLIC_COMPILE_DEFINITIONS DYNAMIC_PRIVATE_COMPILE_DEFINITIONS)
    cmake_parse_arguments(WL2MOD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Encode each module's dependency name lists as a single pipe-joined token so
    # they stay aligned with the parallel registry arrays (which use ';').
    string(REPLACE ";" "|" _wl2_module_requires_joined "${WL2MOD_REQUIRES_MODULES}")
    string(REPLACE ";" "|" _wl2_module_optional_joined "${WL2MOD_OPTIONAL_MODULES}")

    if(NOT WL2MOD_SOURCES)
        message(FATAL_ERROR "wl2_add_module(${module_name}) requires SOURCES")
    endif()

    if(NOT WL2MOD_STATIC_TARGET)
        set(WL2MOD_STATIC_TARGET "${module_name}_static")
    endif()
    if(NOT WL2MOD_DYNAMIC_TARGET)
        set(WL2MOD_DYNAMIC_TARGET "${module_name}")
    endif()
    if(NOT WL2MOD_INCLUDE_DIR)
        set(WL2MOD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
    endif()
    if(NOT WL2MOD_MODULE_NAME)
        string(REGEX REPLACE "^wl2_" "" _wl2_module_short_name "${module_name}")
        set(WL2MOD_MODULE_NAME "wl2:${_wl2_module_short_name}")
    endif()
    if(NOT WL2MOD_REGISTER_FUNCTION)
        set(WL2MOD_REGISTER_FUNCTION "${module_name}_register_module")
    endif()
    if(NOT WL2MOD_VERSION)
        set(WL2MOD_VERSION "${PROJECT_VERSION}")
    endif()
    if(NOT DEFINED APPBUILD OR "${APPBUILD}" STREQUAL "")
        string(TIMESTAMP APPBUILD "%Y.%m.%d.%H%M")
    endif()
    if(NOT WL2MOD_STABLE_ID)
        set(WL2MOD_STABLE_ID "")
    endif()
    if(NOT WL2MOD_FEATURE)
        string(REGEX REPLACE "^wl2_" "" _wl2_module_feature_name "${module_name}")
        string(TOUPPER "${_wl2_module_feature_name}" _wl2_module_feature_name)
        set(WL2MOD_FEATURE "WL2_ENABLE_${_wl2_module_feature_name}")
    endif()
    if(NOT WL2MOD_TEST_DIR)
        set(WL2MOD_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test")
    endif()
    if(NOT WL2MOD_DOCS_DIR)
        set(WL2MOD_DOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs")
    endif()

    set(_wl2_module_created_targets)
    if(WL2_BUILD_STATIC_MODULES)
        add_library(${WL2MOD_STATIC_TARGET} STATIC ${WL2MOD_SOURCES})
        target_include_directories(${WL2MOD_STATIC_TARGET}
            PUBLIC
                $<BUILD_INTERFACE:${WL2MOD_INCLUDE_DIR}>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        if(WL2MOD_PUBLIC_LINK_LIBRARIES OR WL2MOD_PRIVATE_LINK_LIBRARIES)
            target_link_libraries(${WL2MOD_STATIC_TARGET}
                PUBLIC ${WL2MOD_PUBLIC_LINK_LIBRARIES}
                PRIVATE ${WL2MOD_PRIVATE_LINK_LIBRARIES})
        endif()
        if(WL2MOD_PUBLIC_COMPILE_DEFINITIONS OR WL2MOD_PRIVATE_COMPILE_DEFINITIONS)
            target_compile_definitions(${WL2MOD_STATIC_TARGET}
                PUBLIC ${WL2MOD_PUBLIC_COMPILE_DEFINITIONS}
                PRIVATE ${WL2MOD_PRIVATE_COMPILE_DEFINITIONS})
        endif()
        target_compile_definitions(${WL2MOD_STATIC_TARGET}
            PRIVATE
                WL2_VERSION="${PROJECT_VERSION}"
                WL2_BUILD="${APPBUILD}")
        if(WL2MOD_STATIC_PUBLIC_COMPILE_DEFINITIONS OR WL2MOD_STATIC_PRIVATE_COMPILE_DEFINITIONS)
            target_compile_definitions(${WL2MOD_STATIC_TARGET}
                PUBLIC ${WL2MOD_STATIC_PUBLIC_COMPILE_DEFINITIONS}
                PRIVATE ${WL2MOD_STATIC_PRIVATE_COMPILE_DEFINITIONS})
        endif()
        list(APPEND _wl2_module_created_targets ${WL2MOD_STATIC_TARGET})
        if(NOT WL2MOD_NO_INSTALL)
            # Empty dependency lists are stored as "-" so the parallel arrays stay
            # aligned (APPEND of an empty string is unreliable).
            set(_wl2_static_requires "${_wl2_module_requires_joined}")
            if(_wl2_static_requires STREQUAL "")
                set(_wl2_static_requires "-")
            endif()
            set(_wl2_static_optional "${_wl2_module_optional_joined}")
            if(_wl2_static_optional STREQUAL "")
                set(_wl2_static_optional "-")
            endif()
            set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_TARGETS ${WL2MOD_STATIC_TARGET})
            set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_REGISTRATION_FUNCTIONS ${WL2MOD_REGISTER_FUNCTION})
            set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_NAMES ${WL2MOD_MODULE_NAME})
            set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_REQUIRES "${_wl2_static_requires}")
            set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_OPTIONAL "${_wl2_static_optional}")
        endif()
    endif()

    if(WL2_BUILD_SHARED_MODULES)
        add_library(${WL2MOD_DYNAMIC_TARGET} MODULE ${WL2MOD_SOURCES})
        # The wl2 headers require C++20. A dynamic module does not link wl2_core
        # (the host provides those symbols), so it must request the standard
        # itself rather than inheriting it transitively.
        target_compile_features(${WL2MOD_DYNAMIC_TARGET} PRIVATE cxx_std_20)
        target_include_directories(${WL2MOD_DYNAMIC_TARGET}
            PUBLIC
                $<BUILD_INTERFACE:${WL2MOD_INCLUDE_DIR}>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        if(WL2MOD_DYNAMIC_INCLUDE_DIRS)
            target_include_directories(${WL2MOD_DYNAMIC_TARGET}
                PRIVATE ${WL2MOD_DYNAMIC_INCLUDE_DIRS})
        endif()
        if(NOT WL2MOD_NO_DYNAMIC_LINK_LIBRARIES)
            if(WL2MOD_DYNAMIC_LINK_LIBRARIES)
                target_link_libraries(${WL2MOD_DYNAMIC_TARGET}
                    PRIVATE ${WL2MOD_DYNAMIC_LINK_LIBRARIES})
            elseif(WL2MOD_PUBLIC_LINK_LIBRARIES OR WL2MOD_PRIVATE_LINK_LIBRARIES)
                target_link_libraries(${WL2MOD_DYNAMIC_TARGET}
                    PRIVATE ${WL2MOD_PUBLIC_LINK_LIBRARIES} ${WL2MOD_PRIVATE_LINK_LIBRARIES})
            endif()
        endif()
        if(WL2MOD_PUBLIC_COMPILE_DEFINITIONS OR WL2MOD_PRIVATE_COMPILE_DEFINITIONS)
            target_compile_definitions(${WL2MOD_DYNAMIC_TARGET}
                PUBLIC ${WL2MOD_PUBLIC_COMPILE_DEFINITIONS}
                PRIVATE ${WL2MOD_PRIVATE_COMPILE_DEFINITIONS})
        endif()
        target_compile_definitions(${WL2MOD_DYNAMIC_TARGET}
            PRIVATE
                WL2_VERSION="${PROJECT_VERSION}"
                WL2_BUILD="${APPBUILD}")
        if(WL2MOD_DYNAMIC_PUBLIC_COMPILE_DEFINITIONS OR WL2MOD_DYNAMIC_PRIVATE_COMPILE_DEFINITIONS)
            target_compile_definitions(${WL2MOD_DYNAMIC_TARGET}
                PUBLIC ${WL2MOD_DYNAMIC_PUBLIC_COMPILE_DEFINITIONS}
                PRIVATE ${WL2MOD_DYNAMIC_PRIVATE_COMPILE_DEFINITIONS})
        endif()
        list(APPEND _wl2_module_created_targets ${WL2MOD_DYNAMIC_TARGET})
    endif()

    # NO_INSTALL keeps a module out of the Winglib2 package. Example modules such
    # as examples/modules/wl2_echo use it so they are not installed or exported as
    # part of winglib2.
    if(NOT WL2MOD_NO_INSTALL)
        if(_wl2_module_created_targets)
            set_property(GLOBAL APPEND PROPERTY WL2_MODULE_INSTALL_TARGETS ${_wl2_module_created_targets})
        endif()
        if(EXISTS "${WL2MOD_INCLUDE_DIR}")
            set_property(GLOBAL APPEND PROPERTY WL2_MODULE_HEADER_DIRS "${WL2MOD_INCLUDE_DIR}")
        endif()
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_MODULE_NAMES "${WL2MOD_MODULE_NAME}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_STATIC_TARGETS "${WL2MOD_STATIC_TARGET}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_DYNAMIC_TARGETS "${WL2MOD_DYNAMIC_TARGET}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_REGISTRATION_FUNCTIONS "${WL2MOD_REGISTER_FUNCTION}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_VERSIONS "${WL2MOD_VERSION}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_STABLE_IDS "${WL2MOD_STABLE_ID}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_FEATURES "${WL2MOD_FEATURE}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_TEST_DIRS "${WL2MOD_TEST_DIR}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_DOCS_DIRS "${WL2MOD_DOCS_DIR}")
        set(_wl2_registry_requires "${_wl2_module_requires_joined}")
        if(_wl2_registry_requires STREQUAL "")
            set(_wl2_registry_requires "-")
        endif()
        set(_wl2_registry_optional "${_wl2_module_optional_joined}")
        if(_wl2_registry_optional STREQUAL "")
            set(_wl2_registry_optional "-")
        endif()
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_REQUIRES "${_wl2_registry_requires}")
        set_property(GLOBAL APPEND PROPERTY WL2_MODULE_REGISTRY_OPTIONAL "${_wl2_registry_optional}")
    endif()
endfunction()

# Topologically sort the registered builtin static modules by their declared
# REQUIRES_MODULES/OPTIONAL_MODULES edges so dependencies are linked and
# registered before dependents. Required dependencies that are not enabled
# static modules fail configure; optional dependencies that are absent are
# skipped with a status message; dependency cycles fail configure. Outputs are
# parallel lists in dependency-first order.
function(wl2_resolve_builtin_modules)
    set(options)
    set(oneValueArgs OUT_TARGETS OUT_REGISTRATIONS OUT_NAMES)
    set(multiValueArgs)
    cmake_parse_arguments(WL2RES "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    get_property(_names GLOBAL PROPERTY WL2_STATIC_MODULE_NAMES)
    get_property(_targets GLOBAL PROPERTY WL2_STATIC_MODULE_TARGETS)
    get_property(_registrations GLOBAL PROPERTY WL2_STATIC_MODULE_REGISTRATION_FUNCTIONS)
    get_property(_requires GLOBAL PROPERTY WL2_STATIC_MODULE_REQUIRES)
    get_property(_optional GLOBAL PROPERTY WL2_STATIC_MODULE_OPTIONAL)

    list(LENGTH _names _count)
    list(LENGTH _targets _target_count)
    list(LENGTH _registrations _registration_count)
    list(LENGTH _requires _requires_count)
    list(LENGTH _optional _optional_count)
    if(NOT _count EQUAL _target_count OR NOT _count EQUAL _registration_count
            OR NOT _count EQUAL _requires_count OR NOT _count EQUAL _optional_count)
        message(FATAL_ERROR "Winglib2 static module registry metadata is inconsistent")
    endif()

    # Index per-module metadata by a C-identifier key derived from the name.
    set(_keys)
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_i RANGE ${_last})
            list(GET _names ${_i} _name)
            list(GET _targets ${_i} _target)
            list(GET _registrations ${_i} _reg)
            list(GET _requires ${_i} _req_token)
            list(GET _optional ${_i} _opt_token)
            string(MAKE_C_IDENTIFIER "${_name}" _key)
            list(APPEND _keys ${_key})
            set(_name_${_key} "${_name}")
            set(_target_${_key} "${_target}")
            set(_reg_${_key} "${_reg}")
            set(_present_${_key} TRUE)
            set(_req_list_${_key} "")
            if(NOT _req_token STREQUAL "-")
                string(REPLACE "|" ";" _req_list_${_key} "${_req_token}")
            endif()
            set(_opt_list_${_key} "")
            if(NOT _opt_token STREQUAL "-")
                string(REPLACE "|" ";" _opt_list_${_key} "${_opt_token}")
            endif()
        endforeach()
    endif()

    # Resolve each module's present dependency keys and its in-degree.
    foreach(_key IN LISTS _keys)
        set(_deps)
        foreach(_dep IN LISTS _req_list_${_key})
            string(MAKE_C_IDENTIFIER "${_dep}" _dkey)
            if(NOT DEFINED _present_${_dkey})
                message(FATAL_ERROR
                    "Built-in static module '${_name_${_key}}' requires '${_dep}', "
                    "which is not an enabled built-in static module")
            endif()
            list(APPEND _deps ${_dkey})
        endforeach()
        foreach(_dep IN LISTS _opt_list_${_key})
            string(MAKE_C_IDENTIFIER "${_dep}" _dkey)
            if(DEFINED _present_${_dkey})
                list(APPEND _deps ${_dkey})
            else()
                message(STATUS
                    "Built-in static module '${_name_${_key}}' optional dependency "
                    "'${_dep}' is not enabled; skipping")
            endif()
        endforeach()
        if(_deps)
            list(REMOVE_DUPLICATES _deps)
        endif()
        set(_deps_${_key} "${_deps}")
        list(LENGTH _deps _indeg)
        set(_indeg_${_key} ${_indeg})
    endforeach()

    # Kahn's algorithm with a deterministic tie-break by registration order.
    set(_sorted)
    set(_remaining ${_keys})
    set(_progress TRUE)
    while(_remaining AND _progress)
        set(_progress FALSE)
        set(_next "")
        foreach(_key IN LISTS _keys)
            list(FIND _remaining ${_key} _ridx)
            if(_ridx GREATER -1 AND _indeg_${_key} EQUAL 0)
                set(_next ${_key})
                break()
            endif()
        endforeach()
        if(NOT _next STREQUAL "")
            list(APPEND _sorted ${_next})
            list(REMOVE_ITEM _remaining ${_next})
            set(_progress TRUE)
            foreach(_key IN LISTS _remaining)
                list(FIND _deps_${_key} ${_next} _didx)
                if(_didx GREATER -1)
                    math(EXPR _indeg_${_key} "${_indeg_${_key}} - 1")
                endif()
            endforeach()
        endif()
    endwhile()

    if(_remaining)
        set(_cycle)
        foreach(_key IN LISTS _remaining)
            list(APPEND _cycle "${_name_${_key}}")
        endforeach()
        string(REPLACE ";" ", " _cycle_str "${_cycle}")
        message(FATAL_ERROR "Built-in static module dependency cycle detected among: ${_cycle_str}")
    endif()

    set(_out_targets)
    set(_out_registrations)
    set(_out_names)
    foreach(_key IN LISTS _sorted)
        list(APPEND _out_targets "${_target_${_key}}")
        list(APPEND _out_registrations "${_reg_${_key}}")
        list(APPEND _out_names "${_name_${_key}}")
    endforeach()

    if(WL2RES_OUT_TARGETS)
        set(${WL2RES_OUT_TARGETS} "${_out_targets}" PARENT_SCOPE)
    endif()
    if(WL2RES_OUT_REGISTRATIONS)
        set(${WL2RES_OUT_REGISTRATIONS} "${_out_registrations}" PARENT_SCOPE)
    endif()
    if(WL2RES_OUT_NAMES)
        set(${WL2RES_OUT_NAMES} "${_out_names}" PARENT_SCOPE)
    endif()
endfunction()

function(wl2_generate_static_module_registry)
    set(options)
    set(oneValueArgs OUTPUT OUT_SOURCE OUT_TARGETS)
    set(multiValueArgs)
    cmake_parse_arguments(WL2REG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT WL2REG_OUTPUT)
        message(FATAL_ERROR "wl2_generate_static_module_registry requires OUTPUT")
    endif()

    # Link/register in dependency-first order from the static module graph.
    wl2_resolve_builtin_modules(
        OUT_TARGETS _wl2_static_targets
        OUT_REGISTRATIONS _wl2_static_registrations
        OUT_NAMES _wl2_static_names)

    set(_generated_source "#include \"wl2/module.h\"\n#include \"wl2/runtime.h\"\n\n#include <string>\n#include <utility>\n#include <vector>\n\n")
    foreach(_registration IN LISTS _wl2_static_registrations)
        string(APPEND _generated_source "wl2::ModuleInfo ${_registration}(wl2::Runtime& runtime);\n")
    endforeach()
    string(APPEND _generated_source "\nvoid wl2_register_builtin_static_modules(wl2::RuntimeOptions& options) {\n")
    foreach(_registration IN LISTS _wl2_static_registrations)
        string(APPEND _generated_source "    options.staticModules.push_back(${_registration});\n")
    endforeach()
    string(APPEND _generated_source "}\n\nvoid wl2_append_builtin_static_module_names(std::vector<std::string>& names) {\n")
    foreach(_name IN LISTS _wl2_static_names)
        string(APPEND _generated_source "    names.emplace_back(\"${_name}\");\n")
    endforeach()
    string(APPEND _generated_source "}\n")

    # Emit the CMake-declared dependency names per module so a test can validate
    # them against each module's C++ ModuleInfo dependencies.
    get_property(_decl_names GLOBAL PROPERTY WL2_STATIC_MODULE_NAMES)
    get_property(_decl_requires GLOBAL PROPERTY WL2_STATIC_MODULE_REQUIRES)
    get_property(_decl_optional GLOBAL PROPERTY WL2_STATIC_MODULE_OPTIONAL)
    string(APPEND _generated_source
        "\nvoid wl2_append_builtin_static_module_cmake_dependencies(\n"
        "    std::vector<std::pair<std::string, std::vector<std::string>>>& out) {\n")
    list(LENGTH _decl_names _decl_count)
    if(_decl_count GREATER 0)
        math(EXPR _decl_last "${_decl_count} - 1")
        foreach(_i RANGE ${_decl_last})
            list(GET _decl_names ${_i} _dname)
            list(GET _decl_requires ${_i} _dreq)
            list(GET _decl_optional ${_i} _dopt)
            set(_dep_names)
            if(NOT _dreq STREQUAL "-")
                string(REPLACE "|" ";" _dreq_list "${_dreq}")
                list(APPEND _dep_names ${_dreq_list})
            endif()
            if(NOT _dopt STREQUAL "-")
                string(REPLACE "|" ";" _dopt_list "${_dopt}")
                list(APPEND _dep_names ${_dopt_list})
            endif()
            string(APPEND _generated_source "    {\n        std::vector<std::string> deps;\n")
            foreach(_dep IN LISTS _dep_names)
                string(APPEND _generated_source "        deps.emplace_back(\"${_dep}\");\n")
            endforeach()
            string(APPEND _generated_source
                "        out.emplace_back(\"${_dname}\", std::move(deps));\n    }\n")
        endforeach()
    endif()
    string(APPEND _generated_source "}\n")

    file(WRITE "${WL2REG_OUTPUT}" "${_generated_source}")

    if(WL2REG_OUT_SOURCE)
        set(${WL2REG_OUT_SOURCE} "${WL2REG_OUTPUT}" PARENT_SCOPE)
    endif()
    if(WL2REG_OUT_TARGETS)
        set(${WL2REG_OUT_TARGETS} ${_wl2_static_targets} PARENT_SCOPE)
    endif()
endfunction()
