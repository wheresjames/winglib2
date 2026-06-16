
#====================================================================

function(readProjectConfig)

    file(READ ${ARGV0} CFGSTR)
    string(REPLACE "\r" "\n" CFGSTR ${CFGSTR})
    set(CFGSTR "\n${CFGSTR}")
    string(REPLACE ";" ":" CFGSTR "${CFGSTR}")
    string(REPLACE "\n" ";" CFGSTR "${CFGSTR}")

    foreach(line IN LISTS CFGSTR)
        string(STRIP "${line}" line)
        # Skip blank lines and comments. Only lines that actually parse as
        # "key value" set a variable, so a non-matching line cannot silently
        # reuse the previous regex match.
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(line MATCHES "^([A-Za-z0-9_]+)[ \t]+(.+)$")
            string(TOUPPER "${CMAKE_MATCH_1}" key)
            set("APP${key}" "${CMAKE_MATCH_2}")
            set("APP${key}" "${CMAKE_MATCH_2}" PARENT_SCOPE)
        endif()
    endforeach()

    if("${APPBUILD}" STREQUAL "")
        string(TIMESTAMP APPBUILD "%Y.%m.%d.%H%M")
        set("APPBUILD" "${APPBUILD}" PARENT_SCOPE)
    endif()

endfunction()
