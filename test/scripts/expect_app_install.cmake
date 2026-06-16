# CLI checks for source app install, app scopes, launchers, resolution, and
# uninstall behavior.
#
# Required -D variables:
#   WL2_EXECUTABLE  Path to the wl2 runner.
#   MAIN_BUILD_DIR Winglib2 build directory to install from.
#   WORK_DIR       Scratch directory.
#   GENERATOR      CMake generator.
# Optional:
#   BUILD_TYPE     Configuration for multi-config generators.

foreach(_required WL2_EXECUTABLE MAIN_BUILD_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run_step desc)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(_combined "${_out}${_err}")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${desc} failed (${_rc})\n${_combined}")
    endif()
    set(RUN_STEP_OUTPUT "${_combined}" PARENT_SCOPE)
endfunction()

function(run_expect desc expect_rc work_dir out_var)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${work_dir}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(_combined "${_out}${_err}")
    if(expect_rc STREQUAL "ok" AND NOT _rc EQUAL 0)
        message(FATAL_ERROR "${desc} should succeed but failed (${_rc})\n${_combined}")
    endif()
    if(expect_rc STREQUAL "fail" AND _rc EQUAL 0)
        message(FATAL_ERROR "${desc} should fail but succeeded\n${_combined}")
    endif()
    set(${out_var} "${_combined}" PARENT_SCOPE)
endfunction()

