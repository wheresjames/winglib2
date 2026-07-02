#include "wl2_media/schema.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace wl2::media {

namespace {

// ---------------------------------------------------------------------------
// Minimal flat-object JSON writer/reader
//
// The media schemas are flat objects of strings, integers, and booleans, so a
// small purpose-built serializer/parser avoids pulling in a JSON dependency.
// Nested structures are not used; `sideData` is carried as an opaque string.
// ---------------------------------------------------------------------------

void append_escaped(std::string& out, std::string_view value) {
    out.push_back('"');
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0xf]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0xf]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void append_string_field(std::string& out, bool& first, std::string_view key, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_escaped(out, key);
    out.push_back(':');
    append_escaped(out, value);
}

void append_int_field(std::string& out, bool& first, std::string_view key, long long value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_escaped(out, key);
    out.push_back(':');
    out += std::to_string(value);
}

void append_bool_field(std::string& out, bool& first, std::string_view key, bool value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_escaped(out, key);
    out.push_back(':');
    out += value ? "true" : "false";
}

struct JsonField {
    enum class Kind { String, Number, Bool, Null } kind = Kind::Null;
    std::string str;
    long long num = 0;
    bool boolean = false;
};

// Encode a Unicode code point as UTF-8 into out.
void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

void skip_ws(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

bool parse_string(std::string_view s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (i >= s.size()) {
                return false;
            }
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) {
                        return false;
                    }
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') {
                            cp |= static_cast<unsigned int>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            cp |= static_cast<unsigned int>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            cp |= static_cast<unsigned int>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool parse_value(std::string_view s, size_t& i, JsonField& out) {
    skip_ws(s, i);
    if (i >= s.size()) {
        return false;
    }
    char c = s[i];
    if (c == '"') {
        out.kind = JsonField::Kind::String;
        return parse_string(s, i, out.str);
    }
    if (c == 't' || c == 'f') {
        std::string_view lit = c == 't' ? "true" : "false";
        if (s.substr(i, lit.size()) != lit) {
            return false;
        }
        i += lit.size();
        out.kind = JsonField::Kind::Bool;
        out.boolean = c == 't';
        return true;
    }
    if (c == 'n') {
        if (s.substr(i, 4) != "null") {
            return false;
        }
        i += 4;
        out.kind = JsonField::Kind::Null;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = i;
        if (s[i] == '-') {
            ++i;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        // Integers only; a fractional part is not part of the schema.
        std::string token(s.substr(start, i - start));
        if (token.empty() || token == "-") {
            return false;
        }
        out.kind = JsonField::Kind::Number;
        out.num = std::strtoll(token.c_str(), nullptr, 10);
        return true;
    }
    return false;
}

bool parse_flat_object(std::string_view s, std::map<std::string, JsonField>& out) {
    size_t i = 0;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') {
        return false;
    }
    ++i;
    skip_ws(s, i);
    if (i < s.size() && s[i] == '}') {
        return true;
    }
    while (i < s.size()) {
        skip_ws(s, i);
        std::string key;
        if (!parse_string(s, i, key)) {
            return false;
        }
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') {
            return false;
        }
        ++i;
        JsonField field;
        if (!parse_value(s, i, field)) {
            return false;
        }
        out[key] = std::move(field);
        skip_ws(s, i);
        if (i >= s.size()) {
            return false;
        }
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] == '}') {
            return true;
        }
        return false;
    }
    return false;
}

const JsonField* find(const std::map<std::string, JsonField>& fields, const char* key) {
    auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
}

std::string field_string(const std::map<std::string, JsonField>& fields, const char* key, std::string fallback = {}) {
    const JsonField* f = find(fields, key);
    if (f && f->kind == JsonField::Kind::String) {
        return f->str;
    }
    return fallback;
}

long long field_int(const std::map<std::string, JsonField>& fields, const char* key, long long fallback = 0) {
    const JsonField* f = find(fields, key);
    if (f && f->kind == JsonField::Kind::Number) {
        return f->num;
    }
    return fallback;
}

