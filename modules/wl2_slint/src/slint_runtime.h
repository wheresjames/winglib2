// slint_runtime.h — process-wide Slint runtime state.
//
// Slint has a process-wide event loop. This state serializes Component.run()
// calls and gives Runtime shutdown a way to quit an active Slint loop so the UI
// loop never outlives the wl2 Runtime.
#pragma once

#if WL2_HAVE_QUICKJS

#include <slint-interpreter.h>
#include <slint.h>

#include <mutex>

namespace wl2_slint_runtime {

// One process-wide state object. Created lazily and reached from a Runtime async
// shutdown hook. Component.run() owns the recurring slint::Timer that drains
// Runtime::async() while a window is up.
struct SlintRuntimeState {
    std::mutex mutex;
    // True while Component.run() is inside slint::run_event_loop().
    bool loopRunning = false;

    // Claim the event loop for run(). Returns false if a loop is already running
    // (the loop is process-wide, so only one run() may be active at a time).
    bool beginLoop() {
        std::lock_guard<std::mutex> lock(mutex);
        if (loopRunning) {
            return false;
        }
        loopRunning = true;
        return true;
    }

    // Release the event loop after run_event_loop() returns.
    void endLoop() {
        std::lock_guard<std::mutex> lock(mutex);
        loopRunning = false;
    }

    // Stop the Slint event loop if one is running. Safe to call from the
    // JS/main thread during runtime shutdown.
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex);
        if (loopRunning) {
            slint::quit_event_loop();
            loopRunning = false;
        }
    }
};

inline SlintRuntimeState& state() {
    static SlintRuntimeState instance;
    return instance;
}

}  // namespace wl2_slint_runtime

#endif  // WL2_HAVE_QUICKJS
