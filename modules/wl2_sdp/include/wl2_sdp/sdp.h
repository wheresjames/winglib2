#pragma once

/**
 * @file sdp.h
 * @brief Dependency-free SDP (RFC 4566 / 8866) parser, builder, and munging.
 *
 * The canonical representation is an ordered list of raw lines, not a
 * name-keyed attribute map. SDP legitimately repeats lines (`a=candidate`,
 * `a=ssrc`, one `a=rtpmap` per payload type, `a=extmap`, `a=rid`), and their
 * order is significant to real WebRTC/SIP stacks. Storing the raw lines in order
 * guarantees a lossless round-trip: `build(parse(x)) == x`. Typed accessors
 * (rtpmap, candidate, fingerprint, ...) are read-only *views* computed on demand
 * over those lines, never the storage, so the convenience layer can never lose
 * or reorder data.
 *
 * The surface is free of any third-party headers so it can be linked by modules
 * built without a JSON library, matching the wl2:media policy.
 */

#include "wl2/errors.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wl2::sdp {

/// Stable, machine-readable error codes surfaced as the `code` field on the
/// shared Winglib2 module error shape (`name: "SdpError"`).
namespace errors {
inline constexpr const char* InvalidArgument = "sdp_invalid_argument"; ///< Missing/wrong-typed argument.
inline constexpr const char* ParseFailed = "sdp_parse_failed";        ///< Malformed line in strict mode.
inline constexpr const char* TooLarge = "sdp_too_large";              ///< Input or section count exceeded a cap.
inline constexpr const char* NoSession = "sdp_no_session";            ///< Required v=/s= missing (strict).
inline constexpr const char* BuildFailed = "sdp_build_failed";        ///< Object cannot be serialized.
} // namespace errors

/// One raw SDP line, preserved verbatim and in original order. Covers known and
/// unknown line types and duplicates with zero loss. @c type is the single
/// character before '='; @c value is everything after it.
struct Line {
    char type = 'a';
    std::string value;
};

/// A media section, introduced by an `m=` line. Everything under the `m=` line
/// up to the next `m=` (or end) is kept raw in @c lines.
struct Media {
    std::string kind;                    ///< "audio" | "video" | "application" | ...
    std::string port;                    ///< kept as string: "9", "9/2", edge cases.
    std::string proto;                   ///< e.g. "UDP/TLS/RTP/SAVPF".
    std::vector<std::string> formats;    ///< payload types / fmt list, order preserved.
    std::vector<Line> lines;             ///< all lines under this m=, in order.
};

/// A parsed session. Session-level lines (before the first `m=`) plus media.
struct Session {
    std::vector<Line> lines;             ///< session-level lines, in order.
    std::vector<Media> media;            ///< media sections, in order.
    bool crlf = true;                    ///< input used CRLF line endings.
    bool trailingNewline = true;         ///< input ended with a line terminator.
    std::vector<std::string> warnings;   ///< malformed lines collected in lenient mode.
};

/// Options controlling parse strictness and hostile-input caps.
struct ParseOptions {
    bool lenient = false;                ///< collect malformed lines as warnings instead of failing.
    bool crlfOnly = false;               ///< reject bare LF (default accepts LF and CRLF).
    std::size_t maxLen = 1u << 20;       ///< hard cap on total input bytes.
    std::size_t maxMedia = 512;          ///< cap on m= sections.
    std::size_t maxLines = 100000;       ///< cap on total non-empty lines.
};

/// Line ending used by @ref build.
enum class LineEnding {
    Auto,  ///< follow Session::crlf.
    Lf,    ///< force "\n".
    Crlf,  ///< force "\r\n".
};

/// Options controlling serialization.
struct BuildOptions {
    /// When false, emit stored order for an exact round-trip (the munging path).
    /// When true, sort each section into RFC 4566 field order and inject a
    /// required `v=0` / `s=-` when absent (the "clean SDP" path).
    bool canonical = false;
    LineEnding ending = LineEnding::Auto;
};

// --- Parse / build -------------------------------------------------------

/// Parse SDP text. Strict by default; see ParseOptions for lenient mode.
Result<Session> parse(std::string_view text);
Result<Session> parse(std::string_view text, const ParseOptions& options);

