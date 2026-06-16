if(NOT DEFINED WL2_EXECUTABLE)
    message(FATAL_ERROR "WL2_EXECUTABLE is required")
endif()
if(NOT DEFINED MANIFEST)
    message(FATAL_ERROR "MANIFEST is required")
endif()
if(NOT DEFINED EXTRACT_DIR)
    message(FATAL_ERROR "EXTRACT_DIR is required")
endif()

execute_process(
    COMMAND "${WL2_EXECUTABLE}" config --manifest "${MANIFEST}"
    RESULT_VARIABLE _config_result
    OUTPUT_VARIABLE _config_out
    ERROR_VARIABLE _config_err)
if(NOT _config_result EQUAL 0)
    message(FATAL_ERROR "config --manifest failed\n${_config_out}\n${_config_err}")
endif()
if(NOT _config_out MATCHES "schema: wl2.resources.v1")
    message(FATAL_ERROR "config output missing schema\n${_config_out}")
endif()
if(NOT _config_out MATCHES "entry: wl2:/resources/main.js")
    message(FATAL_ERROR "config output missing entry\n${_config_out}")
endif()

execute_process(
    COMMAND "${WL2_EXECUTABLE}" resources list --manifest "${MANIFEST}"
    RESULT_VARIABLE _list_result
    OUTPUT_VARIABLE _list_out
    ERROR_VARIABLE _list_err)
if(NOT _list_result EQUAL 0)
    message(FATAL_ERROR "resources list --manifest failed\n${_list_out}\n${_list_err}")
endif()
if(NOT _list_out MATCHES "wl2:/resources/config.json")
    message(FATAL_ERROR "resources list missing config\n${_list_out}")
endif()

execute_process(
    COMMAND "${WL2_EXECUTABLE}" resources read --manifest "${MANIFEST}" wl2:/resources/config.json
    RESULT_VARIABLE _read_result
    OUTPUT_VARIABLE _read_out
    ERROR_VARIABLE _read_err)
if(NOT _read_result EQUAL 0)
    message(FATAL_ERROR "resources read --manifest failed\n${_read_out}\n${_read_err}")
endif()
if(NOT _read_out MATCHES "\"winglib2\"")
    message(FATAL_ERROR "resources read returned unexpected output\n${_read_out}")
endif()

file(REMOVE_RECURSE "${EXTRACT_DIR}")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" resources extract --manifest "${MANIFEST}" --out "${EXTRACT_DIR}"
    RESULT_VARIABLE _extract_result
    OUTPUT_VARIABLE _extract_out
    ERROR_VARIABLE _extract_err)
if(NOT _extract_result EQUAL 0)
    message(FATAL_ERROR "resources extract --manifest failed\n${_extract_out}\n${_extract_err}")
endif()
if(NOT EXISTS "${EXTRACT_DIR}/resources/config.json")
    message(FATAL_ERROR "resources extract did not write resources/config.json")
endif()
