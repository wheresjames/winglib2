# CLI checks for inspecting resources embedded in a standalone executable.
#
# Required -D variables:
#   WL2_EXECUTABLE      Path to the wl2 runner.
#   RESOURCE_EXE       Standalone executable with embedded wl2 resources.
#   EXTRACT_DIR        Scratch extraction directory.

foreach(_required WL2_EXECUTABLE RESOURCE_EXE EXTRACT_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

function(run_expect desc expect_rc out_var)
    execute_process(
        COMMAND ${ARGN}
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

run_expect("executable resource list" "ok" _out
    "${WL2_EXECUTABLE}" resources list "${RESOURCE_EXE}")
if(NOT _out MATCHES "wl2:/resources/config.json" OR NOT _out MATCHES "wl2:/resources/repeated.txt")
    message(FATAL_ERROR "resource table list did not contain expected resources\n${_out}")
endif()

run_expect("executable resource read" "ok" _out
    "${WL2_EXECUTABLE}" resources read "${RESOURCE_EXE}" wl2:/resources/config.json)
if(NOT _out MATCHES "\"winglib2\"")
    message(FATAL_ERROR "resource table read returned unexpected content\n${_out}")
endif()

run_expect("executable compressed read" "ok" _out
    "${WL2_EXECUTABLE}" resources read "${RESOURCE_EXE}" wl2:/resources/repeated.txt)
string(LENGTH "${_out}" _decompressed_len)
if(NOT _decompressed_len EQUAL 41)
    message(FATAL_ERROR "default executable read should return decompressed bytes, got ${_decompressed_len}")
endif()

run_expect("executable raw read" "ok" _out
    "${WL2_EXECUTABLE}" resources read --raw "${RESOURCE_EXE}" wl2:/resources/repeated.txt)
string(LENGTH "${_out}" _raw_len)
if(NOT _raw_len LESS _decompressed_len)
    message(FATAL_ERROR "--raw should return stored compressed bytes")
endif()

file(REMOVE_RECURSE "${EXTRACT_DIR}")
run_expect("executable resource extract" "ok" _out
    "${WL2_EXECUTABLE}" resources extract "${RESOURCE_EXE}" --out "${EXTRACT_DIR}")
if(NOT EXISTS "${EXTRACT_DIR}/resources/config.json")
    message(FATAL_ERROR "executable extract did not write config.json")
endif()
file(READ "${EXTRACT_DIR}/resources/config.json" _config)
if(NOT _config MATCHES "\"winglib2\"")
    message(FATAL_ERROR "executable extract wrote unexpected config content\n${_config}")
endif()

set(_raw_dir "${EXTRACT_DIR}-raw")
file(REMOVE_RECURSE "${_raw_dir}")
run_expect("executable resource raw extract" "ok" _out
    "${WL2_EXECUTABLE}" resources extract --raw "${RESOURCE_EXE}" --out "${_raw_dir}")
file(SIZE "${_raw_dir}/resources/repeated.txt" _raw_file_size)
if(NOT _raw_file_size LESS 41)
    message(FATAL_ERROR "--raw extract should write stored compressed bytes")
endif()

set(_invalid "${EXTRACT_DIR}-not-wl2.bin")
file(WRITE "${_invalid}" "not a wl2 executable\n")
run_expect("invalid executable resource table" "fail" _out
    "${WL2_EXECUTABLE}" resources list "${_invalid}")
if(NOT _out MATCHES "resource_table_not_found")
    message(FATAL_ERROR "invalid executable did not report resource_table_not_found\n${_out}")
endif()

# A crafted resource table must not be able to write outside --out via a
# traversal in the logical resource name.
set(_evil "${EXTRACT_DIR}-evil.bin")
file(WRITE "${_evil}"
    "WL2_RESOURCE_TABLE_V1\n"
    "../../pwned.txt\tstored\t2\t2\tdeadbeef\t6869\n"
    "WL2_RESOURCE_TABLE_END\n")
set(_evil_out "${EXTRACT_DIR}-evil-out")
file(REMOVE_RECURSE "${_evil_out}")
file(MAKE_DIRECTORY "${_evil_out}")
run_expect("traversal extract is refused" "fail" _out
    "${WL2_EXECUTABLE}" resources extract "${_evil}" --out "${_evil_out}")
if(NOT _out MATCHES "refusing to extract resource outside")
    message(FATAL_ERROR "traversal extract did not report a refusal\n${_out}")
endif()
# The escaped target is two levels above _evil_out; it must not exist.
get_filename_component(_evil_parent "${EXTRACT_DIR}" DIRECTORY)
get_filename_component(_evil_grandparent "${_evil_parent}" DIRECTORY)
if(EXISTS "${_evil_grandparent}/pwned.txt")
    file(REMOVE "${_evil_grandparent}/pwned.txt")
    message(FATAL_ERROR "traversal extract escaped --out and wrote pwned.txt")
endif()

# A malformed numeric field must produce a clean diagnostic, not a crash.
set(_malformed "${EXTRACT_DIR}-malformed.bin")
file(WRITE "${_malformed}"
    "WL2_RESOURCE_TABLE_V1\n"
    "wl2:/x.txt\tstored\tNOTANUMBER\t2\tdeadbeef\t6869\n"
    "WL2_RESOURCE_TABLE_END\n")
run_expect("malformed size field is rejected" "fail" _out
    "${WL2_EXECUTABLE}" resources list "${_malformed}")
if(NOT _out MATCHES "resource_table_invalid")
    message(FATAL_ERROR "malformed size field did not report resource_table_invalid\n${_out}")
endif()

message(STATUS "executable resource inspection checks passed")
