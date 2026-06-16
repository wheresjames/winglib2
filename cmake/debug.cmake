
#====================================================================
# Just a really useful debugging function
# https://stackoverflow.com/questions/9298278
#
# Example dump_cmake_variables("optional-key-search-term", "optional-value-search-term")
#
function(dump_cmake_variables)
    message(STATUS "\n--  ---- dump_cmake_variables(${ARGV}) ----")
    get_cmake_property(_variableNames VARIABLES)
    list (SORT _variableNames)
    foreach (_variableName ${_variableNames})
        if (ARGV0)
            unset(MATCHED)
            string(REGEX MATCH ${ARGV0} MATCHED "${_variableName}")
            if (NOT MATCHED)
                if (NOT ARGV1)
                    continue()
                endif()
                unset(MATCHED)
                string(REGEX MATCH ${ARGV1} MATCHED "${${_variableName}}")
                if (NOT MATCHED)
                    continue()
                endif()
            endif()
        endif()
        message(STATUS " >>>> ${_variableName}=${${_variableName}}")
    endforeach()
endfunction()

#====================================================================
# Show all targets
function(show_all_targets)
    message(STATUS "\n--  ---- show_all_targets() ----")
    get_property(_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target ${_targets})
        get_target_property(_type ${_target} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            message(STATUS " >>>> ${_target} (${_type})")
        endif()
    endforeach()
endfunction()
