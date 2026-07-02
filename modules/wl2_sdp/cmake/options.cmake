if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE DOC "Build wl2_sdp SDP parser/builder module")
endif()

# Optional libFuzzer target for the parser. Off by default and only usable with
# a Clang toolchain; the default build never compiles it.
option(WL2_SDP_FUZZ "Build the wl2:sdp libFuzzer parser target (Clang only)" OFF)
