# Drives the static module graph scenarios as isolated CMake configures so the
# configure-time FATAL_ERROR scenarios can be observed without aborting the
# whole test. Required -D variables: CMAKE_COMMAND_PATH, FIXTURE_SOURCE_DIR,
# FIXTURE_BINARY_DIR, WL2_MODULES_FILE.

foreach(_required CMAKE_COMMAND_PATH FIXTURE_SOURCE_DIR FIXTURE_BINARY_DIR WL2_MODULES_FILE)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

# scenario => expected result ("ok" or "fail"); failures also match a substring.
set(_scenarios order diamond optional_present optional_missing missing cycle)
set(_expect_order ok)
set(_expect_diamond ok)
set(_expect_optional_present ok)
set(_expect_optional_missing ok)
set(_expect_missing fail)
set(_expect_cycle fail)
set(_match_missing "is not an enabled built-in static module")
set(_match_cycle "dependency cycle detected")

foreach(_scenario IN LISTS _scenarios)
    set(_bindir "${FIXTURE_BINARY_DIR}/${_scenario}")
    file(REMOVE_RECURSE "${_bindir}")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND_PATH}"
            -S "${FIXTURE_SOURCE_DIR}"
            -B "${_bindir}"
            "-DWL2_MODULES_FILE=${WL2_MODULES_FILE}"
            "-DSCENARIO=${_scenario}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    # CMake wraps long diagnostic text across lines; collapse whitespace so a
    # substring match is not defeated by the wrapping.
    string(REGEX REPLACE "[ \t\r\n]+" " " _combined "${_out}${_err}")
    set(_expected "${_expect_${_scenario}}")
    if(_expected STREQUAL "ok")
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "scenario ${_scenario} should succeed but failed (${_rc})\n${_combined}")
        endif()
    else()
        if(_rc EQUAL 0)
            message(FATAL_ERROR "scenario ${_scenario} should fail but succeeded\n${_combined}")
        endif()
        if(DEFINED _match_${_scenario} AND NOT _combined MATCHES "${_match_${_scenario}}")
            message(FATAL_ERROR "scenario ${_scenario} failed without expected diagnostic\n${_combined}")
        endif()
    endif()
endforeach()

message(STATUS "static module graph scenarios passed")
