# CLI checks for expanded config output, JSON config, watch mode, and source-map
# stack remapping.
#
# Required -D variables:
#   WL2_EXECUTABLE  Path to the wl2 runner.
#   WL2_ECHO_SO     Path to the built wl2_echo dynamic module.
#   WORK_DIR       Scratch directory.

foreach(_required WL2_EXECUTABLE WL2_ECHO_SO WORK_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
set(_project "${WORK_DIR}/project")
file(MAKE_DIRECTORY "${_project}/files")

file(WRITE "${_project}/files/main.js"
    "console.log('config/watch main v1');\n"
    "const text = wl2.resources.readText('wl2:/app/data.txt');\n"
    "console.log('resource', text.trim());\n")
file(WRITE "${_project}/files/data.txt" "one\n")
file(WRITE "${_project}/wl2.yml"
    "schema: wl2.project.v1\n"
    "prefix: wl2:/app\n"
    "root: files\n"
    "entry: main.js\n"
    "resources:\n"
    "  store:\n"
    "    files:\n"
    "      - data.txt\n")

function(run_expect desc expect_rc out_var)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${_project}"
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

run_expect("config human" "ok" _out "${WL2_EXECUTABLE}" config --manifest wl2.yml)
foreach(_needle "engine:" "manifest:" "modules:" "resources:" "filesystem:" "dependencies:" "diagnostics:")
    if(NOT _out MATCHES "${_needle}")
        message(FATAL_ERROR "config output missing ${_needle}\n${_out}")
    endif()
endforeach()

run_expect("config json" "ok" _json "${WL2_EXECUTABLE}" config --manifest wl2.yml --json)
string(JSON _engine GET "${_json}" engine)
if(NOT _engine MATCHES "quickjs|v8")
    message(FATAL_ERROR "config JSON has unexpected engine: ${_engine}\n${_json}")
endif()
string(JSON _schema GET "${_json}" manifest schema)
if(NOT _schema STREQUAL "wl2.project.v1")
    message(FATAL_ERROR "config JSON did not report manifest schema\n${_json}")
endif()
string(JSON _res_prefix GET "${_json}" resources 0 prefix)
if(NOT _res_prefix STREQUAL "wl2:/app")
    message(FATAL_ERROR "config JSON did not report resource map\n${_json}")
endif()

# Watch mode reruns on resource and manifest changes. The environment limit
# keeps the command finite after the initial run plus one rerun.
execute_process(
    COMMAND /bin/sh -c "(sleep 0.5; printf 'two\\n' > files/data.txt) & WL2_WATCH_EXIT_AFTER_RERUNS=2 '${WL2_EXECUTABLE}' run --manifest wl2.yml --watch"
    WORKING_DIRECTORY "${_project}"
    RESULT_VARIABLE _watch_rc
    OUTPUT_VARIABLE _watch_out
    ERROR_VARIABLE _watch_err
    TIMEOUT 5)
set(_watch_combined "${_watch_out}${_watch_err}")
if(NOT _watch_rc EQUAL 0 OR NOT _watch_combined MATCHES "wl2 watch: changed" OR NOT _watch_combined MATCHES "resource two")
    message(FATAL_ERROR "watch did not rerun on resource change\n${_watch_combined}")
endif()

execute_process(
    COMMAND /bin/sh -c "(sleep 0.5; printf 'schema: wl2.project.v1\\nprefix: wl2:/app\\nroot: files\\nentry: main.js\\nresources:\\n  store:\\n    files:\\n      - data.txt\\n' > wl2.yml) & WL2_WATCH_EXIT_AFTER_RERUNS=2 '${WL2_EXECUTABLE}' run --manifest wl2.yml --watch"
    WORKING_DIRECTORY "${_project}"
    RESULT_VARIABLE _manifest_watch_rc
    OUTPUT_VARIABLE _manifest_watch_out
    ERROR_VARIABLE _manifest_watch_err
    TIMEOUT 5)
set(_manifest_watch_combined "${_manifest_watch_out}${_manifest_watch_err}")
if(NOT _manifest_watch_rc EQUAL 0 OR NOT _manifest_watch_combined MATCHES "wl2 watch: changed")
    message(FATAL_ERROR "watch did not report manifest change\n${_manifest_watch_combined}")
endif()

# Native/module input changes report rebuild-needed instead of rerunning.
file(COPY "${WL2_ECHO_SO}" DESTINATION "${_project}")
get_filename_component(_echo_name "${WL2_ECHO_SO}" NAME)
execute_process(
    COMMAND /bin/sh -c "(sleep 1; touch '${_echo_name}') & WL2_WATCH_EXIT_AFTER_RERUNS=2 '${WL2_EXECUTABLE}' run --manifest wl2.yml --watch --load-module './${_echo_name}'"
    WORKING_DIRECTORY "${_project}"
    RESULT_VARIABLE _native_watch_rc
    OUTPUT_VARIABLE _native_watch_out
    ERROR_VARIABLE _native_watch_err
    TIMEOUT 5)
set(_native_watch_combined "${_native_watch_out}${_native_watch_err}")
if(NOT _native_watch_combined MATCHES "rebuild-needed")
    message(FATAL_ERROR "watch did not report rebuild-needed for native input\n${_native_watch_combined}")
endif()

# External source map remaps generated stack locations to the original source.
set(_gen "${_project}/generated.js")
set(_map "${_project}/generated.js.map")
set(_src "${_project}/original.ts")
file(WRITE "${_src}" "export function boom() { throw new Error('mapped failure'); }\n")
file(WRITE "${_gen}" "throw new Error('mapped failure');\n//# sourceMappingURL=generated.js.map\n")
file(WRITE "${_map}" "{\"version\":3,\"file\":\"generated.js\",\"sources\":[\"original.ts\"],\"names\":[],\"mappings\":\"AAAA\"}\n")
run_expect("source map remap" "fail" _out "${WL2_EXECUTABLE}" run "${_gen}")
if(NOT _out MATCHES "original.ts:1:")
    message(FATAL_ERROR "stack was not remapped through source map\n${_out}")
endif()

# Inline source maps are also supported for base64 JSON data URLs.
set(_inline "${_project}/inline.js")
set(_inline_src "${_project}/inline.ts")
file(WRITE "${_inline_src}" "export function inlineBoom() { throw new Error('inline mapped failure'); }\n")
file(WRITE "${_inline}"
    "throw new Error('inline mapped failure');\n"
    "//# sourceMappingURL=data:application/json;base64,eyJ2ZXJzaW9uIjozLCJmaWxlIjoiaW5saW5lLmpzIiwic291cmNlcyI6WyJpbmxpbmUudHMiXSwibmFtZXMiOltdLCJtYXBwaW5ncyI6IkFBQUEifQ==\n")
run_expect("inline source map remap" "fail" _out "${WL2_EXECUTABLE}" run "${_inline}")
if(NOT _out MATCHES "inline.ts:1:")
    message(FATAL_ERROR "stack was not remapped through inline source map\n${_out}")
endif()

# Missing source map falls back to the generated location.
file(WRITE "${_gen}" "throw new Error('generated failure');\n//# sourceMappingURL=missing.js.map\n")
run_expect("missing source map fallback" "fail" _out "${WL2_EXECUTABLE}" run "${_gen}")
if(NOT _out MATCHES "generated.js:1:" OR _out MATCHES "original.ts")
    message(FATAL_ERROR "missing source map did not fall back to generated location\n${_out}")
endif()

message(STATUS "config/watch/source-map CLI checks passed")
