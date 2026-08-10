add_test(NAME scripts.wl2_uv_network_interfaces
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --load-module $<TARGET_FILE:wl2_uv>
        --allow-listen
        --listen-allow=0.0.0.0:0
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/network-interfaces.test.js)
set_tests_properties(scripts.wl2_uv_network_interfaces PROPERTIES
    LABELS "js;uv;network"
    TIMEOUT 15)

add_test(NAME scripts.wl2_uv_permission_denied
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --load-module $<TARGET_FILE:wl2_uv>
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/permission-denied.test.js)
set_tests_properties(scripts.wl2_uv_permission_denied PROPERTIES
    LABELS "js;uv;permissions"
    TIMEOUT 15)

if(TARGET wl2_uv)
    add_test(NAME scripts.wl2_uv_module_validate
        COMMAND $<TARGET_FILE:wl2> module validate $<TARGET_FILE:wl2_uv>)
    set_tests_properties(scripts.wl2_uv_module_validate PROPERTIES
        LABELS "js;uv;module")
endif()
