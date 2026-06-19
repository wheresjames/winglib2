function(wl2_dependency_normalize_mode mode_var allowed_values)
    set(_mode "${${mode_var}}")
    string(TOLOWER "${_mode}" _mode)
    if(_mode STREQUAL "fetch")
        set(_mode "download")
    elseif(_mode STREQUAL "package")
        set(_mode "system")
    endif()
    if(NOT _mode IN_LIST allowed_values)
        message(FATAL_ERROR "${mode_var} must be one of: ${allowed_values}")
    endif()
    set(${mode_var} "${_mode}" PARENT_SCOPE)
endfunction()

function(wl2_dependency_provider_from_mode mode out_var)
    if(mode STREQUAL "download")
        set(${out_var} "fetch" PARENT_SCOPE)
    elseif(mode STREQUAL "system")
        set(${out_var} "package" PARENT_SCOPE)
    else()
        set(${out_var} "${mode}" PARENT_SCOPE)
    endif()
endfunction()

function(wl2_dependency_normalize_provider dep provider_var)
    set(_allowed auto local system download off)
    set(_provider "${${provider_var}}")
    wl2_dependency_normalize_mode(_provider "${_allowed}")
    wl2_dependency_provider_from_mode("${_provider}" _legacy_provider)
    set(${provider_var} "${_legacy_provider}" PARENT_SCOPE)
endfunction()

function(wl2_dependency_configure_global_default)
    set(WL2_DEPS "auto" CACHE STRING "Default dependency mode: auto, local, system, download, or off")
    set_property(CACHE WL2_DEPS PROPERTY STRINGS auto local system download off)

    set(_allowed auto local system download off)
    wl2_dependency_normalize_mode(WL2_DEPS "${_allowed}")
    set(WL2_DEPS "${WL2_DEPS}" CACHE STRING "Default dependency mode: auto, local, system, download, or off" FORCE)

    if(WL2_DEPS STREQUAL "auto" AND WL2_USE_FETCHED_DEPS)
        message(WARNING "WL2_USE_FETCHED_DEPS is deprecated; use -DWL2_DEPS=download")
        set(WL2_DEPS "download" CACHE STRING "Default dependency mode: auto, local, system, download, or off" FORCE)
        set(WL2_DEPS "download")
    elseif(WL2_DEPS STREQUAL "auto" AND DEFINED WL2_FETCH_DEPS AND NOT WL2_FETCH_DEPS)
        message(WARNING "WL2_FETCH_DEPS=OFF is deprecated; use -DWL2_DEPS=system")
        set(WL2_DEPS "system" CACHE STRING "Default dependency mode: auto, local, system, download, or off" FORCE)
        set(WL2_DEPS "system")
    endif()

    set(WL2_DEPS "${WL2_DEPS}" PARENT_SCOPE)
endfunction()

function(wl2_dependency_mode_is_global_default out_var)
    if(WL2_DEPS STREQUAL "auto" AND NOT WL2_USE_FETCHED_DEPS
            AND (NOT DEFINED WL2_FETCH_DEPS OR WL2_FETCH_DEPS))
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(wl2_dependency_configure_provider dep provider_var)
    string(TOUPPER "${dep}" _dep_upper)

    set(_dep_var "WL2_DEPS_${_dep_upper}")
    set(${_dep_var} "inherit" CACHE STRING "${_dep_upper} dependency mode: inherit, auto, local, system, download, or off")
    set_property(CACHE ${_dep_var} PROPERTY STRINGS inherit auto local system download off)

    set(_allowed inherit auto local system download off)
    set(_dep_mode "${${_dep_var}}")
    wl2_dependency_normalize_mode(_dep_mode "${_allowed}")
    set(${_dep_var} "${_dep_mode}" CACHE STRING "${_dep_upper} dependency mode: inherit, auto, local, system, download, or off" FORCE)

    if(_dep_mode STREQUAL "inherit")
        wl2_dependency_mode_is_global_default(_global_is_default)
        if(_global_is_default AND DEFINED ${provider_var}
                AND NOT "${${provider_var}}" STREQUAL ""
                AND NOT "${${provider_var}}" STREQUAL "auto")
            set(_legacy_mode "${${provider_var}}")
            wl2_dependency_normalize_mode(_legacy_mode "auto;local;system;download;off")
            message(WARNING "${provider_var} is deprecated; use -D${_dep_var}=${_legacy_mode}")
            set(_dep_mode "${_legacy_mode}")
        else()
            set(_dep_mode "${WL2_DEPS}")
        endif()
    endif()

    wl2_dependency_provider_from_mode("${_dep_mode}" _legacy_provider)
    unset(${provider_var} CACHE)
    set(${provider_var} "${_legacy_provider}" PARENT_SCOPE)
    set(${_dep_var} "${${_dep_var}}" PARENT_SCOPE)
endfunction()

function(wl2_dependency_note_result dep provider detail)
    set(_entry "${dep}|${provider}|${detail}")
    set_property(GLOBAL APPEND PROPERTY WL2_DEPENDENCY_SUMMARY "${_entry}")
endfunction()

