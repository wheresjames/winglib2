add_test(NAME scripts.wl2_onvif_direct_client
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/direct-client.test.js)
set_tests_properties(scripts.wl2_onvif_direct_client PROPERTIES
    LABELS "js;onvif;integration;direct-client"
    TIMEOUT 20)

add_test(NAME scripts.wl2_onvif_events
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/events.test.js)
set_tests_properties(scripts.wl2_onvif_events PROPERTIES
    LABELS "js;onvif;integration;events"
    TIMEOUT 20)
