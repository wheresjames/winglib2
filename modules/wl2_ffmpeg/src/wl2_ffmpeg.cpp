// wl2_ffmpeg - FFmpeg-backed media discovery and archive replay module
// This file is the entry point; implementation is split across ffmpeg/*.cpp
// files which are included here to compile as a single translation unit.

#include "wl2_ffmpeg/wl2_ffmpeg.h"

#include "wl2/membus.h"
#include "wl2/runtime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#if WL2_FFMPEG_HAVE_FILTERS
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif

namespace {

#include "ffmpeg/types.cpp"
#include "ffmpeg/demuxer.cpp"
#include "ffmpeg/decoder.cpp"
#include "ffmpeg/replay.cpp"
#include "ffmpeg/muxer.cpp"
#include "ffmpeg/evidence.cpp"
#include "ffmpeg/filter.cpp"

// JS namespace factories and module init are defined in module.cpp but need
// to be inside the anonymous namespace for internal linkage
#include "ffmpeg/module_internal.cpp"

} // namespace

// Module registration functions must be outside anonymous namespace for visibility
#include "ffmpeg/module_exports.cpp"