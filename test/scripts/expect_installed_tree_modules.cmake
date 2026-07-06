# Installed-tree dynamic module smoke test.
#
# Installs the build tree into a throwaway prefix, then proves that the
# installed bin/wl2 resolves a manifest-required module from its installed
# dynamic module store with no --load-module:
#   1. installed run (default policy) succeeds,
#   2. installed-only policy succeeds (no project/user/system participation),
#   3. the whole prefix relocates and still works (executable-relative lookup),
#   4. a tampered payload fails with module_checksum_mismatch.
# HOME/XDG/system-store environment overrides isolate the run from any user or
# system module state.

foreach(_required CMAKE_COMMAND_PATH MAIN_BUILD_DIR WORK_DIR INSTALL_LIBDIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_prefix "${WORK_DIR}/prefix")
set(_install_args --install "${MAIN_BUILD_DIR}" --prefix "${_prefix}")
if(DEFINED BUILD_TYPE AND NOT "${BUILD_TYPE}" STREQUAL "")
    list(APPEND _install_args --config "${BUILD_TYPE}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}" ${_install_args}
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_out
    ERROR_VARIABLE _install_err)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "install failed:\n${_install_out}\n${_install_err}")
endif()

set(_store "${_prefix}/${INSTALL_LIBDIR}/wl2/modules")
if(NOT EXISTS "${_store}/index.yml")
    message(FATAL_ERROR "installed module store has no index: ${_store}/index.yml")
endif()
if(NOT IS_DIRECTORY "${_store}/wl2_json")
    message(FATAL_ERROR "installed module store is missing wl2:json: ${_store}/wl2_json")
endif()
# Static archives legitimately install to the libdir; only dynamic payloads
# (.so/.dylib/.dll) must be confined to the module store.
file(GLOB _dynamic_libs_in_libdir
    "${_prefix}/${INSTALL_LIBDIR}/*wl2_json*.so"
    "${_prefix}/${INSTALL_LIBDIR}/*wl2_json*.dylib"
    "${_prefix}/${INSTALL_LIBDIR}/*wl2_json*.dll")
if(_dynamic_libs_in_libdir)
    message(FATAL_ERROR
        "dynamic module payloads must live only under the module store, found: ${_dynamic_libs_in_libdir}")
endif()

# Isolate the run from the developer's user/system module stores.
file(MAKE_DIRECTORY "${WORK_DIR}/home")
file(MAKE_DIRECTORY "${WORK_DIR}/xdg-data")
file(MAKE_DIRECTORY "${WORK_DIR}/xdg-config")
file(MAKE_DIRECTORY "${WORK_DIR}/system-modules")
set(ENV{HOME} "${WORK_DIR}/home")
set(ENV{XDG_DATA_HOME} "${WORK_DIR}/xdg-data")
set(ENV{XDG_CONFIG_HOME} "${WORK_DIR}/xdg-config")
set(ENV{WL2_SYSTEM_MODULE_DIR} "${WORK_DIR}/system-modules")

set(_app_dir "${WORK_DIR}/app")
file(MAKE_DIRECTORY "${_app_dir}")
file(WRITE "${_app_dir}/main.js" [=[
import { stringify } from "wl2:json";

if (stringify({ ok: true }) !== "{\"ok\":true}") {
  throw new Error("wl2:json did not load from the installed store");
}

console.log("installed dynamic module load ok");
]=])
file(WRITE "${_app_dir}/wl2.yml" [=[
schema: wl2.resources.v1
prefix: wl2:/installed-test
root: .
entry: main.js

modules:
  require:
    - wl2:json

resources:
  store:
    files:
      - main.js
]=])

function(_run_installed_wl2 wl2_path expect_success label)
    set(_extra_args ${ARGN})
    execute_process(
        COMMAND "${wl2_path}" run --manifest "${_app_dir}/wl2.yml" --no-permission-prompt ${_extra_args}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(expect_success)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "${label} failed:\n${_out}\n${_err}")
        endif()
        if(NOT _out MATCHES "installed dynamic module load ok")
            message(FATAL_ERROR "${label} did not produce expected output:\n${_out}\n${_err}")
        endif()
    else()
        if(_result EQUAL 0)
            message(FATAL_ERROR "${label} unexpectedly succeeded:\n${_out}\n${_err}")
        endif()
        set(_failure_output "${_out}\n${_err}" PARENT_SCOPE)
    endif()
endfunction()

set(_wl2 "${_prefix}/bin/wl2")
if(NOT EXISTS "${_wl2}")
    message(FATAL_ERROR "installed wl2 runner not found: ${_wl2}")
endif()

_run_installed_wl2("${_wl2}" TRUE "installed run (default policy)")
_run_installed_wl2("${_wl2}" TRUE "installed run (installed-only policy)"
    --module-policy installed-only)

# Relocate the whole prefix: the compiled install prefix is now stale, so a
# successful run proves executable-relative store discovery.
set(_moved_prefix "${WORK_DIR}/prefix-moved")
file(RENAME "${_prefix}" "${_moved_prefix}")
_run_installed_wl2("${_moved_prefix}/bin/wl2" TRUE "relocated installed run"
    --module-policy installed-only)
file(RENAME "${_moved_prefix}" "${_prefix}")

# Tamper with the installed payload: the recorded checksum must reject it.
set(_json_dir "${_store}/wl2_json")
file(GLOB _json_libs "${_json_dir}/*wl2_json*")
list(FILTER _json_libs EXCLUDE REGEX "\\.yml$")
list(LENGTH _json_libs _json_lib_count)
if(_json_lib_count EQUAL 0)
    message(FATAL_ERROR "no wl2:json payload found in ${_json_dir}")
endif()
list(GET _json_libs 0 _json_lib)
file(APPEND "${_json_lib}" "tampered")
_run_installed_wl2("${_wl2}" FALSE "tampered installed run"
    --module-policy installed-only)
if(NOT _failure_output MATCHES "module_checksum_mismatch|does not match its recorded checksum")
    message(FATAL_ERROR
        "tampered module failed with unexpected diagnostic:\n${_failure_output}")
endif()

message(STATUS "installed-tree dynamic module smoke test passed")
