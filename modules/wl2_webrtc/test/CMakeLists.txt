add_executable(wl2_webrtc_tests wl2_webrtc_tests.cpp)
target_link_libraries(wl2_webrtc_tests PRIVATE wl2_core wl2_webrtc_static wl2_membus_static)

# Deterministic and self-contained: two in-process peers negotiate over loopback
# host candidates. No STUN/TURN, no browser, no external signaling server.
add_test(NAME webrtc.wl2_webrtc COMMAND wl2_webrtc_tests)
set_tests_properties(webrtc.wl2_webrtc PROPERTIES
    LABELS "webrtc;js;integration"
    TIMEOUT 60)

if(TARGET wl2_gstreamer_static)
    add_test(NAME webrtc.gstreamer_packetbuffer_interop
        COMMAND
            $<TARGET_FILE:wl2>
            run
            --allow-shared-memory
            --shared-memory-allow /wl2_webrtc
            ${CMAKE_CURRENT_SOURCE_DIR}/js/gstreamer_packetbuffer_interop.test.js)
    set_tests_properties(webrtc.gstreamer_packetbuffer_interop PROPERTIES
        LABELS "webrtc;gstreamer;js;integration;membus"
        TIMEOUT 60)
endif()
