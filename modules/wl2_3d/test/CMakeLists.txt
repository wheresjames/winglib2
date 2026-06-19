add_executable(wl2_3d_tests wl2_3d_tests.cpp)
target_link_libraries(wl2_3d_tests PRIVATE wl2_core wl2_3d_static)

add_test(NAME three_d.wl2_3d COMMAND wl2_3d_tests)
set_tests_properties(three_d.wl2_3d PROPERTIES
    LABELS "3d;js;unit;integration"
    TIMEOUT 30)

# Headless GPU renderer probe: renders the engine model on the GPU and the CPU,
# writes both to PPM for A/B, checks bounded parity error, and records software
# GL render/readback timing. Built only when Magnum is available; runs under
# surfaceless EGL + llvmpipe so it works on headless CI.
if(WL2_3D_HAVE_MAGNUM)
    add_executable(wl2_3d_gpu_probe gpu_probe.cpp)
    target_include_directories(wl2_3d_gpu_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(wl2_3d_gpu_probe PRIVATE
        WL2_3D_HAVE_MAGNUM=1
        WL2_3D_GPU_SAMPLES=${WL2_3D_GPU_SAMPLES})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(wl2_3d_gpu_probe PRIVATE -Wno-cpp)
    endif()
    target_link_libraries(wl2_3d_gpu_probe PRIVATE
        Magnum::Magnum Magnum::GL Magnum::WindowlessEglApplication)
    add_test(NAME three_d.wl2_3d_gpu_probe
        COMMAND wl2_3d_gpu_probe ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(three_d.wl2_3d_gpu_probe PROPERTIES
        ENVIRONMENT "EGL_PLATFORM=surfaceless;MESA_LOADER_DRIVER_OVERRIDE=llvmpipe"
        LABELS "3d;gpu;gpu_render;unit;parity;performance"
        TIMEOUT 120)
endif()

# Spike: unproject/project and engine-core math (renderer-independent).
add_executable(wl2_3d_camera_tests camera_tests.cpp)
add_test(NAME three_d.wl2_3d_camera COMMAND wl2_3d_camera_tests)
set_tests_properties(three_d.wl2_3d_camera PROPERTIES
    LABELS "3d;unit"
    TIMEOUT 30)

if(UNIX AND NOT EMSCRIPTEN)
    add_executable(wl2_3d_process_isolation_tests process_isolation_tests.cpp)
    target_link_libraries(wl2_3d_process_isolation_tests PRIVATE wl2_core)
    add_test(NAME three_d.wl2_3d_process_isolation COMMAND wl2_3d_process_isolation_tests)
    set_tests_properties(three_d.wl2_3d_process_isolation PROPERTIES
        LABELS "3d;membus;process;integration"
        TIMEOUT 30)
endif()

# WASM readiness (§15): prove the renderer-independent engine core — math, scene
# graph, particles, detections, and the FrameRing transport abstraction — compiles
# for wasm32. This is the regression guard against native-only / WebGL2-incompatible
# code creeping into the portable core. Gated on the Emscripten toolchain (emcc);
# when node is present the wasm module is also executed to confirm behavior.
find_program(WL2_3D_EMCC emcc)
find_program(WL2_3D_NODE node)
if(WL2_3D_EMCC)
    set(_wl2_3d_wasm_out ${CMAKE_CURRENT_BINARY_DIR}/wl2_3d_wasm32_core.js)
    add_test(NAME three_d.wl2_3d_wasm32_compile
        COMMAND ${WL2_3D_EMCC} -std=c++17 -O1
            ${CMAKE_CURRENT_SOURCE_DIR}/camera_tests.cpp
            -o ${_wl2_3d_wasm_out} -sWASM=1 -sEXIT_RUNTIME=1)
    set_tests_properties(three_d.wl2_3d_wasm32_compile PROPERTIES
        LABELS "3d;wasm;unit"
        TIMEOUT 300)
    if(WL2_3D_NODE)
        add_test(NAME three_d.wl2_3d_wasm32_run COMMAND ${WL2_3D_NODE} ${_wl2_3d_wasm_out})
        set_tests_properties(three_d.wl2_3d_wasm32_run PROPERTIES
            LABELS "3d;wasm;unit"
            DEPENDS three_d.wl2_3d_wasm32_compile
            PASS_REGULAR_EXPRESSION "camera/engine ok"
            TIMEOUT 60)
    endif()
else()
    message(STATUS "emcc not found; wl2_3d wasm32 compile lane disabled")
endif()

# Headless JS demo: markers + attention, picking, animateTo, detections.
add_test(NAME scripts.wl2_3d_engine_core
    COMMAND $<TARGET_FILE:wl2> run ${CMAKE_CURRENT_SOURCE_DIR}/js/engine_core.test.js)
set_tests_properties(scripts.wl2_3d_engine_core PROPERTIES
    LABELS "3d;js;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_3d_content_assets
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --map-resource
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures:wl2:/3d-fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/js/content_assets.test.js)
set_tests_properties(scripts.wl2_3d_content_assets PROPERTIES
    LABELS "3d;js;resources;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_3d_rich_scene
    COMMAND $<TARGET_FILE:wl2> run ${CMAKE_CURRENT_SOURCE_DIR}/js/rich_scene.test.js)
set_tests_properties(scripts.wl2_3d_rich_scene PROPERTIES
    LABELS "3d;js;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_3d_dynamic_mesh
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow-graphics
        --allow-shared-memory
        --shared-memory-allow /wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/dynamic_mesh.test.js)
set_tests_properties(scripts.wl2_3d_dynamic_mesh PROPERTIES
    LABELS "3d;js;membus;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_3d_allow_alias
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow graphics,shared-memory:/wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/dynamic_mesh.test.js)
set_tests_properties(scripts.wl2_3d_allow_alias PROPERTIES
    LABELS "3d;js;membus;cli;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_3d_camera_timeline
    COMMAND
        $<TARGET_FILE:wl2>
        run
        # publishTo() renders a frame; with a real renderer that creates a
        # headless graphics context, which authorizeGraphics() gates.
        --allow-graphics
        --allow-shared-memory
        --shared-memory-allow /wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/camera_timeline.test.js)
set_tests_properties(scripts.wl2_3d_camera_timeline PROPERTIES
    LABELS "3d;js;membus;integration"
    TIMEOUT 30)

# Detections -> scene: a synthetic detector publishes moving "car" hits on a
# memmsg queue; the scene tracks them onto the ground plane. Needs shared memory.
add_test(NAME scripts.wl2_3d_detections
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow-shared-memory
        --shared-memory-allow /wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/detections.test.js)
set_tests_properties(scripts.wl2_3d_detections PROPERTIES
    LABELS "3d;js;membus;integration"
    TIMEOUT 30)

# Hardening: detection-decoder fuzz, denied capability, untrusted-buffer
# rejection, and dynamic resize. Needs shared memory for the resize path.
add_test(NAME scripts.wl2_3d_hardening
    COMMAND
        $<TARGET_FILE:wl2>
        run
        # The resize path calls publishTo()/resize(), which render a frame and
        # (with a real renderer) create a headless graphics context. The
        # denied-capability check uses a disallowed shared-memory prefix, which
        # stays denied regardless of the graphics capability.
        --allow-graphics
        --allow-shared-memory
        --shared-memory-allow /wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/hardening.test.js)
set_tests_properties(scripts.wl2_3d_hardening PROPERTIES
    LABELS "3d;js;membus;security;integration"
    TIMEOUT 30)

# UI-on-3D full round-trip across wl2:3d + wl2:slint: surface pick -> UV -> UI
# pixel -> injected click -> UI re-renders -> ring updates. Needs shared memory.
add_test(NAME scripts.wl2_3d_ui_on_3d_roundtrip
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow-shared-memory
        --shared-memory-allow /wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/ui_on_3d_roundtrip.test.js)
set_tests_properties(scripts.wl2_3d_ui_on_3d_roundtrip PROPERTIES
    LABELS "3d;js;slint;membus;integration"
    TIMEOUT 30)

add_test(NAME scripts.wl2_3d_synthetic_frame
    COMMAND
        $<TARGET_FILE:wl2>
        run
        --allow-graphics
        --allow-shared-memory
        --shared-memory-allow /wl2_3d_test_
        ${CMAKE_CURRENT_SOURCE_DIR}/js/synthetic_frame.test.js)
set_tests_properties(scripts.wl2_3d_synthetic_frame PROPERTIES
    LABELS "3d;js;membus;integration"
    TIMEOUT 30)