bool field_bool(const std::map<std::string, JsonField>& fields, const char* key, bool fallback = false) {
    const JsonField* f = find(fields, key);
    if (f && f->kind == JsonField::Kind::Bool) {
        return f->boolean;
    }
    return fallback;
}

} // namespace

bool isKnownMediaType(std::string_view mediaType) noexcept {
    return mediaType == "video" || mediaType == "audio" || mediaType == "subtitle" || mediaType == "data";
}

bool isKnownBackpressureProfile(std::string_view name) noexcept {
    return name == backpressure::Record || name == backpressure::Transcode
        || name == backpressure::Preview || name == backpressure::Relay;
}

bool parseTimeBase(std::string_view timeBase, int64_t& num, int64_t& den) noexcept {
    size_t slash = timeBase.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= timeBase.size()) {
        return false;
    }
    auto to_int = [](std::string_view text, int64_t& value) -> bool {
        if (text.empty()) {
            return false;
        }
        size_t start = 0;
        bool negative = false;
        if (text[0] == '-') {
            negative = true;
            start = 1;
        }
        if (start >= text.size()) {
            return false;
        }
        int64_t acc = 0;
        for (size_t k = start; k < text.size(); ++k) {
            if (text[k] < '0' || text[k] > '9') {
                return false;
            }
            acc = acc * 10 + (text[k] - '0');
        }
        value = negative ? -acc : acc;
        return true;
    };
    int64_t n = 0;
    int64_t d = 0;
    if (!to_int(timeBase.substr(0, slash), n) || !to_int(timeBase.substr(slash + 1), d)) {
        return false;
    }
    if (d == 0) {
        return false;
    }
    num = n;
    den = d;
    return true;
}

int64_t convertTimestamp(int64_t value, std::string_view fromTimeBase, std::string_view toTimeBase) noexcept {
    int64_t fn = 0;
    int64_t fd = 0;
    int64_t tn = 0;
    int64_t td = 0;
    if (!parseTimeBase(fromTimeBase, fn, fd) || !parseTimeBase(toTimeBase, tn, td)) {
        return value;
    }
    // result = value * (fn/fd) / (tn/td) = value * fn * td / (fd * tn)
    __int128 numerator = static_cast<__int128>(value) * fn * td;
    __int128 denominator = static_cast<__int128>(fd) * tn;
    if (denominator == 0) {
        return value;
    }
    bool negative = (numerator < 0) ^ (denominator < 0);
    __int128 an = numerator < 0 ? -numerator : numerator;
    __int128 ad = denominator < 0 ? -denominator : denominator;
    __int128 q = an / ad;
    __int128 rem = an % ad;
    if (rem * 2 >= ad) {
        ++q;
    }
    int64_t result = static_cast<int64_t>(negative ? -q : q);
    return result;
}

Result<void> validate(const StreamDescriptor& descriptor) {
    if (descriptor.schema != kStreamSchema) {
        return Error{errors::InvalidSchema, "Unsupported stream descriptor schema version"};
    }
    if (!isKnownMediaType(descriptor.mediaType)) {
        return Error{errors::UnsupportedMediaType, "Unknown media type: " + descriptor.mediaType};
    }
    if (descriptor.track < 0) {
        return Error{errors::InvalidArgument, "Track id must be non-negative"};
    }
    return Result<void>{};
}

Result<void> validate(const PacketMetadata& metadata) {
    if (metadata.schema != kPacketSchema) {
        return Error{errors::InvalidSchema, "Unsupported packet metadata schema version"};
    }
    if (!metadata.mediaType.empty() && !isKnownMediaType(metadata.mediaType)) {
        return Error{errors::UnsupportedMediaType, "Unknown media type: " + metadata.mediaType};
    }
    if (metadata.track < 0) {
        return Error{errors::InvalidArgument, "Track id must be non-negative"};
    }
    int64_t num = 0;
    int64_t den = 0;
    if (!parseTimeBase(metadata.timeBase, num, den)) {
        return Error{errors::InvalidTimeBase, "Malformed time base: " + metadata.timeBase};
    }
    return Result<void>{};
}

