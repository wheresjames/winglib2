execute_process(
    COMMAND
        "${CMAKE_COMMAND_PATH}"
        -S "${RESOURCE_CONFLICT_SOURCE_DIR}"
        -B "${RESOURCE_CONFLICT_BINARY_DIR}"
        -DWL2_RESOURCES_MODULE=${WL2_RESOURCES_MODULE}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)

if(_result EQUAL 0)
    message(FATAL_ERROR "resource conflict fixture configured successfully")
endif()

string(FIND "${_output}\n${_error}" "conflicting explicit policies" _message_pos)
if(_message_pos EQUAL -1)
    message(FATAL_ERROR "resource conflict fixture failed without the expected diagnostic\noutput:\n${_output}\nerror:\n${_error}")
endif()
