if(NOT DEFINED WL2_EXECUTABLE)
    message(FATAL_ERROR "WL2_EXECUTABLE is required")
endif()
if(NOT DEFINED RESOURCE_ROOT)
    message(FATAL_ERROR "RESOURCE_ROOT is required")
endif()
if(NOT DEFINED EXTRACT_DIR)
    message(FATAL_ERROR "EXTRACT_DIR is required")
endif()

set(_map "${RESOURCE_ROOT}:wl2:/mapped")

execute_process(
    COMMAND "${WL2_EXECUTABLE}" resources list --map-resource "${_map}" wl2:/mapped
    RESULT_VARIABLE _list_result
    OUTPUT_VARIABLE _list_out
    ERROR_VARIABLE _list_err)
if(NOT _list_result EQUAL 0)
    message(FATAL_ERROR "resources list failed\n${_list_out}\n${_list_err}")
endif()
if(NOT _list_out MATCHES "wl2:/mapped/config.json")
    message(FATAL_ERROR "resources list did not include config.json\n${_list_out}")
endif()
if(NOT _list_out MATCHES "${RESOURCE_ROOT}.*/config.json")
    message(FATAL_ERROR "resources list did not include source path\n${_list_out}")
endif()

execute_process(
    COMMAND "${WL2_EXECUTABLE}" resources read --map-resource "${_map}" wl2:/mapped/config.json
    RESULT_VARIABLE _read_result
    OUTPUT_VARIABLE _read_out
    ERROR_VARIABLE _read_err)
if(NOT _read_result EQUAL 0)
    message(FATAL_ERROR "resources read failed\n${_read_out}\n${_read_err}")
endif()
if(NOT _read_out MATCHES "\"winglib2\"")
    message(FATAL_ERROR "resources read returned unexpected output\n${_read_out}")
endif()

file(REMOVE_RECURSE "${EXTRACT_DIR}")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" resources extract --map-resource "${_map}" wl2:/mapped --out "${EXTRACT_DIR}"
    RESULT_VARIABLE _extract_result
    OUTPUT_VARIABLE _extract_out
    ERROR_VARIABLE _extract_err)
if(NOT _extract_result EQUAL 0)
    message(FATAL_ERROR "resources extract failed\n${_extract_out}\n${_extract_err}")
endif()
if(NOT EXISTS "${EXTRACT_DIR}/mapped/config.json")
    message(FATAL_ERROR "resources extract did not write mapped/config.json")
endif()
file(READ "${EXTRACT_DIR}/mapped/config.json" _extracted)
if(NOT _extracted MATCHES "\"winglib2\"")
    message(FATAL_ERROR "resources extract wrote unexpected config\n${_extracted}")
endif()
