# Fixture project that exercises wl2_resolve_builtin_modules() in isolation by
# populating the static module registry global properties directly, then
# asserting the resolved order or triggering a configure-time failure. Selected
# by -DSCENARIO; the resolver lives in -DWL2_MODULES_FILE.
#
# Scenarios:
#   order            linear chain resolves dependency-first
#   diamond          diamond graph resolves each module once, deps first
#   optional_present a present optional dependency is ordered first
#   optional_missing an absent optional dependency is skipped (success)
#   missing          an absent required dependency fails configure
#   cycle            a dependency cycle fails configure
cmake_minimum_required(VERSION 3.24)
project(wl2_static_module_graph_fixture NONE)

if(NOT DEFINED WL2_MODULES_FILE)
    message(FATAL_ERROR "WL2_MODULES_FILE is required")
endif()
if(NOT DEFINED SCENARIO)
    message(FATAL_ERROR "SCENARIO is required")
endif()

include("${WL2_MODULES_FILE}")

# Register a fake static module. req/opt are "-" for none, or pipe-joined names.
function(fake_module name target reg req opt)
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_NAMES "${name}")
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_TARGETS "${target}")
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_REGISTRATION_FUNCTIONS "${reg}")
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_REQUIRES "${req}")
    set_property(GLOBAL APPEND PROPERTY WL2_STATIC_MODULE_OPTIONAL "${opt}")
endfunction()

function(expect_order actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "scenario ${SCENARIO}: expected order [${expected}] but got [${actual}]")
    endif()
endfunction()

if(SCENARIO STREQUAL "order")
    # Registration order is reversed relative to dependency order on purpose.
    fake_module("wl2:a" a_static a_reg "wl2:b" "-")
    fake_module("wl2:b" b_static b_reg "wl2:c" "-")
    fake_module("wl2:c" c_static c_reg "-" "-")
    wl2_resolve_builtin_modules(OUT_NAMES _names OUT_TARGETS _targets OUT_REGISTRATIONS _regs)
    expect_order("${_names}" "wl2:c;wl2:b;wl2:a")
    expect_order("${_targets}" "c_static;b_static;a_static")
    expect_order("${_regs}" "c_reg;b_reg;a_reg")
    message(STATUS "scenario order ok")
elseif(SCENARIO STREQUAL "diamond")
    fake_module("wl2:app" app_static app_reg "wl2:left|wl2:right" "-")
    fake_module("wl2:left" left_static left_reg "wl2:base" "-")
    fake_module("wl2:right" right_static right_reg "wl2:base" "-")
    fake_module("wl2:base" base_static base_reg "-" "-")
    wl2_resolve_builtin_modules(OUT_NAMES _names)
    list(LENGTH _names _n)
    if(NOT _n EQUAL 4)
        message(FATAL_ERROR "scenario diamond: expected 4 modules, got ${_n} [${_names}]")
    endif()
    list(FIND _names "wl2:base" _base_i)
    list(FIND _names "wl2:left" _left_i)
    list(FIND _names "wl2:right" _right_i)
    list(FIND _names "wl2:app" _app_i)
    if(_base_i GREATER _left_i OR _base_i GREATER _right_i
            OR _left_i GREATER _app_i OR _right_i GREATER _app_i)
        message(FATAL_ERROR "scenario diamond: not topologically ordered [${_names}]")
    endif()
    message(STATUS "scenario diamond ok")
elseif(SCENARIO STREQUAL "optional_present")
    fake_module("wl2:a" a_static a_reg "-" "wl2:b")
    fake_module("wl2:b" b_static b_reg "-" "-")
    wl2_resolve_builtin_modules(OUT_NAMES _names)
    expect_order("${_names}" "wl2:b;wl2:a")
    message(STATUS "scenario optional_present ok")
elseif(SCENARIO STREQUAL "optional_missing")
    fake_module("wl2:a" a_static a_reg "-" "wl2:absent")
    fake_module("wl2:b" b_static b_reg "-" "-")
    wl2_resolve_builtin_modules(OUT_NAMES _names)
    list(LENGTH _names _n)
    if(NOT _n EQUAL 2)
        message(FATAL_ERROR "scenario optional_missing: expected 2 modules, got [${_names}]")
    endif()
    message(STATUS "scenario optional_missing ok")
elseif(SCENARIO STREQUAL "missing")
    fake_module("wl2:a" a_static a_reg "wl2:absent" "-")
    wl2_resolve_builtin_modules(OUT_NAMES _names)
    message(FATAL_ERROR "scenario missing: resolve should have failed")
elseif(SCENARIO STREQUAL "cycle")
    fake_module("wl2:a" a_static a_reg "wl2:b" "-")
    fake_module("wl2:b" b_static b_reg "wl2:a" "-")
    wl2_resolve_builtin_modules(OUT_NAMES _names)
    message(FATAL_ERROR "scenario cycle: resolve should have failed")
else()
    message(FATAL_ERROR "unknown scenario: ${SCENARIO}")
endif()
