# Linkage-mode build-and-run test.
#
# Configures this source tree in a throwaway build directory with
# WL2_MODULE_LINKAGE=${LINKAGE} and a minimal module set (wl2:json only),
# builds the wl2 runner, and proves the mode's module path end to end:
#   * static:  the builtin runner satisfies a manifest-required module with no
#              dynamic payload staged anywhere.
#   * dynamic: the runner resolves the staged store payload, and hiding that
#              payload fails the run (no accidental builtin fallback).

foreach(_required CMAKE_COMMAND_PATH MAIN_SOURCE_DIR WORK_DIR GENERATOR LINKAGE)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()
if(NOT LINKAGE STREQUAL "static" AND NOT LINKAGE STREQUAL "dynamic")
    message(FATAL_ERROR "LINKAGE must be static or dynamic")
endif()
if(NOT DEFINED BUILD_TYPE OR "${BUILD_TYPE}" STREQUAL "")
    set(BUILD_TYPE "Release")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(_build_dir "${WORK_DIR}/build")

execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}"
        -S "${MAIN_SOURCE_DIR}"
        -B "${_build_dir}"
        -G "${GENERATOR}"
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE}
        -DWL2_MODULE_LINKAGE=${LINKAGE}
        -DWL2_ENABLE_EXTENDED_MODULES=OFF
        -DWL2_ENABLE_FS=OFF
        -DWL2_ENABLE_MEMBUS=OFF
        -DWL2_ENABLE_LIBMEMBUS=OFF
        -DWL2_BUILD_EXAMPLES=OFF
        -DWL2_BUILD_TESTING=OFF
        -DWL2_BUILD_DOCS=OFF
        -DWL2_BUILD_OUTOFTREE_TESTS=OFF
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_out
    ERROR_VARIABLE _configure_err)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "linkage=${LINKAGE} configure failed:\n${_configure_out}\n${_configure_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}" --build "${_build_dir}" --target wl2 --parallel
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_err)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "linkage=${LINKAGE} build failed:\n${_build_out}\n${_build_err}")
endif()

set(_wl2 "${_build_dir}/bin/wl2")
if(NOT EXISTS "${_wl2}")
    message(FATAL_ERROR "linkage=${LINKAGE} runner not found: ${_wl2}")
endif()

set(_app_dir "${WORK_DIR}/app")
file(MAKE_DIRECTORY "${_app_dir}")
file(WRITE "${_app_dir}/main.js" [=[
import { stringify } from "wl2:json";

if (stringify({ ok: true }) !== "{\"ok\":true}") {
  throw new Error("wl2:json did not load");
}

console.log("linkage mode module load ok");
]=])
file(WRITE "${_app_dir}/wl2.yml" [=[
schema: wl2.resources.v1
prefix: wl2:/linkage-test
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

function(_run_wl2 expect_success label)
    execute_process(
        COMMAND "${_wl2}" run --manifest "${_app_dir}/wl2.yml" --module-policy isolated --no-permission-prompt
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(expect_success)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "${label} failed:\n${_out}\n${_err}")
        endif()
        if(NOT _out MATCHES "linkage mode module load ok")
            message(FATAL_ERROR "${label} did not produce expected output:\n${_out}\n${_err}")
        endif()
    else()
        if(_result EQUAL 0)
            message(FATAL_ERROR "${label} unexpectedly succeeded:\n${_out}\n${_err}")
        endif()
    endif()
endfunction()

set(_store_module_dir "${_build_dir}/lib/wl2/modules/wl2_json")

if(LINKAGE STREQUAL "static")
    if(IS_DIRECTORY "${_store_module_dir}")
        message(FATAL_ERROR
            "static linkage staged a dynamic wl2:json payload: ${_store_module_dir}")
    endif()
    _run_wl2(TRUE "static builtin run")
else()
    if(NOT IS_DIRECTORY "${_store_module_dir}")
        message(FATAL_ERROR "dynamic linkage did not stage wl2:json: ${_store_module_dir}")
    endif()
    _run_wl2(TRUE "dynamic store run")
    file(GLOB _payloads "${_store_module_dir}/*wl2_json*")
    list(FILTER _payloads EXCLUDE REGEX "\\.yml$")
    if(NOT _payloads)
        message(FATAL_ERROR "no staged wl2:json payload in ${_store_module_dir}")
    endif()
    list(GET _payloads 0 _payload)
    file(RENAME "${_payload}" "${_payload}.hidden")
    _run_wl2(FALSE "dynamic run with hidden payload")
    file(RENAME "${_payload}.hidden" "${_payload}")
endif()

message(STATUS "linkage=${LINKAGE} module mode test passed")
