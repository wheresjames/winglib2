# End-to-end CLI test for module install/list/uninstall, run-time resolution of
# an installed module, wl2 config shadowing, and wl2 deps.
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

# Isolate user/system scopes inside the work directory.
set(ENV{XDG_DATA_HOME} "${WORK_DIR}/xdg")
set(ENV{WL2_SYSTEM_MODULE_DIR} "${WORK_DIR}/system/modules")

# A project whose entry imports the wl2:echo module, declared as required.
file(WRITE "${_project}/files/main.js"
    "import { shout } from \"wl2:echo\";\nconsole.log(\"resolved:\", shout(\"ok\"));\n")
file(WRITE "${_project}/wl2.yml"
    "schema: wl2.project.v1\nprefix: wl2:/app\nroot: files\nentry: main.js\nmodules:\n  require:\n    - wl2:echo\n")

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

# Before installing, running fails: wl2:echo is neither built-in nor installed.
run_expect("run before install" "fail" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
if(NOT _out MATCHES "module_required_missing")
    message(FATAL_ERROR "expected module_required_missing before install\n${_out}")
endif()

# Install the module into the local scope.
run_expect("module install" "ok" _out "${WL2_EXECUTABLE}" module install "${WL2_ECHO_SO}" --scope local)
if(NOT EXISTS "${_project}/.wl2/modules/index.yml")
    message(FATAL_ERROR "install did not write the scope index")
endif()

run_expect("module list" "ok" _out "${WL2_EXECUTABLE}" module list)
if(NOT _out MATCHES "wl2:echo")
    message(FATAL_ERROR "module list did not report wl2:echo\n${_out}")
endif()

run_expect("module list all" "ok" _out "${WL2_EXECUTABLE}" module list --scope all)
if(NOT _out MATCHES "wl2:echo")
    message(FATAL_ERROR "module list --scope all did not report wl2:echo\n${_out}")
endif()

run_expect("module list local" "ok" _out "${WL2_EXECUTABLE}" module list --scope local)
if(NOT _out MATCHES "wl2:echo")
    message(FATAL_ERROR "module list --scope local did not report wl2:echo\n${_out}")
endif()

run_expect("module list user before install" "ok" _out "${WL2_EXECUTABLE}" module list --scope user)
if(NOT _out MATCHES "no installed modules")
    message(FATAL_ERROR "module list --scope user should be empty before user install\n${_out}")
endif()

# Now the required module resolves from the local scope and the script runs.
run_expect("run after install" "ok" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
if(NOT _out MATCHES "resolved: OK")
    message(FATAL_ERROR "installed module was not resolved at run time\n${_out}")
endif()

# config reports the installed module.
run_expect("config installed" "ok" _out "${WL2_EXECUTABLE}" config --manifest wl2.yml)
if(NOT _out MATCHES "installed modules")
    message(FATAL_ERROR "config did not report installed modules\n${_out}")
endif()

# Tampering with the installed library is detected before it is loaded: the run
# is refused with a stable checksum-mismatch error. Reinstall to restore.
file(GLOB _echo_libs "${_project}/.wl2/modules/wl2_echo/*.so" "${_project}/.wl2/modules/wl2_echo/*.dylib")
if(_echo_libs)
    list(GET _echo_libs 0 _echo_lib)
    file(APPEND "${_echo_lib}" "tampered")
    run_expect("run tampered module" "fail" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
    if(NOT _out MATCHES "recorded checksum")
        message(FATAL_ERROR "tampered installed module was not detected\n${_out}")
    endif()
    run_expect("reinstall after tamper" "ok" _out "${WL2_EXECUTABLE}" module install "${WL2_ECHO_SO}" --scope local)
    run_expect("run after reinstall" "ok" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
    if(NOT _out MATCHES "resolved: OK")
        message(FATAL_ERROR "reinstalled module did not run\n${_out}")
    endif()
endif()

# Uninstall is refused because the manifest references the module.
run_expect("uninstall referenced" "fail" _out "${WL2_EXECUTABLE}" module uninstall wl2:echo --scope local)
if(NOT _out MATCHES "module_referenced")
    message(FATAL_ERROR "expected module_referenced\n${_out}")
endif()

# With --force it is removed.
run_expect("uninstall forced" "ok" _out "${WL2_EXECUTABLE}" module uninstall wl2:echo --scope local --force)
if(EXISTS "${_project}/.wl2/modules/wl2_echo")
    message(FATAL_ERROR "forced uninstall did not remove the payload")
endif()

# User-scope install uses the temporary XDG data directory, and scoped list
# filters to that scope.
run_expect("module install user" "ok" _out "${WL2_EXECUTABLE}" module install "${WL2_ECHO_SO}" --scope user)
if(NOT EXISTS "$ENV{XDG_DATA_HOME}/wl2/modules/wl2_echo/wl2.module.yml")
    message(FATAL_ERROR "user-scope install did not use the temporary XDG data directory")
endif()
run_expect("module list user after install" "ok" _out "${WL2_EXECUTABLE}" module list --scope user)
if(NOT _out MATCHES "wl2:echo" OR NOT _out MATCHES "\\[user\\]")
    message(FATAL_ERROR "module list --scope user did not report the user install\n${_out}")
endif()
run_expect("module list bad scope" "fail" _out "${WL2_EXECUTABLE}" module list --scope nowhere)
if(NOT _out MATCHES "invalid scope")
    message(FATAL_ERROR "module list --scope nowhere should fail clearly\n${_out}")
endif()
run_expect("uninstall user forced" "ok" _out "${WL2_EXECUTABLE}" module uninstall wl2:echo --scope user --force)

# --- wl2 deps against a local git repository ---------------------------------
find_program(_git git)
if(_git)
    set(_repo "${WORK_DIR}/dep-repo")
    file(MAKE_DIRECTORY "${_repo}")
    execute_process(COMMAND ${_git} -C "${_repo}" init -q)
    execute_process(COMMAND ${_git} -C "${_repo}" config user.email test@example.com)
    execute_process(COMMAND ${_git} -C "${_repo}" config user.name Test)
    file(WRITE "${_repo}/mod.txt" "dependency module\n")
    execute_process(COMMAND ${_git} -C "${_repo}" add mod.txt)
    execute_process(COMMAND ${_git} -C "${_repo}" commit -q -m initial)
    execute_process(COMMAND ${_git} -C "${_repo}" tag v1.0.0)

    file(WRITE "${_project}/wl2.yml"
        "schema: wl2.project.v1\nprefix: wl2:/app\nroot: files\nentry: main.js\ndependencies:\n  modules:\n    - name: wl2_dep\n      git: ${_repo}\n      tag: v1.0.0\n")

    run_expect("deps lock" "ok" _out "${WL2_EXECUTABLE}" deps lock)
    if(NOT EXISTS "${_project}/wl2.lock.yml")
        message(FATAL_ERROR "deps lock did not write wl2.lock.yml")
    endif()
    file(READ "${_project}/wl2.lock.yml" _lock)
    if(NOT _lock MATCHES "commit:")
        message(FATAL_ERROR "lockfile has no commit\n${_lock}")
    endif()

    run_expect("deps fetch" "ok" _out "${WL2_EXECUTABLE}" deps fetch)
    if(NOT IS_DIRECTORY "${_project}/.wl2/deps/wl2_dep/.git")
        message(FATAL_ERROR "deps fetch did not populate project-local storage")
    endif()

    run_expect("deps status" "ok" _out "${WL2_EXECUTABLE}" deps status)
    if(NOT _out MATCHES "fetched=yes")
        message(FATAL_ERROR "deps status did not report the fetched dependency\n${_out}")
    endif()
else()
    message(STATUS "git not found; skipping wl2 deps CLI checks")
endif()

message(STATUS "deps/modules CLI checks passed")
