file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(ENV{XDG_CONFIG_HOME} "${WORK_DIR}/xdg")
set(_trust_json "${WORK_DIR}/xdg/wl2/trust.json")

function(trust_run)
    cmake_parse_arguments(ARG "" "RESULT;OUTPUT;ERROR" "COMMAND" ${ARGN})
    execute_process(
        COMMAND "${WL2_EXECUTABLE}" ${ARG_COMMAND}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(ARG_RESULT)
        set(${ARG_RESULT} "${_rc}" PARENT_SCOPE)
    endif()
    if(ARG_OUTPUT)
        set(${ARG_OUTPUT} "${_out}" PARENT_SCOPE)
    endif()
    if(ARG_ERROR)
        set(${ARG_ERROR} "${_err}" PARENT_SCOPE)
    endif()
endfunction()

# Trust a declared-permissions script so the store has a record to manage.
set(_script "${WORK_DIR}/server.js")
file(WRITE "${_script}" "/* wl2
permissions:
  listen: [\"127.0.0.1:8080\"]
  network: [\"127.0.0.1:*\"]
*/
console.log(\"server ok\");
")
trust_run(COMMAND run --trust-declared "${_script}" RESULT _rc OUTPUT _out ERROR _err)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${_trust_json}")
    message(FATAL_ERROR "initial trusted run failed:\n${_out}\n${_err}")
endif()

# list must show a record id, permission summary, and path.
trust_run(COMMAND trust list RESULT _rc OUTPUT _list_out ERROR _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "trust list failed:\n${_list_out}\n${_err}")
endif()
if(NOT _list_out MATCHES "network,listen" OR NOT _list_out MATCHES "server.js")
    message(FATAL_ERROR "trust list missing expected columns:\n${_list_out}")
endif()
if(NOT _list_out MATCHES "(tr_[0-9a-fA-F]+)")
    message(FATAL_ERROR "trust list did not print a record id:\n${_list_out}")
endif()
set(_id "${CMAKE_MATCH_1}")

# list --json must emit parseable record fields.
trust_run(COMMAND trust list --json RESULT _rc OUTPUT _json_out ERROR _err)
if(NOT _rc EQUAL 0
        OR NOT _json_out MATCHES "\"id\": \"${_id}\""
        OR NOT _json_out MATCHES "\"permissions\"")
    message(FATAL_ERROR "trust list --json missing record fields:\n${_json_out}\n${_err}")
endif()

# show must render the full envelope for a known id.
trust_run(COMMAND trust show "${_id}" RESULT _rc OUTPUT _show_out ERROR _err)
if(NOT _rc EQUAL 0
        OR NOT _show_out MATCHES "sha256:"
        OR NOT _show_out MATCHES "network listeners matching 127.0.0.1:8080")
    message(FATAL_ERROR "trust show missing details:\n${_show_out}\n${_err}")
endif()

# show --json must emit a single object for the id.
trust_run(COMMAND trust show "${_id}" --json RESULT _rc OUTPUT _show_json ERROR _err)
if(NOT _rc EQUAL 0 OR NOT _show_json MATCHES "\"id\": \"${_id}\"")
    message(FATAL_ERROR "trust show --json missing id:\n${_show_json}\n${_err}")
endif()

# show for an unknown id must fail.
trust_run(COMMAND trust show tr_missing RESULT _rc OUTPUT _out ERROR _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "trust show of unknown id should fail:\n${_out}\n${_err}")
endif()

# clear without --yes in non-interactive mode must refuse and keep the record.
trust_run(COMMAND trust clear RESULT _rc OUTPUT _out ERROR _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "trust clear without --yes should refuse non-interactively:\n${_out}\n${_err}")
endif()

# revoke must remove the single record.
trust_run(COMMAND trust revoke "${_id}" RESULT _rc OUTPUT _out ERROR _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "trust revoke failed:\n${_out}\n${_err}")
endif()
trust_run(COMMAND trust list RESULT _rc OUTPUT _list_out ERROR _err)
if(NOT _rc EQUAL 0 OR NOT _list_out MATCHES "no trust records")
    message(FATAL_ERROR "record still present after revoke:\n${_list_out}\n${_err}")
endif()

# revoke of an already-removed id must fail.
trust_run(COMMAND trust revoke "${_id}" RESULT _rc OUTPUT _out ERROR _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "revoking a missing id should fail:\n${_out}\n${_err}")
endif()

# clear --yes must remove all records.
set(_a "${WORK_DIR}/a.js")
set(_b "${WORK_DIR}/b.js")
file(WRITE "${_a}" "/* wl2
permissions:
  listen: [\"127.0.0.1:8080\"]
*/
console.log(\"a\");
")
file(WRITE "${_b}" "/* wl2
permissions:
  listen: [\"127.0.0.1:9090\"]
*/
console.log(\"b\");
")
trust_run(COMMAND run --trust-declared "${_a}" RESULT _rc)
trust_run(COMMAND run --trust-declared "${_b}" RESULT _rc)
trust_run(COMMAND trust clear --yes RESULT _rc OUTPUT _out ERROR _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "cleared all trust records")
    message(FATAL_ERROR "trust clear --yes failed:\n${_out}\n${_err}")
endif()
trust_run(COMMAND trust list RESULT _rc OUTPUT _list_out)
if(NOT _list_out MATCHES "no trust records")
    message(FATAL_ERROR "records remained after clear --yes:\n${_list_out}")
endif()

# A malformed store must produce a non-zero exit rather than silent success.
file(WRITE "${_trust_json}" "not json at all {")
trust_run(COMMAND trust list RESULT _rc OUTPUT _out ERROR _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "trust list should fail on a malformed store:\n${_out}\n${_err}")
endif()
