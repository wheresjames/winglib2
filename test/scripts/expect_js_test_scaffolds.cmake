# CLI checks for JavaScript test mode and scaffolding.
#
# Required -D variables:
#   WL2_EXECUTABLE  Path to the wl2 runner.
#   MAIN_BUILD_DIR Winglib2 build directory to install from.
#   WORK_DIR       Scratch directory.
#   GENERATOR      CMake generator.
#   CTEST_COMMAND  Path to ctest.
# Optional:
#   BUILD_TYPE     Configuration for multi-config generators.

foreach(_required WL2_EXECUTABLE MAIN_BUILD_DIR WORK_DIR GENERATOR CTEST_COMMAND)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run_step desc)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(_combined "${_out}${_err}")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${desc} failed (${_rc})\n${_combined}")
    endif()
    set(RUN_STEP_OUTPUT "${_combined}" PARENT_SCOPE)
endfunction()

function(run_expect desc expect_rc work_dir out_var)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${work_dir}"
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

set(_test_project "${WORK_DIR}/test-project")
file(MAKE_DIRECTORY "${_test_project}/files" "${_test_project}/tests")
file(WRITE "${_test_project}/files/main.js" "console.log('test project');\n")
file(WRITE "${_test_project}/wl2.yml"
    "schema: wl2.project.v1\n"
    "prefix: wl2:/app\n"
    "root: files\n"
    "entry: main.js\n"
    "tests:\n"
    "  roots:\n"
    "    - tests\n"
    "  pattern: \"*.test.js\"\n")
file(WRITE "${_test_project}/tests/pass.test.js"
    "test('sync passes', () => {\n"
    "  assert(true, 'sync should pass');\n"
    "});\n"
    "test('async passes', async () => {\n"
    "  const value = await Promise.resolve(42);\n"
    "  assert(value === 42, 'async should pass');\n"
    "});\n")

run_expect("passing tests" "ok" "${_test_project}" _out "${WL2_EXECUTABLE}" test --manifest wl2.yml)
if(NOT _out MATCHES "ok sync passes" OR NOT _out MATCHES "ok async passes")
    message(FATAL_ERROR "passing test output missing sync/async results\n${_out}")
endif()

run_expect("filtered tests" "ok" "${_test_project}" _out "${WL2_EXECUTABLE}" test --manifest wl2.yml --filter async)
if(NOT _out MATCHES "ok async passes" OR _out MATCHES "ok sync passes")
    message(FATAL_ERROR "filter did not select only async test\n${_out}")
endif()

run_expect("json tests" "ok" "${_test_project}" _json "${WL2_EXECUTABLE}" test --manifest wl2.yml --json)
string(JSON _total GET "${_json}" total)
string(JSON _failed GET "${_json}" failed)
if(NOT _total EQUAL 2 OR NOT _failed EQUAL 0)
    message(FATAL_ERROR "unexpected wl2 test --json summary\n${_json}")
endif()

file(WRITE "${_test_project}/tests/fail.test.js"
    "test('sync fails', () => {\n"
    "  assert(false, 'expected failure');\n"
    "});\n"
    "test('async fails', async () => {\n"
    "  await Promise.resolve();\n"
    "  assert(false, 'expected async failure');\n"
    "});\n")
run_expect("failing tests" "fail" "${_test_project}" _out "${WL2_EXECUTABLE}" test --manifest wl2.yml --filter fails)
if(NOT _out MATCHES "not ok sync fails" OR NOT _out MATCHES "not ok async fails" OR NOT _out MATCHES "JavaScript stack:")
    message(FATAL_ERROR "failing test output missing failures or stack\n${_out}")
endif()

# Scaffolded app runs and its generated tests pass directly through wl2.
run_step("wl2 init" "${WL2_EXECUTABLE}" init sample_app)
set(_app_dir "${WORK_DIR}/sample_app")
run_expect("generated app run" "ok" "${_app_dir}" _out "${WL2_EXECUTABLE}" run --manifest wl2.yml)
if(NOT _out MATCHES "hello from sample_app")
    message(FATAL_ERROR "generated app did not run\n${_out}")
endif()
run_expect("generated app test" "ok" "${_app_dir}" _out "${WL2_EXECUTABLE}" test --manifest wl2.yml)
if(NOT _out MATCHES "main sync" OR NOT _out MATCHES "main async")
    message(FATAL_ERROR "generated app tests did not run\n${_out}")
endif()

# Install the just-built package once, then build both scaffolds out of tree.
set(_install_dir "${WORK_DIR}/install")
set(_config_args)
if(BUILD_TYPE)
    list(APPEND _config_args --config "${BUILD_TYPE}")
endif()
set(_install_command "${CMAKE_COMMAND}" --install "${MAIN_BUILD_DIR}" --prefix "${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _install_command --config "${BUILD_TYPE}")
endif()
run_step("install winglib2" ${_install_command})

set(_app_build "${WORK_DIR}/sample_app_build")
set(_configure_app "${CMAKE_COMMAND}" -S "${_app_dir}" -B "${_app_build}" -G "${GENERATOR}" "-DCMAKE_PREFIX_PATH=${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _configure_app "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
run_step("configure generated app" ${_configure_app})
run_step("build generated app" "${CMAKE_COMMAND}" --build "${_app_build}" ${_config_args})
set(_app_ctest "${CTEST_COMMAND}" --test-dir "${_app_build}" --output-on-failure)
if(BUILD_TYPE)
    list(APPEND _app_ctest --build-config "${BUILD_TYPE}")
endif()
run_step("ctest generated app" ${_app_ctest})

run_step("wl2 module new" "${WL2_EXECUTABLE}" module new widget)
set(_module_dir "${WORK_DIR}/wl2_widget")
set(_module_build "${WORK_DIR}/wl2_widget_build")
set(_configure_module "${CMAKE_COMMAND}" -S "${_module_dir}" -B "${_module_build}" -G "${GENERATOR}" "-DCMAKE_PREFIX_PATH=${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _configure_module "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
run_step("configure generated module" ${_configure_module})
run_step("build generated module" "${CMAKE_COMMAND}" --build "${_module_build}" ${_config_args})
set(_module_ctest "${CTEST_COMMAND}" --test-dir "${_module_build}" --output-on-failure)
if(BUILD_TYPE)
    list(APPEND _module_ctest --build-config "${BUILD_TYPE}")
endif()
run_step("ctest generated module" ${_module_ctest})

message(STATUS "JavaScript test/scaffold CLI checks passed")
