# WL2_MODULE_PATH / --module-path smoke test.
#
# Proves that an explicit module-path directory satisfies a manifest-required
# module even under a policy that excludes every other source (project-only),
# that removing the env var makes the same run fail with module_required_missing,
# and that `wl2 module graph` reports the module-path provider source.

foreach(_required WL2_EXECUTABLE WORK_DIR BUILD_MODULE_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Stage a copy of the build-tree wl2:json module under a throwaway module-path
# directory so it is discoverable only through WL2_MODULE_PATH.
set(_store "${WORK_DIR}/store")
file(MAKE_DIRECTORY "${_store}")
set(_src_json_dir "${BUILD_MODULE_DIR}/wl2_json")
if(NOT IS_DIRECTORY "${_src_json_dir}")
    message(FATAL_ERROR "expected staged wl2:json module directory: ${_src_json_dir}")
endif()
file(COPY "${_src_json_dir}" DESTINATION "${_store}")

set(_app_dir "${WORK_DIR}/app")
file(MAKE_DIRECTORY "${_app_dir}")
file(WRITE "${_app_dir}/main.js" [=[
import { stringify } from "wl2:json";

if (stringify({ ok: true }) !== "{\"ok\":true}") {
  throw new Error("wl2:json did not load from the module-path store");
}

console.log("module-path module load ok");
]=])
file(WRITE "${_app_dir}/wl2.yml" [=[
schema: wl2.resources.v1
prefix: wl2:/module-path-test
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

# With WL2_MODULE_PATH set, the module resolves from the explicit module-path
# directory even though project-only excludes every other source.
set(ENV{WL2_MODULE_PATH} "${_store}")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --manifest "${_app_dir}/wl2.yml"
            --module-policy project-only --no-permission-prompt
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_out
    ERROR_VARIABLE _run_err)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "module-path run failed:\n${_run_out}\n${_run_err}")
endif()
if(NOT _run_out MATCHES "module-path module load ok")
    message(FATAL_ERROR "module-path run did not produce expected output:\n${_run_out}\n${_run_err}")
endif()

# `wl2 module graph` reports the module-path provider source.
execute_process(
    COMMAND "${WL2_EXECUTABLE}" module graph --manifest "${_app_dir}/wl2.yml"
            --module-policy project-only
    RESULT_VARIABLE _graph_result
    OUTPUT_VARIABLE _graph_out
    ERROR_VARIABLE _graph_err)
if(NOT _graph_result EQUAL 0)
    message(FATAL_ERROR "module graph failed:\n${_graph_out}\n${_graph_err}")
endif()
if(NOT _graph_out MATCHES "module-path")
    message(FATAL_ERROR "module graph did not report a module-path provider:\n${_graph_out}\n${_graph_err}")
endif()

# Without WL2_MODULE_PATH, project-only has no source for wl2:json and the run
# fails with module_required_missing.
set(ENV{WL2_MODULE_PATH} "")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --manifest "${_app_dir}/wl2.yml"
            --module-policy project-only --no-permission-prompt
    RESULT_VARIABLE _missing_result
    OUTPUT_VARIABLE _missing_out
    ERROR_VARIABLE _missing_err)
if(_missing_result EQUAL 0)
    message(FATAL_ERROR
        "run succeeded without WL2_MODULE_PATH under project-only; module-path may not be policy-independent\n"
        "stdout:\n${_missing_out}\nstderr:\n${_missing_err}")
endif()
if(NOT _missing_err MATCHES "module_required_missing|Required module is not available")
    message(FATAL_ERROR "missing module-path run failed with unexpected diagnostic:\n${_missing_out}\n${_missing_err}")
endif()

message(STATUS "module-path smoke test passed")