std::string serialize(const StreamDescriptor& descriptor) {
    std::string out = "{";
    bool first = true;
    append_int_field(out, first, "schema", descriptor.schema);
    append_string_field(out, first, "mediaType", descriptor.mediaType);
    append_string_field(out, first, "codec", descriptor.codec);
    append_string_field(out, first, "caps", descriptor.caps);
    append_string_field(out, first, "streamFormat", descriptor.streamFormat);
    append_string_field(out, first, "alignment", descriptor.alignment);
    append_int_field(out, first, "track", static_cast<long long>(descriptor.track));
    out.push_back('}');
    return out;
}

std::string serialize(const PacketMetadata& metadata) {
    std::string out = "{";
    bool first = true;
    append_int_field(out, first, "schema", metadata.schema);
    append_string_field(out, first, "mediaType", metadata.mediaType);
    append_string_field(out, first, "codec", metadata.codec);
    append_string_field(out, first, "caps", metadata.caps);
    append_string_field(out, first, "streamFormat", metadata.streamFormat);
    append_string_field(out, first, "alignment", metadata.alignment);
    append_int_field(out, first, "track", static_cast<long long>(metadata.track));
    append_int_field(out, first, "pts", static_cast<long long>(metadata.pts));
    append_int_field(out, first, "dts", static_cast<long long>(metadata.dts));
    append_int_field(out, first, "duration", static_cast<long long>(metadata.duration));
    append_string_field(out, first, "timeBase", metadata.timeBase);
    append_int_field(out, first, "flags", static_cast<long long>(metadata.flags));
    append_bool_field(out, first, "discontinuity", metadata.discontinuity);
    append_string_field(out, first, "sideData", metadata.sideData);
    out.push_back('}');
    return out;
}

Result<StreamDescriptor> parseStreamDescriptor(std::string_view json) {
    std::map<std::string, JsonField> fields;
    if (!parse_flat_object(json, fields)) {
        return Error{errors::ParseFailed, "Malformed stream descriptor JSON"};
    }
    StreamDescriptor descriptor;
    descriptor.schema = static_cast<int>(field_int(fields, "schema", kStreamSchema));
    descriptor.mediaType = field_string(fields, "mediaType");
    descriptor.codec = field_string(fields, "codec");
    descriptor.caps = field_string(fields, "caps");
    descriptor.streamFormat = field_string(fields, "streamFormat");
    descriptor.alignment = field_string(fields, "alignment");
    descriptor.track = field_int(fields, "track", 0);
    if (auto ok = validate(descriptor); !ok) {
        return ok.error();
    }
    return descriptor;
}

Result<PacketMetadata> parsePacketMetadata(std::string_view json) {
    std::map<std::string, JsonField> fields;
    if (!parse_flat_object(json, fields)) {
        return Error{errors::ParseFailed, "Malformed packet metadata JSON"};
    }
    PacketMetadata metadata;
    metadata.schema = static_cast<int>(field_int(fields, "schema", kPacketSchema));
    metadata.mediaType = field_string(fields, "mediaType");
    metadata.codec = field_string(fields, "codec");
    metadata.caps = field_string(fields, "caps");
    metadata.streamFormat = field_string(fields, "streamFormat");
    metadata.alignment = field_string(fields, "alignment");
    metadata.track = field_int(fields, "track", 0);
    metadata.pts = field_int(fields, "pts", 0);
    metadata.dts = field_int(fields, "dts", 0);
    metadata.duration = field_int(fields, "duration", 0);
    metadata.timeBase = field_string(fields, "timeBase", kDefaultTimeBase);
    metadata.flags = static_cast<uint32_t>(field_int(fields, "flags", 0));
    metadata.discontinuity = field_bool(fields, "discontinuity", false);
    metadata.sideData = field_string(fields, "sideData");
    if (auto ok = validate(metadata); !ok) {
        return ok.error();
    }
    return metadata;
}

} // namespace wl2::media
