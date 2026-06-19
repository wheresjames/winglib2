// Versioned, typed detection records for the wl2:3d detections->scene workflow
// (§14.2). An external detector publishes image-space hits on a libmembus
// `memmsg` queue; this defines the wire encoding (a compact little-endian binary
// record) with a strict, bounds-checked decoder so an untrusted/garbage payload
// is rejected, never crashes (the fuzz target).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace wl2::three_d {

// Image-space coordinate convention for a detection's px/py.
enum class DetectionCoord : uint8_t {
    PixelTopLeft = 0,       // px in [0,imageWidth], py in [0,imageHeight]
    NormalizedTopLeft = 1,  // px,py in [0,1]
};

struct Detection {
    int32_t cameraId = 0;
    int32_t id = 0;
    int64_t sourceFrameSeq = -1;  // -1 = absent
    double ts = 0.0;
    double px = 0.0;
    double py = 0.0;
    int32_t imageWidth = 0;
    int32_t imageHeight = 0;
    float confidence = 0.0f;
    DetectionCoord coord = DetectionCoord::PixelTopLeft;
    bool hasBbox = false;
    float bbox[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // x, y, w, h
    std::string klass;
};

namespace detail {

constexpr char kDetectionMagic[4] = {'W', '3', 'D', 'D'};
constexpr uint16_t kDetectionSchemaVersion = 1;
constexpr uint16_t kDetectionMaxClassLen = 256;
constexpr int32_t kDetectionMaxImageDim = 1 << 16;  // 65536

constexpr uint16_t kFlagHasBbox = 1u << 0;
constexpr uint16_t kFlagHasSourceFrameSeq = 1u << 1;

template <typename T>
void put(std::string& out, T value) {
    char buf[sizeof(T)];
    std::memcpy(buf, &value, sizeof(T));
    out.append(buf, sizeof(T));
}

// Bounds-checked little-endian reader over an untrusted byte span.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    template <typename T>
    bool read(T& value) {
        if (pos_ + sizeof(T) > data_.size()) {
            return false;
        }
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    bool readBytes(size_t count, std::string& out) {
        if (pos_ + count > data_.size()) {
            return false;
        }
        out.assign(data_.data() + pos_, count);
        pos_ += count;
        return true;
    }

private:
    std::string_view data_;
    size_t pos_ = 0;
};

}  // namespace detail

inline std::string encodeDetection(const Detection& d) {
    using namespace detail;
    std::string out;
    out.append(kDetectionMagic, sizeof(kDetectionMagic));
    put<uint16_t>(out, kDetectionSchemaVersion);
    uint16_t flags = 0;
    if (d.hasBbox) flags |= kFlagHasBbox;
    if (d.sourceFrameSeq >= 0) flags |= kFlagHasSourceFrameSeq;
    put<uint16_t>(out, flags);
    put<int32_t>(out, d.cameraId);
    put<int32_t>(out, d.id);
    put<int64_t>(out, d.sourceFrameSeq);
    put<double>(out, d.ts);
    put<double>(out, d.px);
    put<double>(out, d.py);
    put<int32_t>(out, d.imageWidth);
    put<int32_t>(out, d.imageHeight);
    put<float>(out, d.confidence);
    put<uint8_t>(out, static_cast<uint8_t>(d.coord));
    for (float b : d.bbox) {
        put<float>(out, b);
    }
    const uint16_t classLen = static_cast<uint16_t>(
        d.klass.size() > kDetectionMaxClassLen ? kDetectionMaxClassLen : d.klass.size());
    put<uint16_t>(out, classLen);
    out.append(d.klass.data(), classLen);
    return out;
}

// Decode + validate. Returns nullopt for any malformed, truncated, or
// out-of-contract payload — every field is bounds- and sanity-checked.
inline std::optional<Detection> decodeDetection(std::string_view bytes) {
    using namespace detail;
    Reader reader(bytes);

    char magic[4];
    if (!reader.read(magic) || std::memcmp(magic, kDetectionMagic, sizeof(magic)) != 0) {
        return std::nullopt;
    }
    uint16_t version = 0;
    if (!reader.read(version) || version != kDetectionSchemaVersion) {
        return std::nullopt;
    }
    uint16_t flags = 0;
    if (!reader.read(flags)) {
        return std::nullopt;
    }

    Detection d;
    uint8_t coord = 0;
    if (!reader.read(d.cameraId) || !reader.read(d.id) || !reader.read(d.sourceFrameSeq) ||
        !reader.read(d.ts) || !reader.read(d.px) || !reader.read(d.py) ||
        !reader.read(d.imageWidth) || !reader.read(d.imageHeight) || !reader.read(d.confidence) ||
        !reader.read(coord)) {
        return std::nullopt;
    }
    for (float& b : d.bbox) {
        if (!reader.read(b)) {
            return std::nullopt;
        }
    }
    uint16_t classLen = 0;
    if (!reader.read(classLen) || classLen > kDetectionMaxClassLen) {
        return std::nullopt;
    }
    if (!reader.readBytes(classLen, d.klass)) {
        return std::nullopt;
    }

    if (coord > static_cast<uint8_t>(DetectionCoord::NormalizedTopLeft)) {
        return std::nullopt;
    }
    d.coord = static_cast<DetectionCoord>(coord);
    if ((flags & kFlagHasSourceFrameSeq) == 0) {
        d.sourceFrameSeq = -1;
    }
    d.hasBbox = (flags & kFlagHasBbox) != 0;

    // Contract checks: finite coordinates, sane image dimensions.
    if (!std::isfinite(d.px) || !std::isfinite(d.py) || !std::isfinite(d.ts) ||
        !std::isfinite(d.confidence)) {
        return std::nullopt;
    }
    if (d.imageWidth <= 0 || d.imageHeight <= 0 || d.imageWidth > kDetectionMaxImageDim ||
        d.imageHeight > kDetectionMaxImageDim) {
        return std::nullopt;
    }
    return d;
}

// Map a detection's image-space hit to the camera viewport (top-left origin),
// reconciling the detector image size with the camera frame size (§14.2).
inline void detectionToViewport(const Detection& d, double viewportWidth, double viewportHeight,
                                double& outX, double& outY) {
    double normX = 0.0;
    double normY = 0.0;
    if (d.coord == DetectionCoord::NormalizedTopLeft) {
        normX = d.px;
        normY = d.py;
    } else {
        normX = d.imageWidth > 0 ? d.px / d.imageWidth : 0.0;
        normY = d.imageHeight > 0 ? d.py / d.imageHeight : 0.0;
    }
    outX = normX * viewportWidth;
    outY = normY * viewportHeight;
}

}  // namespace wl2::three_d
