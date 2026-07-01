function(wl2_find_ffmpeg)
    set(WL2_HAVE_FFMPEG FALSE PARENT_SCOPE)
    set(WL2_FFMPEG_TARGET "" PARENT_SCOPE)
    set(WL2_FFMPEG_IS_SYSTEM FALSE PARENT_SCOPE)

    find_package(PkgConfig QUIET)
    if(NOT PkgConfig_FOUND)
        message(STATUS "pkg-config not found; cannot discover FFmpeg for wl2_ffmpeg")
        return()
    endif()

    pkg_check_modules(WL2_FFMPEG QUIET IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample)

    if(NOT WL2_FFMPEG_FOUND)
        message(STATUS "FFmpeg development packages not found for wl2_ffmpeg")
        return()
    endif()

    set(WL2_HAVE_FFMPEG TRUE PARENT_SCOPE)
    set(WL2_FFMPEG_TARGET PkgConfig::WL2_FFMPEG PARENT_SCOPE)
    set(WL2_FFMPEG_IS_SYSTEM TRUE PARENT_SCOPE)
    set(WL2_FFMPEG_VERSION "${WL2_FFMPEG_VERSION}" PARENT_SCOPE)
endfunction()
