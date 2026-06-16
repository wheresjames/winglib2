# Out-of-tree module test driver.
#
# Installs the just-built Winglib2 into a throwaway prefix, then configures,
# builds, and tests examples/modules/wl2_echo standalone against that installed
# package via find_package(winglib2 CONFIG REQUIRED). This proves a third-party
# module can be built and tested with nothing but the installed package.
#
# Required -D variables:
#   MAIN_BUILD_DIR  Winglib2 build directory to install from.
#   EXAMPLE_DIR     Source directory of the wl2_echo example.
#   WORK_DIR        Scratch directory (install/src/build live underneath).
#   GENERATOR       CMake generator to use for the example build.
#   CTEST_COMMAND   Path to the ctest executable.
# Optional:
#   BUILD_TYPE      Configuration to install/build/test.

foreach(_required MAIN_BUILD_DIR EXAMPLE_DIR WORK_DIR GENERATOR CTEST_COMMAND)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

# Build in parallel. Use an explicit job count so `--parallel` never expands to
# an unbounded `make -j`.
include(ProcessorCount)
ProcessorCount(_parallel_jobs)
if(_parallel_jobs EQUAL 0)
    set(_parallel_jobs 1)
endif()

set(_install_dir "${WORK_DIR}/install")
set(_src_dir "${WORK_DIR}/src")
set(_build_dir "${WORK_DIR}/build")

# Start from a clean slate so reruns are deterministic.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Copy the example out of the source tree so its configure step never writes
# into the repository.
file(COPY "${EXAMPLE_DIR}/" DESTINATION "${_src_dir}")

set(_config_args)
if(BUILD_TYPE)
    list(APPEND _config_args "--config" "${BUILD_TYPE}")
endif()

function(wl2_run_step description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${description} failed (${_result})\n--- stdout ---\n${_stdout}\n--- stderr ---\n${_stderr}")
    endif()
endfunction()

# 1. Install Winglib2 into the throwaway prefix.
set(_install_command "${CMAKE_COMMAND}" --install "${MAIN_BUILD_DIR}" --prefix "${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _install_command --config "${BUILD_TYPE}")
endif()
wl2_run_step("install winglib2" ${_install_command})

# 2. Configure the example against the installed package.
set(_configure_command
    "${CMAKE_COMMAND}" -S "${_src_dir}" -B "${_build_dir}" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${_install_dir}")
if(BUILD_TYPE)
    list(APPEND _configure_command "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
wl2_run_step("configure wl2_echo" ${_configure_command})

# 3. Build the example (static module, dynamic module, runner, and test).
wl2_run_step("build wl2_echo" "${CMAKE_COMMAND}" --build "${_build_dir}" --parallel ${_parallel_jobs} ${_config_args})

# 4. Run the example's tests.
set(_ctest_command "${CTEST_COMMAND}" --test-dir "${_build_dir}" --output-on-failure)
if(BUILD_TYPE)
    list(APPEND _ctest_command --build-config "${BUILD_TYPE}")
endif()
wl2_run_step("ctest wl2_echo" ${_ctest_command})

message(STATUS "out-of-tree wl2_echo build and test passed")