/// Serialize a session back to SDP text.
std::string build(const Session& session, const BuildOptions& options = {});

// --- Typed views (read-only projections over a section's lines) ----------

struct Rtpmap {
    int payload = -1;
    std::string codec;
    int clock = 0;
    int channels = 0;   ///< 0 when unspecified.
};

struct Fmtp {
    int payload = -1;
    std::string params; ///< raw "a=b;c=d" kept verbatim.
};

struct Candidate {
    std::string foundation;
    int component = 0;
    std::string proto;
    int64_t priority = 0;
    std::string ip;
    int port = 0;
    std::string type;   ///< host | srflx | prflx | relay (from "typ").
    std::vector<std::pair<std::string, std::string>> ext; ///< raddr/rport/tcptype/... pairs.
};

struct Fingerprint {
    std::string hashFunc; ///< e.g. "sha-256".
    std::string value;    ///< colon-separated hex.
};

struct ExtMap {
    int id = 0;
    std::string direction; ///< "", "sendonly", "recvonly", "sendrecv", "inactive".
    std::string uri;
};

struct Ssrc {
    uint32_t id = 0;
    std::string attribute; ///< e.g. "cname".
    std::string value;     ///< empty for a bare flag.
};

std::vector<Rtpmap> rtpmaps(const Media& media);
std::vector<Fmtp> fmtps(const Media& media);
std::vector<Candidate> candidates(const Media& media);
std::vector<ExtMap> extmaps(const Media& media);
std::vector<Ssrc> ssrcs(const Media& media);

/// First `a=fingerprint:` in the section, if any. Media falls back to session.
std::optional<Fingerprint> fingerprint(const Media& media);
std::optional<Fingerprint> fingerprint(const Session& session);

/// First value of `a=<key>:<value>` (empty string for a bare `a=<key>` flag);
/// std::nullopt when absent. Overloads work on either scope.
std::optional<std::string> attribute(const Media& media, std::string_view key);
std::optional<std::string> attribute(const Session& session, std::string_view key);

/// All values for a repeated attribute key, in order.
std::vector<std::string> attributes(const Media& media, std::string_view key);
std::vector<std::string> attributes(const Session& session, std::string_view key);

// --- Munging helpers (preserve order/multiplicity) -----------------------

/// Replace the first `a=<key>:...` with `a=<key>:<value>`, or append when absent.
void setAttribute(Media& media, std::string_view key, std::string_view value);
void setAttribute(Session& session, std::string_view key, std::string_view value);

/// Set a valueless flag attribute `a=<key>` (replace-first-or-append).
void setFlag(Media& media, std::string_view key);
void setFlag(Session& session, std::string_view key);

/// Always append a raw attribute line `a=<rawValue>`.
void addAttribute(Media& media, std::string_view rawValue);
void addAttribute(Session& session, std::string_view rawValue);

/// Remove every `a=<key>` / `a=<key>:...` line. Returns the number removed.
std::size_t removeAttribute(Media& media, std::string_view key);
std::size_t removeAttribute(Session& session, std::string_view key);

/// Keep only the given payload types: filters the `m=` format list and drops
/// orphaned `a=rtpmap`/`a=fmtp`/`a=rtcp-fb` lines (wildcards are kept).
void keepPayloads(Media& media, const std::vector<int>& payloads);

/// Reorder the `m=` format list so the given payloads come first (codec
/// preference). Payloads not present are ignored; unlisted formats keep their
/// original relative order at the end. Does not remove any lines.
void reorderPayloads(Media& media, const std::vector<int>& order);

// --- Comparison (test helper) --------------------------------------------

/// Order-normalized *textual* equality: media order is significant, but lines
/// within a scope are compared as sorted sets and line endings are ignored.
/// This is a test helper, not protocol-level semantic equivalence.
bool equalNormalized(const Session& a, const Session& b);

// --- Small shared helpers (exposed for the JS binding and tests) ---------

/// Split an attribute line value into key and value around the first ':'.
/// Returns true when a ':' was present (value may still be empty).
bool splitAttribute(std::string_view lineValue, std::string& key, std::string& value);

} // namespace wl2::sdp
