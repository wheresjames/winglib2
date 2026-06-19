// Process-wide windowless GL context for the wl2:3d GPU renderer.
//
// The CPU and GPU renderers are both driven from the JS thread (no render
// worker — see start_render_thread), so one windowless EGL context, created
// lazily on that thread and kept current, serves every scene's MagnumRenderer.
// Kept separate from wl2_3d_gpu_render.h so the GL-object renderer stays
// independent of context/EGL setup (the headless probe manages its own context).
//
// Compiles to nothing unless WL2_3D_HAVE_MAGNUM is set.
#pragma once

#if WL2_3D_HAVE_MAGNUM

#include <Magnum/GL/Context.h>
#include <Magnum/Platform/GLContext.h>
#include <Magnum/Platform/WindowlessEglApplication.h>

#include <exception>
#include <iostream>
#include <string>

namespace wl2::three_d {

inline void gpu_log_fallback_once(const std::string& reason) {
    static bool logged = false;
    if (logged) {
        return;
    }
    logged = true;
    std::cerr << "wl2:3d GPU renderer unavailable; using CPU renderer";
    if (!reason.empty()) {
        std::cerr << ": " << reason;
    }
    std::cerr << '\n';
}

// Lazily create a windowless GL context on the calling thread and make it
// current; returns false if none can be created (no EGL/GPU), so callers fall
// back to the CPU renderer instead of aborting. The context objects are leaked
// intentionally to avoid teardown-ordering hazards at process exit. Single-thread
// (JS-thread) use only.
inline bool gpu_context_available() {
    static int state = -1;  // -1 untried, 0 unavailable, 1 ready
    static Magnum::Platform::WindowlessEglContext* eglContext = nullptr;
    if (state == 1) {
        if (!eglContext->makeCurrent()) {  // ensure current (usually already is)
            state = 0;
            gpu_log_fallback_once("existing EGL context could not be made current");
            return false;
        }
        return true;
    }
    if (state == 0) {
        return false;
    }
    try {
        state = 0;
        auto* egl =
            new Magnum::Platform::WindowlessEglContext{Magnum::Platform::WindowlessEglContext::Configuration{}};
        if (!egl->makeCurrent()) {
            delete egl;
            gpu_log_fallback_once("windowless EGL context could not be made current");
            return false;
        }
        // `--magnum-log quiet` suppresses Magnum's GL feature/workaround banner so the
        // library stays silent on stdout (the renderer string is surfaced explicitly
        // via gpu_device_info() / the `wl2 graphics` command instead).
        const char* magnumArgs[] = {"wl2", "--magnum-log", "quiet"};
        auto* magnum = new Magnum::Platform::GLContext{Magnum::NoCreate, 3, magnumArgs};
        if (!magnum->tryCreate()) {
            delete magnum;
            delete egl;
            gpu_log_fallback_once("Magnum GL context could not be created");
            return false;
        }
        eglContext = egl;
        state = 1;
        return true;
    } catch (const std::exception& e) {
        gpu_log_fallback_once(e.what());
        state = 0;
        return false;
    } catch (...) {
        gpu_log_fallback_once("unknown GL context error");
        state = 0;
        return false;
    }
}

// GL device/version strings for diagnostics. Valid only once gpu_context_available()
// has returned true (a context must be current).
struct GpuDeviceInfo {
    std::string device;
    std::string glVersion;
};
inline GpuDeviceInfo gpu_device_info() {
    return {Magnum::GL::Context::current().rendererString(),
            Magnum::GL::Context::current().versionString()};
}

}  // namespace wl2::three_d

#endif  // WL2_3D_HAVE_MAGNUM
