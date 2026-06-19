// Platform / transport abstraction for the wl2:3d WASM target (§15).
//
// Two seams keep a browser port a backend swap rather than a rewrite:
//   * GraphicsBackend — native EGL vs web WebGL2 context creation (declared
//     here; the renderer implements it behind the Magnum provider).
//   * FrameTransport — the FrameRing carrier: native libmembus `memvid` vs a web
//     backend (SharedArrayBuffer across Web Workers, or the single-threaded
//     in-memory fallback below). libmembus stays the native backend.
//
// This header has no native dependency, so it compiles for wasm32 and is the
// regression guard against native-only/WebGL2-incompatible code creeping into
// the portable core.
#pragma once

#include <cstdint>
#include <vector>

namespace wl2::three_d {

// Identifies the GPU context strategy a build targets. The renderer selects one;
// the portable core never assumes either.
enum class GraphicsBackend { None, NativeEgl, WebGl2 };

// Shared RGBA8 frame contract carried by every transport: premultiplied alpha,
// sRGB, top-left origin, explicit byte stride.
struct FrameDesc {
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;  // bytes per row; >= width * 4
};

// Abstract RGBA8 frame ring. The native binding uses libmembus directly today;
// this interface is the seam a web build implements with SharedArrayBuffer.
class FrameTransport {
public:
    virtual ~FrameTransport() = default;
    virtual bool create(const FrameDesc& desc, int ringSize) = 0;
    virtual uint8_t* writeSlot() = 0;
    virtual void commit() = 0;
    virtual int64_t sequence() const = 0;
    virtual const uint8_t* readSlot(int64_t* outSequence) const = 0;
    virtual FrameDesc desc() const = 0;
};

// Portable single-threaded in-memory ring: the web fallback and the basis for a
// SharedArrayBuffer-backed multi-worker variant. No native dependency.
class InMemoryFrameTransport : public FrameTransport {
public:
    bool create(const FrameDesc& desc, int ringSize) override {
        if (desc.width <= 0 || desc.height <= 0 || ringSize < 1) {
            return false;
        }
        desc_ = desc;
        if (desc_.stride < desc_.width * 4) {
            desc_.stride = desc_.width * 4;
        }
        const size_t frameBytes = static_cast<size_t>(desc_.stride) * desc_.height;
        ring_.assign(static_cast<size_t>(ringSize), std::vector<uint8_t>(frameBytes, 0));
        write_ = 0;
        committed_ = -1;
        sequence_ = 0;
        return true;
    }

    uint8_t* writeSlot() override { return ring_.empty() ? nullptr : ring_[write_].data(); }

    void commit() override {
        if (ring_.empty()) {
            return;
        }
        committed_ = write_;
        ++sequence_;
        write_ = (write_ + 1) % static_cast<int>(ring_.size());
    }

    int64_t sequence() const override { return sequence_; }

    const uint8_t* readSlot(int64_t* outSequence) const override {
        if (committed_ < 0) {
            return nullptr;
        }
        if (outSequence) {
            *outSequence = sequence_;
        }
        return ring_[static_cast<size_t>(committed_)].data();
    }

    FrameDesc desc() const override { return desc_; }

private:
    FrameDesc desc_;
    std::vector<std::vector<uint8_t>> ring_;
    int write_ = 0;
    int committed_ = -1;
    int64_t sequence_ = 0;
};

}  // namespace wl2::three_d
