foreach(_required CMAKE_COMMAND_PATH MAIN_SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

# Build in parallel; this driver does a full from-scratch winglib2 build, which
# is otherwise serial under single-config generators. Use an explicit job count
# so `--parallel` never expands to an unbounded `make -j`.
include(ProcessorCount)
ProcessorCount(_parallel_jobs)
if(_parallel_jobs EQUAL 0)
    set(_parallel_jobs 1)
endif()

set(_modules_root "${WORK_DIR}/modules")
set(_module_dir "${_modules_root}/wl2_extdep")
set(_build_dir "${WORK_DIR}/build")
set(_install_dir "${WORK_DIR}/install")
set(_consumer_dir "${WORK_DIR}/consumer")
set(_consumer_build_dir "${WORK_DIR}/consumer-build")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY
    "${_module_dir}/cmake"
    "${_module_dir}/include/wl2_extdep"
    "${_module_dir}/src"
    "${_module_dir}/third_party/tinydep/include/tinydep"
    "${_consumer_dir}")

function(run_step description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${description} failed (${_rc})\n--- stdout ---\n${_out}\n--- stderr ---\n${_err}")
    endif()
endfunction()

file(WRITE "${_module_dir}/wl2.module.source.yml"
    "schema: 1\nprovides: wl2:extdep\nversion: 1.0.0\ncategory: extended\n")

file(WRITE "${_module_dir}/cmake/options.cmake"
    "wl2_module_option(ENABLE DOC \"Build wl2_extdep fixture module\")\n")

file(WRITE "${_module_dir}/third_party/tinydep/include/tinydep/tinydep.h"
    "#pragma once\ninline int tinydep_magic() { return 42; }\n")

file(WRITE "${_module_dir}/include/wl2_extdep/wl2_extdep.h"
    "#pragma once\n#include \"wl2/module.h\"\nwl2::ModuleInfo wl2_extdep_register_module(wl2::Runtime& runtime);\n")

file(WRITE "${_module_dir}/src/wl2_extdep.cpp" [=[
#include "wl2_extdep/wl2_extdep.h"
#include "tinydep/tinydep.h"

#include "wl2/runtime.h"

#include <string>

wl2::ModuleInfo wl2_extdep_register_module(wl2::Runtime& runtime) {
    (void)runtime;
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:extdep",
        .version = "1.0.0",
        .stableId = "2c3d74e2-a6a8-49af-8a0d-7f2a85bc5f83",
        .summary = "Out-of-tree extended fixture using tinydep " + std::to_string(tinydep_magic()),
        .api = "Fixture module.",
        .unloadSafe = true,
    };
}
]=])

file(WRITE "${_module_dir}/CMakeLists.txt" [=[
include(cmake/options.cmake)

if(NOT WL2_ENABLE_EXTDEP)
    return()
endif()

wl2_add_module(wl2_extdep
    MODULE_NAME wl2:extdep
    REGISTER_FUNCTION wl2_extdep_register_module
    STABLE_ID 2c3d74e2-a6a8-49af-8a0d-7f2a85bc5f83
    FEATURE WL2_ENABLE_EXTDEP
    SOURCES src/wl2_extdep.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_EXTDEP_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")

foreach(_target wl2_extdep_static wl2_extdep)
    if(TARGET ${_target})
        target_include_directories(${_target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinydep/include")
    endif()
endforeach()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinydep/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET tinydep::tinydep)
    add_library(tinydep::tinydep INTERFACE IMPORTED)
    set_target_properties(tinydep::tinydep PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include")
endif()
]==])
]=])

file(WRITE "${_consumer_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(wl2_extdep_consumer LANGUAGES CXX)
find_package(winglib2 CONFIG REQUIRED)
add_executable(wl2_extdep_consumer main.cpp)
target_link_libraries(wl2_extdep_consumer PRIVATE winglib2::wl2_core winglib2::wl2_extdep_static)
]=])

file(WRITE "${_consumer_dir}/main.cpp" [=[
#include "wl2_extdep/wl2_extdep.h"
#include "tinydep/tinydep.h"

int main() {
    return tinydep_magic() == 42 ? 0 : 1;
}
]=])

set(_config_args
    "${CMAKE_COMMAND_PATH}" -S "${MAIN_SOURCE_DIR}" -B "${_build_dir}" -G "${GENERATOR}"
    "-DWL2_EXTRA_MODULE_DIRS=${_modules_root}"
    "-DWL2_BUILD_EXAMPLES=OFF"
    "-DWL2_BUILD_TESTING=OFF"
    "-DWL2_BUILD_DOCS=OFF")
if(BUILD_TYPE)
    list(APPEND _config_args "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
run_step("configure winglib2 with out-of-tree module" ${_config_args})

set(_build_args "${CMAKE_COMMAND_PATH}" --build "${_build_dir}" --parallel ${_parallel_jobs})
set(_install_args "${CMAKE_COMMAND_PATH}" --install "${_build_dir}" --prefix "${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _build_args --config "${BUILD_TYPE}")
    list(APPEND _install_args --config "${BUILD_TYPE}")
endif()
run_step("build winglib2 with out-of-tree module" ${_build_args})
run_step("install winglib2 with out-of-tree module" ${_install_args})

set(_consumer_config_args
    "${CMAKE_COMMAND_PATH}" -S "${_consumer_dir}" -B "${_consumer_build_dir}" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _consumer_config_args "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
run_step("configure consumer" ${_consumer_config_args})

set(_consumer_build_args "${CMAKE_COMMAND_PATH}" --build "${_consumer_build_dir}" --parallel ${_parallel_jobs})
if(BUILD_TYPE)
    list(APPEND _consumer_build_args --config "${BUILD_TYPE}")
endif()
run_step("build consumer" ${_consumer_build_args})

set(_exe "${_consumer_build_dir}/wl2_extdep_consumer")
if(WIN32 AND BUILD_TYPE)
    set(_exe "${_consumer_build_dir}/${BUILD_TYPE}/wl2_extdep_consumer.exe")
endif()
run_step("run consumer" "${_exe}")

message(STATUS "out-of-tree extended module package test passed")
