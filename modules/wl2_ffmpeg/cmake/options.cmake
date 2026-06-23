# This file is included both from the module CMakeLists and (after install) from
# the packaged module config, where wl2_module_option is not defined; guard the
# call so external consumers can include it safely.
if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE
        DEFAULT ${WL2_ENABLE_EXTENDED_MODULES}
        DOC "Build the wl2:ffmpeg extended media module")
endif()

set(WL2_FFMPEG_ENABLE_FILTERS OFF CACHE BOOL
    "Build optional libavfilter-backed filter graph support in wl2:ffmpeg")

set(WL2_FFMPEG_ENABLE_EXTERNAL_TEST_MEDIA OFF CACHE BOOL
    "Enable wl2_ffmpeg tests that use an external media root")
set(WL2_FFMPEG_TEST_MEDIA_ROOT "" CACHE PATH
    "Optional root containing external wl2_ffmpeg test media")
set(WL2_FFMPEG_DOWNLOAD_TEST_MEDIA OFF CACHE BOOL
    "Allow explicit wl2_ffmpeg test-media download targets")
set(WL2_FFMPEG_TEST_MEDIA_PROFILE "small" CACHE STRING
    "External wl2_ffmpeg test-media profile: small, codec, archive, or stress")
set_property(CACHE WL2_FFMPEG_TEST_MEDIA_PROFILE PROPERTY STRINGS small codec archive stress)
