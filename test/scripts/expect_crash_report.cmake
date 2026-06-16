# Crash report checks driven through the wl2_crash_fixture executable.
#
# Required -D variables:
#   WL2_CRASH_FIXTURE  Path to the built crash fixture.
#   WORK_DIR          Scratch directory.

foreach(_required WL2_CRASH_FIXTURE WORK_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# The fixture always crashes, so a zero exit code means it failed to crash.
function(run_fixture)
    execute_process(
        COMMAND ${WL2_CRASH_FIXTURE} ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "fixture exited cleanly but should have crashed\n${_out}${_err}")
    endif()
endfunction()

# --- auto report captures signal, stack, manifest, modules, and threads -------
set(_auto_dir "${WORK_DIR}/auto")
file(MAKE_DIRECTORY "${_auto_dir}")
run_fixture(
    --report auto --dir "${_auto_dir}"
    --manifest wl2.yml
    --module wl2:curl --module wl2:fs
    --thread /main/worker
    --crash abort)

file(GLOB _auto_logs "${_auto_dir}/crash-*.log")
list(LENGTH _auto_logs _auto_count)
if(NOT _auto_count EQUAL 1)
    message(FATAL_ERROR "expected exactly one auto crash log, found ${_auto_count}: ${_auto_logs}")
endif()
list(GET _auto_logs 0 _auto_log)
file(READ "${_auto_log}" _report)

foreach(_needle
        "== wl2 crash report =="
        "signal: SIGABRT (6)"
        "C++ stack (crashing thread):"
        "manifest: wl2.yml"
        "wl2:curl"
        "wl2:fs"
        "/main"
        "/main/worker")
    string(FIND "${_report}" "${_needle}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "crash report missing '${_needle}'\n${_report}")
    endif()
endforeach()

# The stack section must contain at least one frame line after its header.
string(REGEX MATCH "C\\+\\+ stack \\(crashing thread\\):\n[^\n]+" _stack "${_report}")
if(_stack STREQUAL "")
    message(FATAL_ERROR "crash report has no stack frames\n${_report}")
endif()

# --- JSON trailer parses and reports the expected fields ----------------------
set(_marker "--- json ---")
string(FIND "${_report}" "${_marker}" _json_at)
if(_json_at EQUAL -1)
    message(FATAL_ERROR "crash report has no JSON trailer\n${_report}")
endif()
string(LENGTH "${_marker}" _marker_len)
math(EXPR _json_start "${_json_at} + ${_marker_len}")
string(SUBSTRING "${_report}" ${_json_start} -1 _json)

string(JSON _signal_name GET "${_json}" signalName)
if(NOT _signal_name STREQUAL "SIGABRT")
    message(FATAL_ERROR "JSON signalName was '${_signal_name}', expected SIGABRT\n${_json}")
endif()
string(JSON _json_manifest GET "${_json}" manifest)
if(NOT _json_manifest STREQUAL "wl2.yml")
    message(FATAL_ERROR "JSON manifest was '${_json_manifest}'\n${_json}")
endif()
string(JSON _module0 GET "${_json}" modules 0)
if(NOT _module0 STREQUAL "wl2:curl")
    message(FATAL_ERROR "JSON modules[0] was '${_module0}'\n${_json}")
endif()
string(JSON _thread0 GET "${_json}" threads 0)
if(_thread0 STREQUAL "")
    message(FATAL_ERROR "JSON threads list is empty\n${_json}")
endif()
string(JSON _frame_count GET "${_json}" stackFrameCount)
if(_frame_count LESS_EQUAL 0)
    message(FATAL_ERROR "JSON stackFrameCount was '${_frame_count}'\n${_json}")
endif()

# --- custom report path is honored --------------------------------------------
set(_custom "${WORK_DIR}/custom-report.log")
run_fixture(--report "${_custom}" --crash abort)
if(NOT EXISTS "${_custom}")
    message(FATAL_ERROR "custom report path was not written: ${_custom}")
endif()

# --- segv reports its signal name ---------------------------------------------
set(_segv "${WORK_DIR}/segv-report.log")
run_fixture(--report "${_segv}" --crash segv)
if(NOT EXISTS "${_segv}")
    message(FATAL_ERROR "segv report path was not written: ${_segv}")
endif()
file(READ "${_segv}" _segv_report)
string(FIND "${_segv_report}" "signal: SIGSEGV" _segv_found)
if(_segv_found EQUAL -1)
    message(FATAL_ERROR "segv report missing SIGSEGV signal\n${_segv_report}")
endif()

# --- off disables reporting ---------------------------------------------------
set(_off_dir "${WORK_DIR}/off")
file(MAKE_DIRECTORY "${_off_dir}")
run_fixture(--report off --dir "${_off_dir}" --crash abort)
file(GLOB _off_logs "${_off_dir}/crash-*.log")
if(_off_logs)
    message(FATAL_ERROR "off mode still wrote a crash log: ${_off_logs}")
endif()

message(STATUS "crash report checks passed")
