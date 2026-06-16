# End-to-end CLI test for Phase 3 dynamic dependency graphs:
#   - wl2 module install writes wl2.module.v2 dependency metadata.
#   - wl2 module validate reports declared dependencies.
#   - wl2 module graph resolves transitive installed dependencies in order.
#   - wl2 config --json reports the selected graph.
#   - wl2 run loads a transitive installed dependency automatically: the app
#     requires only the dependent module and the dependency is selected/loaded.
#
# Required -D variables:
#   WL2_EXECUTABLE  Path to the wl2 runner.
#   WL2_DYN_GOOD    Dynamic module providing wl2:dyn_good (no dependencies).
#   WL2_DYN_DEPS    Dynamic module providing wl2:dyn_deps (requires wl2:dyn_good).
#   WORK_DIR        Scratch directory.

foreach(_required WL2_EXECUTABLE WL2_DYN_GOOD WL2_DYN_DEPS WORK_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
set(_project "${WORK_DIR}/project")
file(MAKE_DIRECTORY "${_project}/files")

# Isolate user/system scopes inside the work directory.
set(ENV{XDG_DATA_HOME} "${WORK_DIR}/xdg")
set(ENV{WL2_SYSTEM_MODULE_DIR} "${WORK_DIR}/system/modules")

# The app requires only wl2:dyn_deps; its dependency wl2:dyn_good must be
# selected and loaded automatically. The entry script imports neither module.
file(WRITE "${_project}/files/main.js" "console.log(\"ran ok\");\n")
file(WRITE "${_project}/wl2.yml"
    "schema: wl2.project.v1\nprefix: wl2:/app\nroot: files\nentry: main.js\nmodules:\n  require:\n    - wl2:dyn_deps\n")

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

# module validate reports the declared dependencies.
run_expect("validate dependency module" "ok" _out "${WL2_EXECUTABLE}" module validate "${WL2_DYN_DEPS}")
if(NOT _out MATCHES "dependencies:" OR NOT _out MATCHES "wl2:dyn_good")
    message(FATAL_ERROR "module validate did not report dependencies\n${_out}")
endif()

# Before install, the required module is missing.
run_expect("run before install" "fail" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
if(NOT _out MATCHES "module_required_missing")
    message(FATAL_ERROR "expected module_required_missing before install\n${_out}")
endif()

# Install the dependency and the dependent module into the local scope.
run_expect("install dependency" "ok" _out "${WL2_EXECUTABLE}" module install "${WL2_DYN_GOOD}" --scope local)
run_expect("install dependent" "ok" _out "${WL2_EXECUTABLE}" module install "${WL2_DYN_DEPS}" --scope local)

# Installed metadata is wl2.module.v2 with the dependency recorded.
file(READ "${_project}/.wl2/modules/wl2_dyn_deps/wl2.module.yml" _meta)
if(NOT _meta MATCHES "schema: wl2.module.v2" OR NOT _meta MATCHES "wl2:dyn_good")
    message(FATAL_ERROR "install did not write wl2.module.v2 dependency metadata\n${_meta}")
endif()

# module graph resolves the transitive dependency and lists both modules.
run_expect("module graph json" "ok" _out "${WL2_EXECUTABLE}" module graph --manifest wl2.yml --json)
if(NOT _out MATCHES "wl2:dyn_good" OR NOT _out MATCHES "wl2:dyn_deps")
    message(FATAL_ERROR "module graph did not list both modules\n${_out}")
endif()

# The text graph prints only the resolved load order, so position is meaningful:
# the dependency must precede the dependent.
run_expect("module graph text" "ok" _out "${WL2_EXECUTABLE}" module graph --manifest wl2.yml)
string(FIND "${_out}" "wl2:dyn_good" _good_pos)
string(FIND "${_out}" "wl2:dyn_deps" _deps_pos)
if(_good_pos EQUAL -1 OR _deps_pos EQUAL -1 OR _good_pos GREATER _deps_pos)
    message(FATAL_ERROR "module graph did not order the dependency first\n${_out}")
endif()

# config --json reports the selected graph.
run_expect("config json graph" "ok" _out "${WL2_EXECUTABLE}" config --manifest wl2.yml --json)
if(NOT _out MATCHES "\"graph\"" OR NOT _out MATCHES "wl2:dyn_good")
    message(FATAL_ERROR "config --json did not report the resolved graph\n${_out}")
endif()

# The app requires only wl2:dyn_deps; running it loads wl2:dyn_good automatically.
run_expect("run after install" "ok" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
if(NOT _out MATCHES "ran ok")
    message(FATAL_ERROR "transitive dependency was not loaded at run time\n${_out}")
endif()

message(STATUS "module graph CLI checks passed")