function(wl2_dependency_print_summary)
    get_property(_entries GLOBAL PROPERTY WL2_DEPENDENCY_SUMMARY)
    if(NOT _entries)
        return()
    endif()

    message(STATUS "")
    message(STATUS "Winglib2 dependency summary (WL2_DEPS=${WL2_DEPS}):")
    foreach(_entry IN LISTS _entries)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _dep)
        list(GET _parts 1 _provider)
        list(GET _parts 2 _detail)
        message(STATUS "  ${_dep}: ${_provider} ${_detail}")
    endforeach()
endfunction()

function(wl2_dependency_default_root dep out_var)
    string(TOLOWER "${dep}" _dep_lower)
    set(${out_var} "${WL2_DEPS_ROOT}/${_dep_lower}" PARENT_SCOPE)
endfunction()

function(wl2_dependency_note_package dep provider)
    if(provider STREQUAL "package")
        message(STATUS "Using explicit package/system ${dep} dependency")
    elseif(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "Refusing implicit package/system ${dep} dependency while cross-compiling. "
            "Use -DWL2_${dep}_PROVIDER=package to accept this explicitly, or provide a target-local dependency.")
    else()
        message(WARNING
            "Using package/system ${dep} dependency from provider '${provider}'. "
            "For reproducible embedded builds, prefer a target-local dependency or set -DWL2_${dep}_PROVIDER=package explicitly.")
    endif()
endfunction()

function(wl2_declare_dependency dep)
    set(options)
    set(oneValueArgs
        ROOT_DEFAULT
        VERSION
        URL
        URL_HASH
        FETCH_CMAKE_ARGS
        FIND_CALLBACK)
    set(multiValueArgs)
    cmake_parse_arguments(WL2DEP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(TOUPPER "${dep}" _dep_upper)
    string(TOLOWER "${dep}" _dep_lower)

    if(WL2DEP_ROOT_DEFAULT)
        set(_root_default "${WL2DEP_ROOT_DEFAULT}")
    else()
        wl2_dependency_default_root("${_dep_upper}" _root_default)
    endif()

    set(WL2_${_dep_upper}_ROOT "${_root_default}" CACHE PATH "Path to a target-local ${_dep_lower} install")
    set(WL2_${_dep_upper}_PROVIDER "auto" CACHE STRING "${_dep_lower} provider compatibility value")
    set_property(CACHE WL2_${_dep_upper}_PROVIDER PROPERTY STRINGS auto local package fetch off)
    if(DEFINED WL2DEP_VERSION)
        set(WL2_${_dep_upper}_VERSION "${WL2DEP_VERSION}" CACHE STRING "${_dep_lower} release version for the fetch provider")
    endif()
    if(DEFINED WL2DEP_URL)
        set(WL2_${_dep_upper}_URL "${WL2DEP_URL}" CACHE STRING "${_dep_lower} source archive URL")
    endif()
    if(DEFINED WL2DEP_URL_HASH)
        set(WL2_${_dep_upper}_URL_HASH "${WL2DEP_URL_HASH}" CACHE STRING "${_dep_lower} source archive hash for the fetch provider")
    endif()
    if(DEFINED WL2DEP_FETCH_CMAKE_ARGS)
        set(WL2_${_dep_upper}_FETCH_CMAKE_ARGS "${WL2DEP_FETCH_CMAKE_ARGS}" CACHE STRING "Additional CMake arguments for the ${_dep_lower} fetch provider")
    endif()

    wl2_dependency_configure_provider("${_dep_upper}" WL2_${_dep_upper}_PROVIDER)
    set(WL2_${_dep_upper}_FOUND FALSE)
    set(WL2_${_dep_upper}_TARGET "")
    set(WL2_${_dep_upper}_IS_SYSTEM FALSE)
    set(WL2_${_dep_upper}_PROVIDER_USED "")

    if(WL2_${_dep_upper}_PROVIDER STREQUAL "off")
        message(STATUS "${_dep_lower} dependency disabled")
    elseif(WL2DEP_FIND_CALLBACK)
        cmake_language(CALL
            ${WL2DEP_FIND_CALLBACK}
            WL2_${_dep_upper}_FOUND
            WL2_${_dep_upper}_TARGET
            WL2_${_dep_upper}_IS_SYSTEM
            WL2_${_dep_upper}_PROVIDER_USED)
    endif()

    set(WL2_${_dep_upper}_FOUND "${WL2_${_dep_upper}_FOUND}" PARENT_SCOPE)
    set(WL2_${_dep_upper}_TARGET "${WL2_${_dep_upper}_TARGET}" PARENT_SCOPE)
    set(WL2_${_dep_upper}_IS_SYSTEM "${WL2_${_dep_upper}_IS_SYSTEM}" PARENT_SCOPE)
    set(WL2_${_dep_upper}_PROVIDER_USED "${WL2_${_dep_upper}_PROVIDER_USED}" PARENT_SCOPE)
    set(WL2_HAVE_${_dep_upper} "${WL2_${_dep_upper}_FOUND}" PARENT_SCOPE)
endfunction()
