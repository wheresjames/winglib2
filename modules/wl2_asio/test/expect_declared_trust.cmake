file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(ENV{XDG_CONFIG_HOME} "${WORK_DIR}/xdg")
set(_script "${WORK_DIR}/declared_permissions.test.js")
configure_file("${SOURCE_SCRIPT}" "${_script}" COPYONLY)

execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --trust-declared "${_script}"
    RESULT_VARIABLE _trust_rc
    OUTPUT_VARIABLE _trust_out
    ERROR_VARIABLE _trust_err)
if(NOT _trust_rc EQUAL 0)
    message(FATAL_ERROR "initial trusted run failed:\n${_trust_out}\n${_trust_err}")
endif()

execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --no-permission-prompt "${_script}"
    RESULT_VARIABLE _saved_rc
    OUTPUT_VARIABLE _saved_out
    ERROR_VARIABLE _saved_err)
if(NOT _saved_rc EQUAL 0)
    message(FATAL_ERROR "saved declared-permission approval was not honored:\n${_saved_out}\n${_saved_err}")
endif()

file(APPEND "${_script}" "\n// invalidate saved declared-permission approval\n")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --no-permission-prompt "${_script}"
    RESULT_VARIABLE _changed_rc
    OUTPUT_VARIABLE _changed_out
    ERROR_VARIABLE _changed_err)
if(_changed_rc EQUAL 0)
    message(FATAL_ERROR "changed script unexpectedly reused saved declared-permission approval:\n${_changed_out}\n${_changed_err}")
endif()
