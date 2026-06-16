function(wl2_find_v8)
    if(TARGET v8)
        return()
    endif()

    if(WL2_V8_PROVIDER STREQUAL "off")
        message(FATAL_ERROR "WL2_JS_ENGINE=v8 requires V8, but WL2_V8_PROVIDER=off")
    endif()

    set(_candidates)

    if(WL2_V8_ROOT)
        list(APPEND _candidates "${WL2_V8_ROOT}")
    endif()
    if(WL2_DEPS_ROOT)
        list(APPEND _candidates "${WL2_DEPS_ROOT}/v8")
    endif()

    if(NOT WL2_V8_PROVIDER STREQUAL "local")
        list(APPEND _candidates
            "${CMAKE_CURRENT_LIST_DIR}/../../_wl2/lib3/linux-x86_64/release/v8/14.6.206/v8-install"
            "${CMAKE_CURRENT_LIST_DIR}/../../../_wl2/lib3/linux-x86_64/release/v8/14.6.206/v8-install")
    endif()

    foreach(_root IN LISTS _candidates)
        if(EXISTS "${_root}/include/v8.h" AND EXISTS "${_root}/lib/libv8_monolith.a")
            add_library(v8 STATIC IMPORTED GLOBAL)
            set_target_properties(v8 PROPERTIES
                IMPORTED_LOCATION "${_root}/lib/libv8_monolith.a"
                INTERFACE_INCLUDE_DIRECTORIES "${_root}/include")
            if(WL2_V8_EXTRA_LIBRARIES)
                set_target_properties(v8 PROPERTIES
                    INTERFACE_LINK_LIBRARIES "${WL2_V8_EXTRA_LIBRARIES}")
            endif()
            message(STATUS "Using V8 from ${_root}")
            return()
        endif()
    endforeach()

    message(FATAL_ERROR "V8 not found. Expected local install at ${WL2_DEPS_ROOT}/v8, or set WL2_V8_ROOT.")
endfunction()
