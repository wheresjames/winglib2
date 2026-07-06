foreach(_required WL2_EXECUTABLE WORK_DIR BUILD_MODULE_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_script "${WORK_DIR}/main.js")
set(_manifest "${WORK_DIR}/wl2.yml")
file(WRITE "${_script}" [=[
import { stringify } from "wl2:json";

if (stringify({ ok: true }) !== "{\"ok\":true}") {
  throw new Error("wl2:json did not load dynamically");
}

console.log("dynamic build-tree module load ok");
]=])
file(WRITE "${_manifest}" [=[
schema: wl2.resources.v1
prefix: wl2:/dynamic-test
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

# The isolated policy keeps a developer's user/system module stores from
# satisfying (or shadowing) the build-tree resolution under test.
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --manifest "${_manifest}" --module-policy isolated --no-permission-prompt
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_out
    ERROR_VARIABLE _run_err)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "dynamic build-tree run failed:\n${_run_out}\n${_run_err}")
endif()
if(NOT _run_out MATCHES "dynamic build-tree module load ok")
    message(FATAL_ERROR "dynamic build-tree run did not produce expected output:\n${_run_out}\n${_run_err}")
endif()

set(_json_dir "${BUILD_MODULE_DIR}/wl2_json")
if(NOT IS_DIRECTORY "${_json_dir}")
    message(FATAL_ERROR "expected staged wl2:json module directory: ${_json_dir}")
endif()
file(GLOB _json_libs "${_json_dir}/*wl2_json*${CMAKE_SHARED_MODULE_SUFFIX}")
if(NOT _json_libs)
    file(GLOB _json_libs "${_json_dir}/*wl2_json*")
endif()
list(LENGTH _json_libs _json_lib_count)
if(_json_lib_count EQUAL 0)
    message(FATAL_ERROR "expected staged wl2:json module payload in ${_json_dir}")
endif()
list(GET _json_libs 0 _json_lib)
set(_hidden_lib "${_json_lib}.hidden")
file(RENAME "${_json_lib}" "${_hidden_lib}")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --manifest "${_manifest}" --module-policy isolated --no-permission-prompt
    RESULT_VARIABLE _missing_result
    OUTPUT_VARIABLE _missing_out
    ERROR_VARIABLE _missing_err)
file(RENAME "${_hidden_lib}" "${_json_lib}")
if(_missing_result EQUAL 0)
    message(FATAL_ERROR
        "run succeeded after staged dynamic module was hidden; static fallback may be masking dynamic resolution\n"
        "stdout:\n${_missing_out}\nstderr:\n${_missing_err}")
endif()
if(NOT _missing_err MATCHES "module_library_missing|module_required_missing|module_load_failed|Installed module library is missing|Unable to open")
    message(FATAL_ERROR "missing dynamic module failed with unexpected diagnostic:\n${_missing_out}\n${_missing_err}")
endif()
