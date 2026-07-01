# Optional pinned external-media fetcher for wl2_ffmpeg.
#
# This script is only invoked by the wl2_ffmpeg_fetch_test_media target, which
# is gated behind -DWL2_FFMPEG_DOWNLOAD_TEST_MEDIA=ON. Default CI never runs it
# and never touches the network.
#
# Each entry is "name|url|sha256|relative-destination". Every downloaded file is
# verified against its pinned SHA-256 and stored under
# WL2_FFMPEG_TEST_MEDIA_ROOT, outside the source tree. Add entries (mirroring
# test/media.yml) only with a pinned URL, hash, size, and license note.

if(NOT WL2_FFMPEG_TEST_MEDIA_ROOT)
    message(FATAL_ERROR "WL2_FFMPEG_TEST_MEDIA_ROOT must be set to download external media")
endif()

# Pinned manifest. Empty by default so the target is a no-op until a maintainer
# adds verified entries for the selected profile.
set(WL2_FFMPEG_MEDIA_ENTRIES "")

if(NOT WL2_FFMPEG_MEDIA_ENTRIES)
    message(STATUS "No pinned wl2_ffmpeg media entries for profile '${WL2_FFMPEG_TEST_MEDIA_PROFILE}'; nothing to download")
    return()
endif()

file(MAKE_DIRECTORY "${WL2_FFMPEG_TEST_MEDIA_ROOT}")
foreach(entry IN LISTS WL2_FFMPEG_MEDIA_ENTRIES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 url)
    list(GET parts 2 sha256)
    list(GET parts 3 dest)
    set(target "${WL2_FFMPEG_TEST_MEDIA_ROOT}/${dest}")
    message(STATUS "Fetching ${name} -> ${target}")
    file(DOWNLOAD "${url}" "${target}"
        EXPECTED_HASH SHA256=${sha256}
        SHOW_PROGRESS
        STATUS download_status)
    list(GET download_status 0 code)
    if(NOT code EQUAL 0)
        list(GET download_status 1 reason)
        message(FATAL_ERROR "Failed to download ${name}: ${reason}")
    endif()
endforeach()
