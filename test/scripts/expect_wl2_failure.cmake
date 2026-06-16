if(NOT DEFINED WL2_EXECUTABLE)
    message(FATAL_ERROR "WL2_EXECUTABLE is required")
endif()
if(NOT DEFINED EXPECT_MODE)
    message(FATAL_ERROR "EXPECT_MODE is required")
endif()

if(EXPECT_MODE STREQUAL "invalid_app_action")
    execute_process(
        COMMAND "${WL2_EXECUTABLE}" app missing-action
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_must_match "unknown app action")
elseif(EXPECT_MODE STREQUAL "stack_on")
    if(NOT DEFINED SCRIPT)
        message(FATAL_ERROR "SCRIPT is required")
    endif()
    execute_process(
        COMMAND "${WL2_EXECUTABLE}" run "${SCRIPT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_must_match "JavaScript stack:")
elseif(EXPECT_MODE STREQUAL "stack_off")
    if(NOT DEFINED SCRIPT)
        message(FATAL_ERROR "SCRIPT is required")
    endif()
    execute_process(
        COMMAND "${WL2_EXECUTABLE}" run --stack-traces=off "${SCRIPT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_must_not_match "JavaScript stack:")
elseif(EXPECT_MODE STREQUAL "promise_stack")
    if(NOT DEFINED SCRIPT)
        message(FATAL_ERROR "SCRIPT is required")
    endif()
    execute_process(
        COMMAND "${WL2_EXECUTABLE}" run "${SCRIPT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_must_match "JavaScript stack:")
else()
    message(FATAL_ERROR "Unknown EXPECT_MODE: ${EXPECT_MODE}")
endif()

set(_combined "${_stdout}\n${_stderr}")
if(_result EQUAL 0)
    message(FATAL_ERROR "Expected wl2 to fail, but it exited 0\n${_combined}")
endif()
if(DEFINED _must_match AND NOT _combined MATCHES "${_must_match}")
    message(FATAL_ERROR "Expected output to match '${_must_match}'\n${_combined}")
endif()
if(DEFINED _must_not_match AND _combined MATCHES "${_must_not_match}")
    message(FATAL_ERROR "Expected output not to match '${_must_not_match}'\n${_combined}")
endif()
