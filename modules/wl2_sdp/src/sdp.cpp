#include "wl2_sdp/sdp.h"

#include <algorithm>
#include <cstddef>

namespace wl2::sdp {

namespace {

// --- Small string utilities (bounds-safe, no raw pointer arithmetic) ------

// Split on single spaces, dropping empty tokens (tolerant of runs of spaces).
std::vector<std::string_view> split_spaces(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
        std::size_t start = i;
        while (i < s.size() && s[i] != ' ') {
            ++i;
        }
        if (i > start) {
            out.push_back(s.substr(start, i - start));
        }
    }
    return out;
}

// Parse a non-negative or signed integer. Returns false on any non-digit.
bool parse_int(std::string_view s, int64_t& out) {
    if (s.empty()) {
        return false;
    }
    std::size_t start = 0;
    bool negative = false;
    if (s[0] == '-' || s[0] == '+') {
        negative = s[0] == '-';
        start = 1;
    }
    if (start >= s.size()) {
        return false;
    }
    int64_t acc = 0;
    for (std::size_t i = start; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        acc = acc * 10 + (s[i] - '0');
    }
    out = negative ? -acc : acc;
    return true;
}

int to_int_or(std::string_view s, int fallback) {
    int64_t v = 0;
    return parse_int(s, v) ? static_cast<int>(v) : fallback;
}

// The single character that introduces an attribute line.
constexpr char kAttr = 'a';

// Return the raw value of the first `a=<key>...` line, split into key/value.
// `matchKey` compares only the key (text before the first ':').
bool attribute_key_is(const Line& line, std::string_view key) {
    if (line.type != kAttr) {
        return false;
    }
    std::string k;
    std::string v;
    splitAttribute(line.value, k, v);
    return k == key;
}

// --- Typed view parsing over a line list ----------------------------------

std::optional<Fingerprint> fingerprint_in(const std::vector<Line>& lines) {
    for (const Line& line : lines) {
        std::string key;
        std::string val;
        if (line.type != kAttr || !splitAttribute(line.value, key, val) || key != "fingerprint") {
            continue;
        }
        auto tokens = split_spaces(val);
        if (tokens.size() < 2) {
            continue;
        }
        return Fingerprint{std::string(tokens[0]), std::string(tokens[1])};
    }
    return std::nullopt;
}

std::optional<std::string> attribute_in(const std::vector<Line>& lines, std::string_view key) {
    for (const Line& line : lines) {
        std::string k;
        std::string v;
        if (line.type != kAttr) {
            continue;
        }
        splitAttribute(line.value, k, v);
        if (k == key) {
            return v;
        }
    }
    return std::nullopt;
}

std::vector<std::string> attributes_in(const std::vector<Line>& lines, std::string_view key) {
    std::vector<std::string> out;
    for (const Line& line : lines) {
        std::string k;
        std::string v;
        if (line.type != kAttr) {
            continue;
        }
        splitAttribute(line.value, k, v);
        if (k == key) {
            out.push_back(v);
        }
    }
    return out;
}

// --- Canonical ordering ----------------------------------------------------

// RFC 4566 field order for a scope. `m` is emitted separately so it is not in
// the media order string. Unknown types sort to the end, keeping input order.
int order_index(char type, bool sessionScope) {
    static constexpr std::string_view kSession = "vosiuepcbtrzka";
    static constexpr std::string_view kMedia = "icbka";
    std::string_view order = sessionScope ? kSession : kMedia;
    std::size_t pos = order.find(type);
    return pos == std::string_view::npos ? static_cast<int>(order.size()) : static_cast<int>(pos);
}

void stable_sort_by_rfc_order(std::vector<Line>& lines, bool sessionScope) {
    std::stable_sort(lines.begin(), lines.end(), [sessionScope](const Line& a, const Line& b) {
        return order_index(a.type, sessionScope) < order_index(b.type, sessionScope);
    });
}

bool has_line_type(const std::vector<Line>& lines, char type) {
    for (const Line& line : lines) {
        if (line.type == type) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- splitAttribute --------------------------------------------------------

bool splitAttribute(std::string_view lineValue, std::string& key, std::string& value) {
    std::size_t colon = lineValue.find(':');
    if (colon == std::string_view::npos) {
        key = std::string(lineValue);
        value.clear();
        return false;
    }
    key = std::string(lineValue.substr(0, colon));
    value = std::string(lineValue.substr(colon + 1));
    return true;
}

// --- parse -----------------------------------------------------------------

Result<Session> parse(std::string_view text) {
    return parse(text, ParseOptions{});
}

Result<Session> parse(std::string_view text, const ParseOptions& options) {
    if (text.size() > options.maxLen) {
        return Error{errors::TooLarge, "SDP input exceeds maxLen"};
    }

    Session session;
    session.crlf = text.find("\r\n") != std::string_view::npos;
    session.trailingNewline = !text.empty() && text.back() == '\n';

    std::size_t start = 0;
    std::size_t lineCount = 0;
    Media* current = nullptr;

    while (start < text.size()) {
        std::size_t nl = text.find('\n', start);
        std::string_view line = nl == std::string_view::npos ? text.substr(start) : text.substr(start, nl - start);
        bool hadCr = false;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
            hadCr = true;
        }
        start = nl == std::string_view::npos ? text.size() : nl + 1;

        if (line.empty()) {
            continue; // tolerate blank/terminator-only segments.
        }
        if (options.crlfOnly && !hadCr) {
            if (options.lenient) {
                session.warnings.emplace_back("bare LF line: " + std::string(line));
                continue;
            }
            return Error{errors::ParseFailed, "Bare LF line rejected in crlfOnly mode"};
        }
        if (++lineCount > options.maxLines) {
            return Error{errors::TooLarge, "SDP exceeds maxLines"};
        }

        if (line.size() < 2 || line[1] != '=') {
            if (options.lenient) {
                session.warnings.emplace_back("malformed line: " + std::string(line));
                continue;
            }
            return Error{errors::ParseFailed, "Malformed line (expected \"x=...\"): " + std::string(line)};
        }

        char type = line[0];
        std::string_view value = line.substr(2);

        if (type == 'm') {
            auto tokens = split_spaces(value);
            if (tokens.size() < 3) {
                if (!options.lenient) {
                    return Error{errors::ParseFailed, "Malformed m= line: " + std::string(line)};
                }
                session.warnings.emplace_back("short m= line: " + std::string(line));
            }
            if (session.media.size() >= options.maxMedia) {
                return Error{errors::TooLarge, "SDP exceeds maxMedia"};
            }
            Media m;
            if (!tokens.empty()) {
                m.kind = std::string(tokens[0]);
            }
            if (tokens.size() > 1) {
                m.port = std::string(tokens[1]);
            }
            if (tokens.size() > 2) {
                m.proto = std::string(tokens[2]);
            }
            for (std::size_t i = 3; i < tokens.size(); ++i) {
                m.formats.emplace_back(tokens[i]);
            }
            session.media.push_back(std::move(m));
            current = &session.media.back();
            continue;
        }

        Line stored{type, std::string(value)};
        if (current != nullptr) {
            current->lines.push_back(std::move(stored));
        } else {
            session.lines.push_back(std::move(stored));
        }
    }

    if (!options.lenient) {
        if (!has_line_type(session.lines, 'v') || !has_line_type(session.lines, 's')) {
            return Error{errors::NoSession, "SDP is missing required v= and/or s= lines"};
        }
    }

    return session;
}

// --- build -----------------------------------------------------------------

std::string build(const Session& session, const BuildOptions& options) {
    bool crlf = session.crlf;
    if (options.ending == LineEnding::Lf) {
        crlf = false;
    } else if (options.ending == LineEnding::Crlf) {
        crlf = true;
    }
    const std::string_view sep = crlf ? "\r\n" : "\n";

    auto append_line = [&](std::string& out, char type, std::string_view value) {
        out.push_back(type);
        out.push_back('=');
        out.append(value);
        out.append(sep);
    };

    std::string out;

    std::vector<Line> sessionLines = session.lines;
    if (options.canonical) {
        if (!has_line_type(sessionLines, 'v')) {
            sessionLines.insert(sessionLines.begin(), Line{'v', "0"});
        }
        if (!has_line_type(sessionLines, 's')) {
            sessionLines.push_back(Line{'s', "-"});
        }
        stable_sort_by_rfc_order(sessionLines, /*sessionScope=*/true);
    }
    for (const Line& line : sessionLines) {
        append_line(out, line.type, line.value);
    }

    for (const Media& media : session.media) {
        std::string mline = media.kind;
        mline.push_back(' ');
        mline.append(media.port);
        mline.push_back(' ');
        mline.append(media.proto);
        for (const std::string& fmt : media.formats) {
            mline.push_back(' ');
            mline.append(fmt);
        }
        append_line(out, 'm', mline);

        std::vector<Line> mediaLines = media.lines;
        if (options.canonical) {
            stable_sort_by_rfc_order(mediaLines, /*sessionScope=*/false);
        }
        for (const Line& line : mediaLines) {
            append_line(out, line.type, line.value);
        }
    }

    // Match the input's trailing-terminator presence for an exact round-trip.
    // Canonical output always ends with a terminator.
    if (!options.canonical && !session.trailingNewline && out.size() >= sep.size()) {
        out.resize(out.size() - sep.size());
    }

    return out;
}

// --- Typed views -----------------------------------------------------------

std::vector<Rtpmap> rtpmaps(const Media& media) {
    std::vector<Rtpmap> out;
    for (const Line& line : media.lines) {
        std::string key;
        std::string val;
        if (line.type != kAttr || !splitAttribute(line.value, key, val) || key != "rtpmap") {
            continue;
        }
        auto tokens = split_spaces(val);
        if (tokens.size() < 2) {
            continue;
        }
        Rtpmap map;
        map.payload = to_int_or(tokens[0], -1);
        // tokens[1] is "codec/clock[/channels]".
        std::string_view enc = tokens[1];
        std::size_t s1 = enc.find('/');
        if (s1 == std::string_view::npos) {
            map.codec = std::string(enc);
        } else {
            map.codec = std::string(enc.substr(0, s1));
            std::string_view rest = enc.substr(s1 + 1);
            std::size_t s2 = rest.find('/');
            if (s2 == std::string_view::npos) {
                map.clock = to_int_or(rest, 0);
            } else {
                map.clock = to_int_or(rest.substr(0, s2), 0);
                map.channels = to_int_or(rest.substr(s2 + 1), 0);
            }
        }
        out.push_back(std::move(map));
    }
    return out;
}

std::vector<Fmtp> fmtps(const Media& media) {
    std::vector<Fmtp> out;
    for (const Line& line : media.lines) {
        std::string key;
        std::string val;
        if (line.type != kAttr || !splitAttribute(line.value, key, val) || key != "fmtp") {
            continue;
        }
        std::size_t sp = val.find(' ');
        Fmtp fmtp;
        if (sp == std::string::npos) {
            fmtp.payload = to_int_or(val, -1);
        } else {
            fmtp.payload = to_int_or(std::string_view(val).substr(0, sp), -1);
            fmtp.params = val.substr(sp + 1);
        }
        out.push_back(std::move(fmtp));
    }
    return out;
}

std::vector<Candidate> candidates(const Media& media) {
    std::vector<Candidate> out;
    for (const Line& line : media.lines) {
        std::string key;
        std::string val;
        if (line.type != kAttr || !splitAttribute(line.value, key, val) || key != "candidate") {
            continue;
        }
        auto t = split_spaces(val);
        if (t.size() < 6) {
            continue;
        }
        Candidate c;
        c.foundation = std::string(t[0]);
        c.component = to_int_or(t[1], 0);
        c.proto = std::string(t[2]);
        int64_t prio = 0;
        parse_int(t[3], prio);
        c.priority = prio;
        c.ip = std::string(t[4]);
        c.port = to_int_or(t[5], 0);
        // Remaining tokens are "typ <type>" then key/value extension pairs.
        std::size_t i = 6;
        if (i + 1 < t.size() && t[i] == "typ") {
            c.type = std::string(t[i + 1]);
            i += 2;
        }
        for (; i + 1 < t.size(); i += 2) {
            c.ext.emplace_back(std::string(t[i]), std::string(t[i + 1]));
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<ExtMap> extmaps(const Media& media) {
    std::vector<ExtMap> out;
    for (const Line& line : media.lines) {
        std::string key;
        std::string val;
        if (line.type != kAttr || !splitAttribute(line.value, key, val) || key != "extmap") {
            continue;
        }
        auto t = split_spaces(val);
        if (t.empty()) {
            continue;
        }
        ExtMap ext;
        std::string_view idField = t[0];
        std::size_t slash = idField.find('/');
        if (slash == std::string_view::npos) {
            ext.id = to_int_or(idField, 0);
        } else {
            ext.id = to_int_or(idField.substr(0, slash), 0);
            ext.direction = std::string(idField.substr(slash + 1));
        }
        if (t.size() > 1) {
            ext.uri = std::string(t[1]);
        }
        out.push_back(std::move(ext));
    }
    return out;
}

std::vector<Ssrc> ssrcs(const Media& media) {
    std::vector<Ssrc> out;
    for (const Line& line : media.lines) {
        std::string key;
        std::string val;
        if (line.type != kAttr || !splitAttribute(line.value, key, val) || key != "ssrc") {
            continue;
        }
        std::size_t sp = val.find(' ');
        Ssrc ssrc;
        std::string_view idField = sp == std::string::npos ? std::string_view(val) : std::string_view(val).substr(0, sp);
        int64_t id = 0;
        parse_int(idField, id);
        ssrc.id = static_cast<uint32_t>(id);
        if (sp != std::string::npos) {
            std::string_view rest = std::string_view(val).substr(sp + 1);
            std::size_t colon = rest.find(':');
            if (colon == std::string_view::npos) {
                ssrc.attribute = std::string(rest);
            } else {
                ssrc.attribute = std::string(rest.substr(0, colon));
                ssrc.value = std::string(rest.substr(colon + 1));
            }
        }
        out.push_back(std::move(ssrc));
    }
    return out;
}

std::optional<Fingerprint> fingerprint(const Media& media) {
    if (auto fp = fingerprint_in(media.lines)) {
        return fp;
    }
    return std::nullopt;
}

std::optional<Fingerprint> fingerprint(const Session& session) {
    return fingerprint_in(session.lines);
}

std::optional<std::string> attribute(const Media& media, std::string_view key) {
    return attribute_in(media.lines, key);
}

std::optional<std::string> attribute(const Session& session, std::string_view key) {
    return attribute_in(session.lines, key);
}

std::vector<std::string> attributes(const Media& media, std::string_view key) {
    return attributes_in(media.lines, key);
}

std::vector<std::string> attributes(const Session& session, std::string_view key) {
    return attributes_in(session.lines, key);
}

// --- Munging ---------------------------------------------------------------

namespace {

void set_attribute_lines(std::vector<Line>& lines, std::string_view key, std::string_view value, bool flag) {
    std::string rendered = std::string(key);
    if (!flag) {
        rendered.push_back(':');
        rendered.append(value);
    }
    for (Line& line : lines) {
        if (attribute_key_is(line, key)) {
            line.value = rendered;
            return;
        }
    }
    lines.push_back(Line{kAttr, std::move(rendered)});
}

std::size_t remove_attribute_lines(std::vector<Line>& lines, std::string_view key) {
    std::size_t before = lines.size();
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                    [key](const Line& line) { return attribute_key_is(line, key); }),
        lines.end());
    return before - lines.size();
}

} // namespace

void setAttribute(Media& media, std::string_view key, std::string_view value) {
    set_attribute_lines(media.lines, key, value, /*flag=*/false);
}
void setAttribute(Session& session, std::string_view key, std::string_view value) {
    set_attribute_lines(session.lines, key, value, /*flag=*/false);
}
void setFlag(Media& media, std::string_view key) {
    set_attribute_lines(media.lines, key, {}, /*flag=*/true);
}
void setFlag(Session& session, std::string_view key) {
    set_attribute_lines(session.lines, key, {}, /*flag=*/true);
}
void addAttribute(Media& media, std::string_view rawValue) {
    media.lines.push_back(Line{kAttr, std::string(rawValue)});
}
void addAttribute(Session& session, std::string_view rawValue) {
    session.lines.push_back(Line{kAttr, std::string(rawValue)});
}
std::size_t removeAttribute(Media& media, std::string_view key) {
    return remove_attribute_lines(media.lines, key);
}
std::size_t removeAttribute(Session& session, std::string_view key) {
    return remove_attribute_lines(session.lines, key);
}

namespace {

// Payload type referenced by the given attribute value, or -1 / wildcard(-2).
int payload_of_attribute(std::string_view key, std::string_view val) {
    if (key != "rtpmap" && key != "fmtp" && key != "rtcp-fb") {
        return -1;
    }
    std::size_t sp = val.find(' ');
    std::string_view first = sp == std::string_view::npos ? val : val.substr(0, sp);
    if (first == "*") {
        return -2; // wildcard: never dropped.
    }
    int64_t pt = 0;
    return parse_int(first, pt) ? static_cast<int>(pt) : -1;
}

} // namespace

void keepPayloads(Media& media, const std::vector<int>& payloads) {
    auto keep = [&](int pt) {
        return std::find(payloads.begin(), payloads.end(), pt) != payloads.end();
    };

    // Filter the m= format list, preserving original order.
    std::vector<std::string> formats;
    for (const std::string& fmt : media.formats) {
        int64_t pt = 0;
        if (parse_int(fmt, pt) && keep(static_cast<int>(pt))) {
            formats.push_back(fmt);
        }
    }
    media.formats = std::move(formats);

    // Drop rtpmap/fmtp/rtcp-fb lines whose payload is no longer kept.
    media.lines.erase(std::remove_if(media.lines.begin(), media.lines.end(),
                          [&](const Line& line) {
                              if (line.type != kAttr) {
                                  return false;
                              }
                              std::string key;
                              std::string val;
                              splitAttribute(line.value, key, val);
                              int pt = payload_of_attribute(key, val);
                              if (pt < 0) {
                                  return false; // not payload-scoped or wildcard.
                              }
                              return !keep(pt);
                          }),
        media.lines.end());
}

void reorderPayloads(Media& media, const std::vector<int>& order) {
    std::vector<std::string> result;
    result.reserve(media.formats.size());
    for (int pt : order) {
        std::string want = std::to_string(pt);
        if (std::find(media.formats.begin(), media.formats.end(), want) != media.formats.end()
            && std::find(result.begin(), result.end(), want) == result.end()) {
            result.push_back(want);
        }
    }
    for (const std::string& fmt : media.formats) {
        if (std::find(result.begin(), result.end(), fmt) == result.end()) {
            result.push_back(fmt);
        }
    }
    media.formats = std::move(result);
}

// --- equalNormalized -------------------------------------------------------

namespace {

std::vector<std::string> sorted_line_keys(const std::vector<Line>& lines) {
    std::vector<std::string> keys;
    keys.reserve(lines.size());
    for (const Line& line : lines) {
        std::string k(1, line.type);
        k.push_back('=');
        k.append(line.value);
        keys.push_back(std::move(k));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace

bool equalNormalized(const Session& a, const Session& b) {
    if (sorted_line_keys(a.lines) != sorted_line_keys(b.lines)) {
        return false;
    }
    if (a.media.size() != b.media.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.media.size(); ++i) {
        const Media& ma = a.media[i];
        const Media& mb = b.media[i];
        if (ma.kind != mb.kind || ma.port != mb.port || ma.proto != mb.proto || ma.formats != mb.formats) {
            return false;
        }
        if (sorted_line_keys(ma.lines) != sorted_line_keys(mb.lines)) {
            return false;
        }
    }
    return true;
}

} // namespace wl2::sdp
