# yaml-cpp is used only by the wl2_yml module. It is vendored: a pinned release
# is downloaded and its sources are compiled directly into the module target
# rather than added as a separate CMake project. That keeps yaml-cpp fully
# private to this module (no extra exported target, no install rules, and no
# `uninstall` target that would collide with winglib2's) and needs no packaging
# plumbing.
#
# wl2_find_yaml_cpp() sets, in the caller's scope:
#   WL2_HAVE_YAMLCPP          - TRUE when the sources were populated
#   WL2_YAMLCPP_SOURCES       - list of yaml-cpp .cpp files to compile
#   WL2_YAMLCPP_INCLUDE_DIR   - public header directory (<yaml-cpp/...>)
#   WL2_YAMLCPP_SRC_DIR       - private source directory (internal "..." headers)
include_guard(GLOBAL)

include(FetchContent)

function(wl2_find_yaml_cpp)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)

    set(WL2_HAVE_YAMLCPP FALSE PARENT_SCOPE)

    if(WL2_YAMLCPP_PROVIDER STREQUAL "off")
        message(STATUS "yaml-cpp provider is off; wl2_yml module disabled")
        return()
    endif()

    set(FETCHCONTENT_BASE_DIR "${WL2_DEPS_ROOT}/_fetch" CACHE PATH "FetchContent base directory" FORCE)

    # Point SOURCE_SUBDIR at yaml-cpp's `src` directory (which has no
    # CMakeLists.txt) so FetchContent_MakeAvailable extracts the release without
    # calling add_subdirectory() on yaml-cpp's own project.
    message(STATUS "Fetching yaml-cpp ${WL2_YAMLCPP_VERSION} from ${WL2_YAMLCPP_URL}")
    if(WL2_YAMLCPP_URL_HASH)
        FetchContent_Declare(yaml_cpp_source
            URL "${WL2_YAMLCPP_URL}"
            URL_HASH "${WL2_YAMLCPP_URL_HASH}"
            SOURCE_SUBDIR src
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    else()
        FetchContent_Declare(yaml_cpp_source
            URL "${WL2_YAMLCPP_URL}"
            SOURCE_SUBDIR src
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_MakeAvailable(yaml_cpp_source)

    set(_src "${yaml_cpp_source_SOURCE_DIR}")
    # Core library sources only; the optional graph-builder lives under
    # src/contrib and is not part of the public API this module exposes.
    file(GLOB _yaml_sources CONFIGURE_DEPENDS "${_src}/src/*.cpp")

    set(WL2_HAVE_YAMLCPP TRUE PARENT_SCOPE)
    set(WL2_YAMLCPP_SOURCES "${_yaml_sources}" PARENT_SCOPE)
    set(WL2_YAMLCPP_INCLUDE_DIR "${_src}/include" PARENT_SCOPE)
    set(WL2_YAMLCPP_SRC_DIR "${_src}/src" PARENT_SCOPE)
    message(STATUS "Using vendored yaml-cpp ${WL2_YAMLCPP_VERSION}")
endfunction()
