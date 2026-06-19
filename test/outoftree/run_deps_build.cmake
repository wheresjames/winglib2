# Out-of-tree source-dependency build/install test driver.
#
# Proves a project can declare a source-dependency module, then lock/fetch/build/
# install it AND its transitive module dependency locally, then run an app that
# requires only the top module while the dependency is selected automatically.
#
# It installs the just-built Winglib2 into a throwaway prefix, generates two
# minimal out-of-tree module repositories (wl2_app_mod -> wl2_base) as local git
# repos, then drives `wl2 deps lock/fetch/build/install/status` and `wl2 run`.
#
# Required -D variables:
#   CMAKE_COMMAND_PATH  Path to cmake.
#   MAIN_BUILD_DIR      Winglib2 build directory to install from.
#   WL2_EXECUTABLE      Path to the built wl2 runner.
#   WORK_DIR            Scratch directory.
#   GENERATOR           CMake generator for the module builds.
# Optional:
#   BUILD_TYPE          Configuration for install/build.

foreach(_required CMAKE_COMMAND_PATH MAIN_BUILD_DIR WL2_EXECUTABLE WORK_DIR GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

include(ProcessorCount)
ProcessorCount(_parallel_jobs)
if(_parallel_jobs EQUAL 0)
    set(_parallel_jobs 1)
endif()

find_program(_git git)
if(NOT _git)
    message(STATUS "git not found; skipping deps build/install test")
    return()
endif()

set(_install_dir "${WORK_DIR}/install")
set(_repos "${WORK_DIR}/repos")
set(_project "${WORK_DIR}/project")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${_repos}")
file(MAKE_DIRECTORY "${_project}/files")

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

function(run_in_project description out_var)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${_project}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${description} failed (${_rc})\n${_out}${_err}")
    endif()
    set(${out_var} "${_out}${_err}" PARENT_SCOPE)
endfunction()

# --- 1. Install Winglib2 into the throwaway prefix --------------------------
set(_install_command "${CMAKE_COMMAND}" --install "${MAIN_BUILD_DIR}" --prefix "${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _install_command --config "${BUILD_TYPE}")
endif()
run_step("install winglib2" ${_install_command})

# --- 2. Generate two out-of-tree module repositories ------------------------
# The module C++ source is minimal: it provides the dynamic ABI metadata (and a
# static register function) without registering a JavaScript module, which is
# enough to build, install, validate, and load it through the module graph.
set(_module_cmakelists [=[
cmake_minimum_required(VERSION 3.24)
project(@NAME@ VERSION 1.0.0 LANGUAGES CXX)
find_package(winglib2 CONFIG REQUIRED)
set(WL2_BUILD_STATIC_MODULES OFF)
set(WL2_BUILD_SHARED_MODULES ON)
wl2_add_module(@NAME@
    MODULE_NAME @CANON@
    NO_DYNAMIC_LINK_LIBRARIES
    SOURCES src/@NAME@.cpp
    PUBLIC_LINK_LIBRARIES winglib2::wl2_core
    DYNAMIC_INCLUDE_DIRS "$<TARGET_PROPERTY:winglib2::wl2_core,INTERFACE_INCLUDE_DIRECTORIES>"
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")
]=])

set(_module_source [=[
#include "wl2/module.h"
namespace { void* factory(void*, const char*) { return nullptr; } }
@ABI_DECL@
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) return 1;
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "@CANON@";
    out->version = "1.0.0";
    // No specific host version requirement for this fixture module.
    out->required_wl2_version = nullptr;
    @ABI_ASSIGN@
    return 0;
}
extern "C" int wl2_module_register(const wl2_module_host* host) {
    if (!host || !host->register_quickjs_module) return 1;
    host->register_quickjs_module(host->host, "@CANON@", &factory);
    return 0;
}
]=])

# repo_dir, NAME, CANON, ABI_DECL, ABI_ASSIGN, SOURCE_YML
function(make_module_repo repo_dir NAME CANON ABI_DECL ABI_ASSIGN SOURCE_YML)
    file(MAKE_DIRECTORY "${repo_dir}/src")
    # A tracked header so the module's include directory exists after checkout.
    file(WRITE "${repo_dir}/include/${NAME}/${NAME}.h" "#pragma once\n")
    string(CONFIGURE "${_module_cmakelists}" _cml @ONLY)
    file(WRITE "${repo_dir}/CMakeLists.txt" "${_cml}")
    string(CONFIGURE "${_module_source}" _src @ONLY)
    file(WRITE "${repo_dir}/src/${NAME}.cpp" "${_src}")
    file(WRITE "${repo_dir}/wl2.module.source.yml" "${SOURCE_YML}")
    run_step("git init ${NAME}" ${_git} -C "${repo_dir}" init -q)
    run_step("git config email" ${_git} -C "${repo_dir}" config user.email test@example.com)
    run_step("git config name" ${_git} -C "${repo_dir}" config user.name Test)
    run_step("git add ${NAME}" ${_git} -C "${repo_dir}" add -A)
    run_step("git commit ${NAME}" ${_git} -C "${repo_dir}" commit -q -m initial)
    run_step("git tag ${NAME}" ${_git} -C "${repo_dir}" tag v1.0.0)
endfunction()

set(_base_repo "${_repos}/wl2_base")
set(_app_repo "${_repos}/wl2_app_mod")

make_module_repo("${_base_repo}" wl2_base "wl2:base" "" ""
    "schema: wl2.module-source.v1\nprovides: wl2:base\nversion: 1.0.0\n")

# wl2_app_mod requires wl2:base in its ABI metadata and declares wl2_base as a
# transitive source dependency.
set(_app_abi_decl "static const wl2_module_dependency_info kDeps[] = { {\"wl2:base\", \"\", nullptr, 1} };")
set(_app_abi_assign "out->dependencies = kDeps; out->dependency_count = 1;")
make_module_repo("${_app_repo}" wl2_app_mod "wl2:appmod" "${_app_abi_decl}" "${_app_abi_assign}"
    "schema: wl2.module-source.v1\nprovides: wl2:appmod\nversion: 1.0.0\ndependencies:\n  required:\n    - name: wl2:base\nsourceDependencies:\n  modules:\n    - name: wl2_base\n      provides: wl2:base\n      git: ${_base_repo}\n      tag: v1.0.0\n")

# --- 3. Project that declares the source dependency -------------------------
file(WRITE "${_project}/files/main.js" "console.log(\"ran ok\");\n")
file(WRITE "${_project}/wl2.yml"
    "schema: wl2.project.v1\nprefix: wl2:/app\nroot: files\nentry: main.js\nmodules:\n  require:\n    - wl2:appmod\ndependencies:\n  modules:\n    - name: wl2_app_mod\n      provides: wl2:appmod\n      git: ${_app_repo}\n      tag: v1.0.0\n")

# --- 4. lock: resolves and includes the transitive source dependency --------
run_in_project("deps lock" _out "${WL2_EXECUTABLE}" deps lock)
file(READ "${_project}/wl2.lock.yml" _lock)
if(NOT _lock MATCHES "wl2_app_mod" OR NOT _lock MATCHES "wl2_base")
    message(FATAL_ERROR "lock did not include the transitive dependency\n${_lock}")
endif()

# --- 5. fetch ---------------------------------------------------------------
run_in_project("deps fetch" _out "${WL2_EXECUTABLE}" deps fetch)
if(NOT IS_DIRECTORY "${_project}/.wl2/deps/wl2_base/.git"
        OR NOT IS_DIRECTORY "${_project}/.wl2/deps/wl2_app_mod/.git")
    message(FATAL_ERROR "fetch did not populate both dependencies")
endif()

# --- 6. build (dependency-first) --------------------------------------------
# `wl2 deps build` shells out to `cmake --build`, which honors this env var, so
# the nested module builds run in parallel without a dedicated CLI flag.
set(ENV{CMAKE_BUILD_PARALLEL_LEVEL} "${_parallel_jobs}")
set(_build_command "${WL2_EXECUTABLE}" deps build --prefix "${_install_dir}" --generator "${GENERATOR}")
if(BUILD_TYPE)
    list(APPEND _build_command --build-type "${BUILD_TYPE}")
endif()
run_in_project("deps build" _out ${_build_command})

# --- 7. install into the local project scope --------------------------------
run_in_project("deps install" _out "${WL2_EXECUTABLE}" deps install)
if(NOT EXISTS "${_project}/.wl2/modules/wl2_base/wl2.module.yml"
        OR NOT EXISTS "${_project}/.wl2/modules/wl2_appmod/wl2.module.yml")
    message(FATAL_ERROR "deps install did not install both modules locally\n${_out}")
endif()

# --- 8. status reports built and installed for both -------------------------
run_in_project("deps status" _out "${WL2_EXECUTABLE}" deps status)
if(NOT _out MATCHES "wl2_app_mod.*built=yes.*installed=yes"
        AND NOT _out MATCHES "built=yes")
    message(FATAL_ERROR "deps status did not report built/installed state\n${_out}")
endif()

# --- 9. run: requiring only wl2:appmod loads its dependency automatically ----
run_in_project("run after install" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
if(NOT _out MATCHES "ran ok")
    message(FATAL_ERROR "app run did not succeed with installed source dependencies\n${_out}")
endif()

message(STATUS "deps build/install out-of-tree test passed")
