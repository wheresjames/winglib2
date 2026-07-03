add_test(NAME scripts.wl2_json_smoke
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --no-permission-prompt
        ${CMAKE_CURRENT_SOURCE_DIR}/js/json_smoke.test.js)
set_tests_properties(scripts.wl2_json_smoke PROPERTIES
    LABELS "js;json")
