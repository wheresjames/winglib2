file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(ENV{XDG_CONFIG_HOME} "${WORK_DIR}/xdg")
set(_trust_json "${WORK_DIR}/xdg/wl2/trust.json")

function(file_sha256 path out_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E sha256sum "${path}"
        OUTPUT_VARIABLE _sha_out
        RESULT_VARIABLE _sha_rc)
    if(NOT _sha_rc EQUAL 0)
        message(FATAL_ERROR "sha256 failed for ${path}")
    endif()
    string(REGEX MATCH "^[0-9a-fA-F]+" _sha "${_sha_out}")
    set(${out_var} "${_sha}" PARENT_SCOPE)
endfunction()

function(real_path path out_var)
    file(REAL_PATH "${path}" _real)
    set(${out_var} "${_real}" PARENT_SCOPE)
endfunction()

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
if(NOT EXISTS "${_trust_json}")
    message(FATAL_ERROR "trust.json was not written")
endif()
file(READ "${_trust_json}" _trust_text)
if(NOT _trust_text MATCHES "\"schema\": \"wl2.trust.v1\""
        OR NOT _trust_text MATCHES "\"permissions\""
        OR NOT _trust_text MATCHES "\"sha256\"")
    message(FATAL_ERROR "trust.json does not contain expected record fields\n${_trust_text}")
endif()

execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --no-permission-prompt "${_script}"
    RESULT_VARIABLE _saved_rc
    OUTPUT_VARIABLE _saved_out
    ERROR_VARIABLE _saved_err)
if(NOT _saved_rc EQUAL 0)
    message(FATAL_ERROR "saved declared-permission approval was not honored:\n${_saved_out}\n${_saved_err}")
endif()

set(_moved "${WORK_DIR}/declared_permissions_moved.test.js")
configure_file("${SOURCE_SCRIPT}" "${_moved}" COPYONLY)
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --no-permission-prompt "${_moved}"
    RESULT_VARIABLE _moved_rc
    OUTPUT_VARIABLE _moved_out
    ERROR_VARIABLE _moved_err)
if(_moved_rc EQUAL 0 OR NOT _moved_err MATCHES "same content as a previously trusted file")
    message(FATAL_ERROR "moved script should not auto-approve and should explain same content:\n${_moved_out}\n${_moved_err}")
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
if(NOT _changed_err MATCHES "changed since its previous trust approval")
    message(FATAL_ERROR "changed script did not explain changed trust identity:\n${_changed_out}\n${_changed_err}")
endif()

set(_home_dir "$ENV{HOME}")
set(_raw_script "${WORK_DIR}/raw_permissions.test.js")
file(WRITE "${_raw_script}" "/* wl2
permissions:
  filesystemRead: [\"\${HOME}/wl2-trust-test\"]
*/
console.log(\"raw permissions ok\");
")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --trust-declared "${_raw_script}"
    RESULT_VARIABLE _raw_rc
    OUTPUT_VARIABLE _raw_out
    ERROR_VARIABLE _raw_err)
if(NOT _raw_rc EQUAL 0)
    message(FATAL_ERROR "raw filesystem trust run failed:\n${_raw_out}\n${_raw_err}")
endif()
file(READ "${_trust_json}" _trust_text)
if(NOT _trust_text MATCHES "\"raw\": \"\\$\\{HOME\\}/wl2-trust-test\""
        OR NOT _trust_text MATCHES "\"resolved\": \"${_home_dir}/wl2-trust-test\"")
    message(FATAL_ERROR "trust.json did not preserve raw and resolved filesystem paths\n${_trust_text}")
endif()

set(_narrow_script "${WORK_DIR}/narrow_permissions.test.js")
file(WRITE "${_narrow_script}" "/* wl2
permissions:
  listen: [\"127.0.0.1:8080\"]
*/
console.log(\"narrow permissions ok\");
")
file_sha256("${_narrow_script}" _narrow_sha)
real_path("${_narrow_script}" _narrow_path)
file(MAKE_DIRECTORY "${WORK_DIR}/xdg/wl2")
file(WRITE "${_trust_json}" "{
  \"schema\": \"wl2.trust.v1\",
  \"records\": [
    {
      \"id\": \"tr_narrow\",
      \"kind\": \"script\",
      \"path\": \"${_narrow_path}\",
      \"displayPath\": \"narrow_permissions.test.js\",
      \"sha256\": \"${_narrow_sha}\",
      \"permissions\": {
        \"network\": [],
        \"listen\": [\"127.0.0.1:*\"],
        \"sharedMemory\": [],
        \"filesystemRead\": [],
        \"ui\": false,
        \"graphics\": false
      },
      \"grantedAt\": \"2026-07-03T12:00:00Z\",
      \"lastUsedAt\": \"2026-07-03T12:00:00Z\",
      \"source\": \"test\"
    }
  ]
}
")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --no-permission-prompt "${_narrow_script}"
    RESULT_VARIABLE _narrow_rc
    OUTPUT_VARIABLE _narrow_out
    ERROR_VARIABLE _narrow_err)
if(NOT _narrow_rc EQUAL 0)
    message(FATAL_ERROR "narrower declared permissions were not auto-approved:\n${_narrow_out}\n${_narrow_err}")
endif()

set(_broad_script "${WORK_DIR}/broad_permissions.test.js")
file(WRITE "${_broad_script}" "/* wl2
permissions:
  listen: [\"127.0.0.1:8080\", \"127.0.0.1:9090\"]
*/
console.log(\"broad permissions ok\");
")
file_sha256("${_broad_script}" _broad_sha)
real_path("${_broad_script}" _broad_path)
file(WRITE "${_trust_json}" "{
  \"schema\": \"wl2.trust.v1\",
  \"records\": [
    {
      \"id\": \"tr_broad\",
      \"kind\": \"script\",
      \"path\": \"${_broad_path}\",
      \"displayPath\": \"broad_permissions.test.js\",
      \"sha256\": \"${_broad_sha}\",
      \"permissions\": {
        \"network\": [],
        \"listen\": [\"127.0.0.1:8080\"],
        \"sharedMemory\": [],
        \"filesystemRead\": [],
        \"ui\": false,
        \"graphics\": false
      },
      \"grantedAt\": \"2026-07-03T12:00:00Z\",
      \"lastUsedAt\": \"2026-07-03T12:00:00Z\",
      \"source\": \"test\"
    }
  ]
}
")
execute_process(
    COMMAND "${WL2_EXECUTABLE}" run --no-permission-prompt "${_broad_script}"
    RESULT_VARIABLE _broad_rc
    OUTPUT_VARIABLE _broad_out
    ERROR_VARIABLE _broad_err)
if(_broad_rc EQUAL 0
        OR NOT _broad_err MATCHES "requests additional permissions"
        OR NOT _broad_err MATCHES "127.0.0.1:9090")
    message(FATAL_ERROR "broader declared permissions should require approval and show delta:\n${_broad_out}\n${_broad_err}")
endif()
