if(NOT DEFINED WL2_EXECUTABLE)
    message(FATAL_ERROR "WL2_EXECUTABLE is required")
endif()
if(NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "WORK_DIR is required")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}/files")
file(WRITE "${WORK_DIR}/files/main.js" "console.log('bad manifest fixture');\n")
file(WRITE "${WORK_DIR}/bad.yml" "schema: wl2.resources.v1\nroot: files\nentry: main.js\nunknown: nope\n")

execute_process(
    COMMAND "${WL2_EXECUTABLE}" config --manifest "${WORK_DIR}/bad.yml"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(_result EQUAL 0)
    message(FATAL_ERROR "bad manifest unexpectedly passed\n${_stdout}\n${_stderr}")
endif()
set(_combined "${_stdout}\n${_stderr}")
if(NOT _combined MATCHES "manifest_unknown_key")
    message(FATAL_ERROR "bad manifest did not report manifest_unknown_key\n${_combined}")
endif()
