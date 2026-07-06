# Runner module-mode build-and-run test for dynamic-with-static-fallback.
#
# Configures this source tree in a throwaway build directory with
# WL2_MODULE_LINKAGE=both and WL2_WL2_MODULE_MODE=dynamic-with-static-fallback,
# builds the wl2 runner (which links the full static registry AND stages the
# dynamic store), and proves the fallback contract end to end:
#   1. with the dynamic payload staged, the run resolves it (dynamic-first);
#   2. with the staged dynamic wl2:json directory removed, the run still
#      succeeds by falling back to the linked static wl2:json module;
#   3. with the dynamic payload removed AND --no-builtin-module-fallback set,
#      the run fails with module_required_missing (the flag actually gates the
#      fallback).

foreach(_required CMAKE_COMMAND_PATH MAIN_SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()
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
        -DWL2_MODULE_LINKAGE=both
        -DWL2_WL2_MODULE_MODE=dynamic-with-static-fallback
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
    message(FATAL_ERROR "module-mode configure failed:\n${_configure_out}\n${_configure_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build_dir}" --target wl2 --parallel
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_err)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "module-mode build failed:\n${_build_out}\n${_build_err}")
endif()

set(_wl2 "${_build_dir}/bin/wl2")
if(NOT EXISTS "${_wl2}")
    message(FATAL_ERROR "module-mode runner not found: ${_wl2}")
endif()

set(_app_dir "${WORK_DIR}/app")
file(MAKE_DIRECTORY "${_app_dir}")
file(WRITE "${_app_dir}/main.js" [=[
import { stringify } from "wl2:json";

if (stringify({ ok: true }) !== "{\"ok\":true}") {
  throw new Error("wl2:json did not load");
}

console.log("module-mode run ok");
]=])
file(WRITE "${_app_dir}/wl2.yml" [=[
schema: wl2.resources.v1
prefix: wl2:/module-mode-test
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

function(_run_wl2 expect_success extra_args label)
    execute_process(
        COMMAND "${_wl2}" run --manifest "${_app_dir}/wl2.yml"
                --module-policy isolated --no-permission-prompt ${extra_args}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(expect_success)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "${label} failed:\n${_out}\n${_err}")
        endif()
        if(NOT _out MATCHES "module-mode run ok")
            message(FATAL_ERROR "${label} did not produce expected output:\n${_out}\n${_err}")
        endif()
    else()
        if(_result EQUAL 0)
            message(FATAL_ERROR "${label} unexpectedly succeeded:\n${_out}\n${_err}")
        endif()
        if(NOT _err MATCHES "module_required_missing|Required module is not available")
            message(FATAL_ERROR "${label} failed with unexpected diagnostic:\n${_out}\n${_err}")
        endif()
    endif()
endfunction()

set(_store_module_dir "${_build_dir}/lib/wl2/modules/wl2_json")
if(NOT IS_DIRECTORY "${_store_module_dir}")
    message(FATAL_ERROR "fallback mode did not stage a dynamic wl2:json payload: ${_store_module_dir}")
endif()

# 1. Dynamic-first: the staged dynamic module resolves and runs.
_run_wl2(TRUE "" "dynamic-first run")

# 2. Remove the staged dynamic module entirely; the run still succeeds via the
#    linked static builtin (the resolver selects builtin, and the static module
#    is registered in the runtime).
file(REMOVE_RECURSE "${_store_module_dir}")
_run_wl2(TRUE "" "static fallback run")

# 3. With the dynamic payload still gone, --no-builtin-module-fallback disables
#    the fallback and the run fails with module_required_missing.
_run_wl2(FALSE "--no-builtin-module-fallback" "no-builtin-fallback run")

message(STATUS "module-mode (dynamic-with-static-fallback) test passed")