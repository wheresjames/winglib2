#pragma once

/**
 * @file schema.h
 * @brief Backend-agnostic media schemas shared across Winglib2 media modules.
 *
 * These types and helpers are the single source of truth for the media metadata
 * that flows between backends (wl2:gstreamer, wl2:ffmpeg) and through libmembus
 * `PacketBuffer` main/per-record metadata. The canonical wire form is a
 * JSON-compatible string, so both native modules serialize and parse through one
 * code path and interoperate without depending on each other.
 *
 * The C++ surface is intentionally small and free of any backend headers so it
 * can be linked by modules that are built without GStreamer or FFmpeg present.
 */

#include "wl2/errors.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace wl2::media {

/// Packet metadata schema major version. Readers reject a different major.
inline constexpr int kPacketSchema = 1;

/// Stream descriptor schema major version. Readers reject a different major.
inline constexpr int kStreamSchema = 1;

/// Default JS-boundary time base. GStreamer's native unit is nanoseconds.
inline constexpr const char* kDefaultTimeBase = "1/1000000000";

/// Stable, machine-readable error codes for shared media failures. Backends
/// reuse the Winglib2 module error shape and surface these as the `code` field.
namespace errors {
inline constexpr const char* InvalidArgument = "media_invalid_argument";
inline constexpr const char* InvalidSchema = "media_invalid_schema";
inline constexpr const char* UnsupportedMediaType = "media_unsupported_media_type";
inline constexpr const char* InvalidTimeBase = "media_invalid_time_base";
inline constexpr const char* ParseFailed = "media_parse_failed";
} // namespace errors

/// Named backpressure profiles. Backends map these to concrete drop/block
/// behavior; the names are shared so scripts do not repeat strings per backend.
namespace backpressure {
inline constexpr const char* Record = "record";       ///< block or time out; lossless.
inline constexpr const char* Transcode = "transcode"; ///< block with configurable timeout.
inline constexpr const char* Preview = "preview";     ///< drop oldest frames/packets.
inline constexpr const char* Relay = "relay";         ///< drop non-keyframes until a keyframe.
} // namespace backpressure

/// Whether a backpressure profile name is one of the shared profiles.
bool isKnownBackpressureProfile(std::string_view name) noexcept;

/// Raw video format descriptor (canonical caps-style fields).
struct VideoFormat {
    std::string format;    ///< e.g. "RGBA", "GRAY8".
    int64_t width = 0;
    int64_t height = 0;
    std::string framerate; ///< "num/den", e.g. "30/1". Empty when unknown.
};

/// Raw audio format descriptor.
struct AudioFormat {
    std::string format;  ///< e.g. "S16LE", "F32LE".
    int64_t rate = 0;
    int64_t channels = 0;
    std::string layout;  ///< e.g. "interleaved". Empty when unknown.
};

/// Stream-level descriptor stored in `PacketBuffer` main metadata.
struct StreamDescriptor {
    int schema = kStreamSchema;
    std::string mediaType;    ///< "video" | "audio" | "subtitle" | "data".
    std::string codec;        ///< canonical codec id, e.g. "h264".
    std::string caps;         ///< canonical caps string.
    std::string streamFormat; ///< e.g. "byte-stream".
    std::string alignment;    ///< e.g. "au".
    int64_t track = 0;
};

/// Per-record packet metadata stored alongside a `PacketBuffer` payload.
struct PacketMetadata {
    int schema = kPacketSchema;
    std::string codec;
    std::string mediaType;
    std::string caps;
    std::string streamFormat;
    std::string alignment;
    int64_t track = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    std::string timeBase = kDefaultTimeBase;
    uint32_t flags = 0;
    bool discontinuity = false;
    /// Opaque, backend-namespaced JSON carried verbatim. Empty when unused.
    std::string sideData;
};

/// Whether a media type string is one of the recognized values.
bool isKnownMediaType(std::string_view mediaType) noexcept;

/**
 * @brief Parse a "num/den" time base into its numerator and denominator.
 * @return True on a well-formed, non-zero-denominator time base.
 */
bool parseTimeBase(std::string_view timeBase, int64_t& num, int64_t& den) noexcept;

/**
 * @brief Convert a timestamp between two "num/den" time bases.
 *
 * The result is `value * fromTimeBase / toTimeBase` rounded to nearest, computed
 * in 128-bit intermediate precision. When either time base is malformed the
 * value is returned unchanged; validate the time bases first when that matters.
 */
int64_t convertTimestamp(int64_t value, std::string_view fromTimeBase, std::string_view toTimeBase) noexcept;

/// Validate a stream descriptor (schema major and media type).
Result<void> validate(const StreamDescriptor& descriptor);

/// Validate packet metadata (schema major, media type, and time base).
Result<void> validate(const PacketMetadata& metadata);

/// Serialize to the canonical JSON-compatible wire string.
std::string serialize(const StreamDescriptor& descriptor);
std::string serialize(const PacketMetadata& metadata);

/// Parse the canonical JSON-compatible wire string. Missing optional fields take
/// their defaults; a malformed object or wrong schema major fails.
Result<StreamDescriptor> parseStreamDescriptor(std::string_view json);
Result<PacketMetadata> parsePacketMetadata(std::string_view json);

} // namespace wl2::media
