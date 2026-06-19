// slint_offscreen.h — a headless Slint platform for UI-on-3D.
//
// Slint's default backend opens an on-screen window; to map a UI onto a 3D
// surface we instead render a component into a CPU buffer (then a FrameRing).
// This installs a custom slint::platform::Platform whose WindowAdapter owns a
// SoftwareRenderer, the documented "bring your own renderer" path.
//
// The platform is process-global and one-shot (slint::platform::set_platform),
// so off-screen mode is mutually exclusive with the windowed backend used by
// Component.run()/show(). Each wl2 script runs in its own process, so a script
// picks one mode; ensure_offscreen_platform() reports failure if the windowed
// backend is already live instead of corrupting it.
#pragma once

#if WL2_HAVE_QUICKJS

#include <slint-platform.h>
#include <slint.h>

#include <cstdint>
#include <memory>

namespace wl2_slint_offscreen {

// A window backed by a SoftwareRenderer that draws into a caller-provided RGB8
// buffer. Size is fixed at creation (the negotiated off-screen viewport).
class OffscreenWindowAdapter : public slint::platform::WindowAdapter {
public:
    OffscreenWindowAdapter(uint32_t width, uint32_t height)
        : m_renderer(slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer),
          m_size{{width, height}} {}

    slint::platform::AbstractRenderer& renderer() override { return m_renderer; }
    slint::PhysicalSize size() override { return m_size; }
    void set_size(slint::PhysicalSize size) { m_size = size; }

    slint::platform::SoftwareRenderer& software_renderer() { return m_renderer; }

private:
    slint::platform::SoftwareRenderer m_renderer;
    slint::PhysicalSize m_size;
};

// Single-process registry: hands the just-created adapter back to the caller
// (create_window_adapter has no parameters) and tracks one-shot install state.
// Everything runs on the JS/main thread, so no locking is needed.
struct Registry {
    bool installed = false;
    bool failed = false;
    uint32_t pendingWidth = 0;
    uint32_t pendingHeight = 0;
    OffscreenWindowAdapter* lastAdapter = nullptr;
};

inline Registry& registry() {
    static Registry instance;
    return instance;
}

class OffscreenPlatform : public slint::platform::Platform {
public:
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        Registry& reg = registry();
        auto adapter = std::make_unique<OffscreenWindowAdapter>(reg.pendingWidth, reg.pendingHeight);
        reg.lastAdapter = adapter.get();
        return adapter;
    }
};

// Install the off-screen platform if it is not already active. Returns false if
// a platform was already set by the windowed backend (mutually exclusive), or if
// a prior install attempt failed.
inline bool ensure_offscreen_platform() {
    Registry& reg = registry();
    if (reg.installed) {
        return true;
    }
    if (reg.failed) {
        return false;
    }
    try {
        slint::platform::set_platform(std::make_unique<OffscreenPlatform>());
    } catch (...) {
        reg.failed = true;
        return false;
    }
    reg.installed = true;
    return true;
}

}  // namespace wl2_slint_offscreen

#endif  // WL2_HAVE_QUICKJS
