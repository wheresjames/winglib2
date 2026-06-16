function(wl2_dependency_normalize_provider dep provider_var)
    set(_provider "${${provider_var}}")
    string(TOLOWER "${_provider}" _provider)
    set(_allowed auto local package fetch off)
    if(NOT _provider IN_LIST _allowed)
        message(FATAL_ERROR "${provider_var} must be one of: ${_allowed}")
    endif()
    set(${provider_var} "${_provider}" PARENT_SCOPE)
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
    set(WL2_${_dep_upper}_PROVIDER "auto" CACHE STRING "${_dep_lower} provider: auto, local, package, fetch, or off")
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

    wl2_dependency_normalize_provider("${_dep_upper}" WL2_${_dep_upper}_PROVIDER)
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