set(_install_dir "${WORK_DIR}/install")
set(_install_command "${CMAKE_COMMAND}" --install "${MAIN_BUILD_DIR}" --prefix "${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _install_command --config "${BUILD_TYPE}")
endif()
run_step("install winglib2 package" ${_install_command})

set(_project "${WORK_DIR}/project")
file(MAKE_DIRECTORY "${_project}")
set(ENV{XDG_DATA_HOME} "${WORK_DIR}/xdg")
set(ENV{WL2_SYSTEM_APP_DIR} "${WORK_DIR}/system/apps")
set(ENV{WL2_PACKAGE_PREFIX} "${_install_dir}")

set(_repo "${WORK_DIR}/repo")
file(MAKE_DIRECTORY "${_repo}/apps/hello_app/files")
file(WRITE "${_repo}/apps/hello_app/files/main.js"
    "console.log('installed hello app', wl2.runtime.argv.join(','));\n")
file(WRITE "${_repo}/apps/hello_app/wl2.yml"
    "schema: wl2.project.v1\n"
    "prefix: wl2:/app\n"
    "root: files\n"
    "entry: main.js\n"
    "resources:\n"
    "  store:\n"
    "    files:\n"
    "      - main.js\n")
file(WRITE "${_repo}/apps/hello_app/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(hello_app LANGUAGES CXX)\n"
    "find_package(winglib2 CONFIG REQUIRED)\n"
    "wl2_add_javascript_executable(hello_app\n"
    "    MANIFEST \${CMAKE_CURRENT_SOURCE_DIR}/wl2.yml)\n")

find_program(_git git REQUIRED)
run_step("git init app repo" "${_git}" -C "${_repo}" init -q)
run_step("git config email" "${_git}" -C "${_repo}" config user.email test@example.com)
run_step("git config name" "${_git}" -C "${_repo}" config user.name Test)
run_step("git add" "${_git}" -C "${_repo}" add apps/hello_app)
run_step("git commit" "${_git}" -C "${_repo}" commit -q -m initial)
run_step("git tag" "${_git}" -C "${_repo}" tag v1.0.0)

# Missing prerequisites fail before scope directories are written.
set(_missing_prereq_project "${WORK_DIR}/missing-prereq")
file(MAKE_DIRECTORY "${_missing_prereq_project}")
run_expect("missing prerequisite" "fail" "${_missing_prereq_project}" _out
    "${CMAKE_COMMAND}" -E env "PATH=/no-such-tools" "WL2_PACKAGE_PREFIX=${_install_dir}"
    "${WL2_EXECUTABLE}" app install "${_repo}#v1.0.0:apps/hello_app" --scope local)
if(NOT _out MATCHES "app_prerequisite_missing")
    message(FATAL_ERROR "missing prerequisite did not fail clearly\n${_out}")
endif()
if(EXISTS "${_missing_prereq_project}/.wl2/apps")
    message(FATAL_ERROR "missing prerequisite wrote app scope data")
endif()

run_expect("user app install" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app install "${_repo}#v1.0.0:apps/hello_app" --scope user)
if(NOT EXISTS "$ENV{XDG_DATA_HOME}/wl2/apps/hello_app/wl2.app.yml")
    message(FATAL_ERROR "user-scope app metadata missing")
endif()
if(NOT EXISTS "$ENV{XDG_DATA_HOME}/wl2/apps/bin/hello_app")
    message(FATAL_ERROR "user-scope app launcher missing")
endif()

run_expect("app list user" "ok" "${_project}" _out "${WL2_EXECUTABLE}" app list --scope user)
if(NOT _out MATCHES "hello_app" OR NOT _out MATCHES "\\[user\\]")
    message(FATAL_ERROR "app list --scope user did not report install\n${_out}")
endif()

run_expect("app run user" "ok" "${_project}" _out "${WL2_EXECUTABLE}" app run hello_app -- from-user)
if(NOT _out MATCHES "installed hello app")
    message(FATAL_ERROR "installed user app did not run\n${_out}")
endif()

run_expect("local app install" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app install "${_repo}#v1.0.0:apps/hello_app" --scope local)
if(NOT EXISTS "${_project}/.wl2/apps/hello_app/wl2.app.yml")
    message(FATAL_ERROR "local app metadata missing")
endif()

run_expect("app list all shadow" "ok" "${_project}" _out "${WL2_EXECUTABLE}" app list --scope all)
if(NOT _out MATCHES "hello_app" OR NOT _out MATCHES "shadowed")
    message(FATAL_ERROR "app list did not report shadowed lower-priority app\n${_out}")
endif()

run_expect("app run local preferred" "ok" "${_project}" _out "${WL2_EXECUTABLE}" app run hello_app -- from-local)
if(NOT _out MATCHES "installed hello app")
    message(FATAL_ERROR "local app did not run by name\n${_out}")
endif()

run_expect("explicit launcher run" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app run "$ENV{XDG_DATA_HOME}/wl2/apps/bin/hello_app" -- explicit)
if(NOT _out MATCHES "installed hello app")
    message(FATAL_ERROR "explicit launcher did not run\n${_out}")
endif()

# App uninstall leaves installed modules alone.
file(MAKE_DIRECTORY "${_project}/.wl2/modules/shared_module")
file(WRITE "${_project}/.wl2/modules/shared_module/keep.txt" "module stays\n")

run_expect("local app uninstall preserves cache" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app uninstall hello_app --scope local)
if(EXISTS "${_project}/.wl2/apps/hello_app")
    message(FATAL_ERROR "local app payload was not removed")
endif()
if(EXISTS "${_project}/.wl2/apps/bin/hello_app")
    message(FATAL_ERROR "local app launcher was not removed")
endif()
if(NOT EXISTS "${_project}/.wl2/apps/.cache/hello_app")
    message(FATAL_ERROR "app cache should be preserved by default")
endif()
if(NOT EXISTS "${_project}/.wl2/modules/shared_module/keep.txt")
    message(FATAL_ERROR "app uninstall removed shared module data")
endif()

run_expect("local app reinstall" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app install "${_repo}#v1.0.0:apps/hello_app" --scope local)
run_expect("local app purge uninstall" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app uninstall hello_app --scope local --purge-cache)
if(EXISTS "${_project}/.wl2/apps/.cache/hello_app")
    message(FATAL_ERROR "--purge-cache did not remove app cache")
endif()

run_expect("user app uninstall" "ok" "${_project}" _out
    "${WL2_EXECUTABLE}" app uninstall hello_app --scope user --purge-cache)
if(EXISTS "$ENV{XDG_DATA_HOME}/wl2/apps/hello_app")
    message(FATAL_ERROR "user app payload was not removed")
endif()

message(STATUS "app install CLI checks passed")
